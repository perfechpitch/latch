#ifndef _LATCH_BACH_KSIM_LOADER_
#define _LATCH_BACH_KSIM_LOADER_

// kernel 文件的读入。一行一条记录，空格分隔，地址十六进制带前缀，其余十进制。
//
//   BACHK <version>
//   META  <key> <value，吃到行尾>
//   CORE  <core_id>                                   往下是这个核的 kernel
//   LAYER <i>                                         往下是第 i 层
//   GEMM  <m> <k> <n> <a> <b> <c> <dtype> <cycles>
//   ELEM  <op> <n> <dst> <limit> <cycles> <src>...    源地址变长，放最后
//   SEND  <src> <length> <dst_core> <port>
//   RECV  <port> <dst> <length>
//   FEED  <cycle> <core> <port> <length>
//   DRAIN <core> <port> <length>
//
// 遇到认不出的记录标签、认不出的 dtype 或算子名，一律当错误停下。静默跳过一条不认
// 识的记录，等于跑的不是编译出来的那份东西。META 的 key 例外，它不影响执行。
//
// 两个入口：TryLoadKernels 把错误写进字符串返回 false，供测试覆盖各条拒绝路径；
// LoadKernels 直接在错误上停机，供装配路径用。

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "spdlog/spdlog.h"

#include "base/log.h"
#include "bach/ksim/kernel.h"

namespace latch {
namespace bach {
namespace ksim {

namespace detail {

inline bool ParseU64(std::string const& s, uint64_t* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const uint64_t v = std::strtoull(s.c_str(), &end, 0);
  if (end == nullptr || *end != '\0') return false;
  *out = v;
  return true;
}

inline bool ParseI64(std::string const& s, int64_t* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const int64_t v = std::strtoll(s.c_str(), &end, 0);
  if (end == nullptr || *end != '\0') return false;
  *out = v;
  return true;
}

inline bool ParseDType(std::string const& s, DType* out) {
  if (s == "fp4") { *out = DType::kFp4; return true; }
  if (s == "fp8") { *out = DType::kFp8; return true; }
  if (s == "bf16" || s == "bfloat16") { *out = DType::kBf16; return true; }
  if (s == "fp32" || s == "float32") { *out = DType::kFp32; return true; }
  return false;
}

inline bool ParseElemOp(std::string const& s, ElemOp* out) {
  if (s == "swiglu") { *out = ElemOp::kSwiglu; return true; }
  if (s == "add") { *out = ElemOp::kAdd; return true; }
  if (s == "mul") { *out = ElemOp::kMul; return true; }
  if (s == "silu") { *out = ElemOp::kSilu; return true; }
  return false;
}

}  // namespace detail

inline bool TryLoadKernels(std::string const& path, KernelImage* image,
                           std::string* err) {
  std::ifstream in(path);
  if (!in.is_open()) {
    *err = "打不开 " + path;
    return false;
  }

  CoreKernel* cur = nullptr;
  uint32_t layer = 0;
  uint64_t lineno = 0;
  std::string line;

  auto fail = [&](std::string const& what) {
    *err = "第 " + std::to_string(lineno) + " 行：" + what;
    return false;
  };

  while (std::getline(in, line)) {
    ++lineno;
    std::istringstream ss(line);
    std::vector<std::string> f;
    std::string tok;
    while (ss >> tok) f.push_back(tok);
    if (f.empty()) continue;

    const std::string& tag = f[0];

    if (tag == "BACHK") {
      if (f.size() != 2) return fail("BACHK 要一个版本号");
      continue;
    }
    if (tag == "META") {
      if (f.size() < 3) return fail("META 要 key 与取值");
      std::string v = f[2];
      for (std::size_t i = 3; i < f.size(); ++i) v += " " + f[i];
      image->meta[f[1]] = v;
      continue;
    }
    if (tag == "CORE") {
      uint64_t id = 0;
      if (f.size() != 2 || !detail::ParseU64(f[1], &id))
        return fail("CORE 要一个核号");
      image->cores.push_back(CoreKernel{id, {}});
      cur = &image->cores.back();
      layer = 0;
      continue;
    }
    if (tag == "LAYER") {
      uint64_t v = 0;
      if (f.size() != 2 || !detail::ParseU64(f[1], &v))
        return fail("LAYER 要一个层号");
      layer = static_cast<uint32_t>(v);
      continue;
    }
    if (tag == "FEED") {
      uint64_t a = 0, b = 0, c = 0, d = 0;
      if (f.size() != 5 || !detail::ParseU64(f[1], &a) ||
          !detail::ParseU64(f[2], &b) || !detail::ParseU64(f[3], &c) ||
          !detail::ParseU64(f[4], &d))
        return fail("FEED 要 cycle core port length");
      image->feeds.push_back(Feed{a, b, static_cast<uint32_t>(c), d});
      continue;
    }
    if (tag == "DRAIN") {
      uint64_t a = 0, b = 0, c = 0;
      if (f.size() != 4 || !detail::ParseU64(f[1], &a) ||
          !detail::ParseU64(f[2], &b) || !detail::ParseU64(f[3], &c))
        return fail("DRAIN 要 core port length");
      image->drains.push_back(Drain{a, static_cast<uint32_t>(b), c});
      continue;
    }

    if (tag != "GEMM" && tag != "ELEM" && tag != "SEND" && tag != "RECV")
      return fail("认不出的记录 " + tag);
    if (cur == nullptr) return fail(tag + " 出现在任何 CORE 之前");

    KernelOp op;
    op.layer = layer;

    if (tag == "GEMM") {
      if (f.size() != 9) return fail("GEMM 要 m k n a b c dtype cycles");
      if (!detail::ParseU64(f[1], &op.m) || !detail::ParseU64(f[2], &op.k) ||
          !detail::ParseU64(f[3], &op.n) || !detail::ParseU64(f[4], &op.a) ||
          !detail::ParseU64(f[5], &op.b) || !detail::ParseU64(f[6], &op.c) ||
          !detail::ParseU64(f[8], &op.cycles))
        return fail("GEMM 的取值不是整数");
      if (!detail::ParseDType(f[7], &op.dtype))
        return fail("认不出的 dtype " + f[7]);
      op.kind = OpKind::kGemm;
    } else if (tag == "ELEM") {
      if (f.size() < 7) return fail("ELEM 要 op n dst limit cycles 与至少一个源");
      if (!detail::ParseElemOp(f[1], &op.elem))
        return fail("认不出的算子 " + f[1]);
      if (!detail::ParseU64(f[2], &op.n) || !detail::ParseU64(f[3], &op.dst) ||
          !detail::ParseU64(f[5], &op.cycles))
        return fail("ELEM 的取值不是整数");
      op.limit = std::strtod(f[4].c_str(), nullptr);
      for (std::size_t i = 6; i < f.size(); ++i) {
        uint64_t s = 0;
        if (!detail::ParseU64(f[i], &s)) return fail("ELEM 的源地址不是整数");
        op.srcs.push_back(s);
      }
      op.kind = OpKind::kElem;
    } else if (tag == "SEND") {
      uint64_t p = 0;
      if (f.size() != 5) return fail("SEND 要 src length dst_core port");
      if (!detail::ParseU64(f[1], &op.src) ||
          !detail::ParseU64(f[2], &op.length) ||
          !detail::ParseI64(f[3], &op.dst_core) || !detail::ParseU64(f[4], &p))
        return fail("SEND 的取值不是整数");
      op.port = static_cast<uint32_t>(p);
      op.kind = OpKind::kSend;
    } else {
      uint64_t p = 0;
      if (f.size() != 4) return fail("RECV 要 port dst length");
      if (!detail::ParseU64(f[1], &p) || !detail::ParseU64(f[2], &op.dst) ||
          !detail::ParseU64(f[3], &op.length))
        return fail("RECV 的取值不是整数");
      op.port = static_cast<uint32_t>(p);
      op.kind = OpKind::kRecv;
    }
    cur->ops.push_back(std::move(op));
  }

  if (image->cores.empty()) return fail("一条 CORE 记录都没有");
  return true;
}

inline KernelImage LoadKernels(std::string const& path) {
  KernelImage image;
  std::string err;
  if (!TryLoadKernels(path, &image, &err)) {
    spdlog::error("读 kernel 文件失败：{}", err);
    LOGCHECK(false, "LoadKernels failed, see the error above.");
  }
  return image;
}

}
}
}

#endif
