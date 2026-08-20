#ifndef _LATCH_BACH_KSIM_KERNEL_
#define _LATCH_BACH_KSIM_KERNEL_

// 一个核要执行的 kernel 序列。它是模型编译的产物，构造期读入，运行期只读。
//
// 一条 kernel 就是一个算子，带上操作数在本核 SRAM 里的地址和形状。核自己决定怎么
// 分块取数，所以这里不写循环，只写整条的形状。地址由编译器静态排好，权重常驻。
//
// 字段的可读范围跟着 kind 走，读错位置会拿到一个有值但无意义的数：
//
//   Gemm  m k n 是形状，a b c 是三个操作数的地址，dtype 是 b 的位宽
//   Elem  n 是元素总数，srcs 是几个源地址，dst 是落点，limit 只有 swiglu 用
//   Send  src 取数的地址，length 字节数，dst_core 目标核，port 走哪个口
//   Recv  port 从哪个口收，dst 落点，length 字节数
//
// cycles 只有 Gemm 与 Elem 有意义：它是这条算子在核里算多少拍，来自实测表，查不到
// 时由编译器按权重加载量估。搬运要多久不在这里，由链路的带宽与排队算出来。
//
// layer 不参与执行，只用来把观测归到模型的哪一层上。

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "base/log.h"

namespace latch {
namespace bach {
namespace ksim {

enum class OpKind : uint32_t {
  kGemm = 0,
  kElem = 1,
  kSend = 2,
  kRecv = 3,
};

// 位宽而不是字节数：fp4 半个字节，用字节数表示不了。
enum class DType : uint32_t {
  kFp4 = 4,
  kFp8 = 8,
  kBf16 = 16,
  kFp32 = 32,
};

enum class ElemOp : uint32_t {
  kSwiglu = 0,
  kAdd = 1,
  kMul = 2,
  kSilu = 3,
};

inline uint64_t BitsOf(DType t) { return static_cast<uint64_t>(t); }

// 元素个数换算成字节数。位宽不足一个字节时按总位数向上取整，所以 fp4 的奇数个元素
// 也算得出来。
inline uint64_t BytesOf(uint64_t elems, DType t) {
  return (elems * BitsOf(t) + 7) / 8;
}

struct KernelOp {
  OpKind kind = OpKind::kGemm;
  uint32_t layer = 0;

  uint64_t m = 0;
  uint64_t k = 0;
  uint64_t n = 0;
  uint64_t a = 0;
  uint64_t b = 0;
  uint64_t c = 0;
  DType dtype = DType::kBf16;

  ElemOp elem = ElemOp::kSwiglu;
  std::vector<uint64_t> srcs;
  uint64_t dst = 0;
  double limit = 0.0;

  uint64_t src = 0;
  uint64_t length = 0;
  int64_t dst_core = -1;
  uint32_t port = 0;

  uint64_t cycles = 0;
};

struct CoreKernel {
  uint64_t core = 0;
  std::vector<KernelOp> ops;
};

// 外围模块什么时候往哪个核的哪个口喂多少字节。核只做 FFN，前后的计算在外围模块里，
// 它照这张表回放，本身没有逻辑。
struct Feed {
  uint64_t cycle = 0;
  uint64_t core = 0;
  uint32_t port = 0;
  uint64_t length = 0;
};

// 外围模块在哪个核的哪个口上收结果。
struct Drain {
  uint64_t core = 0;
  uint32_t port = 0;
  uint64_t length = 0;
};

struct KernelImage {
  std::vector<CoreKernel> cores;
  std::vector<Feed> feeds;
  std::vector<Drain> drains;
  std::map<std::string, std::string> meta;

  CoreKernel const* Find(uint64_t core_id) const {
    for (auto const& c : cores) {
      if (c.core == core_id) return &c;
    }
    return nullptr;
  }

  uint64_t OpCount() const {
    uint64_t n = 0;
    for (auto const& c : cores) n += c.ops.size();
    return n;
  }
};

}
}
}

#endif
