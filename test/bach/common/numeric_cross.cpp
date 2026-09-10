// 步 1 的判据：`numeric/` 与 `reference/` 的编解码逐 bit 对齐。
//
// 两份实现独立写出来：C++ 这一份在 src/bach/common/numeric/，Python 那一份在
// src/bach/compiler/reference/。比对向量由 Python 那一份产出，这里读进来用 C++ 这一份
// 复算一遍，对不上就说明其中一份错了。
//
// 数值一律按 FP32 的 32 位模式比，不比十进制 —— 十进制的字面量在两侧解析出来
// 未必是同一个 bit。

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "bach/common/numeric/accum.h"
#include "bach/common/numeric/formats.h"
#include "bach/common/numeric/mx.h"

using namespace latch::bach;

namespace {

std::string VectorDir() {
  return std::string(LATCH_SOURCE_DIR) + "/src/bach/compiler/reference/vectors/";
}

// 读一份向量文件，去掉注释行与空行。
std::vector<std::string> ReadLines(std::string const& name) {
  std::vector<std::string> out;
  std::ifstream f(VectorDir() + name);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    out.push_back(line);
  }
  return out;
}

std::vector<std::string> Split(std::string const& s) {
  std::vector<std::string> out;
  std::istringstream is(s);
  std::string tok;
  while (is >> tok) out.push_back(tok);
  return out;
}

uint32_t Hex(std::string const& s) {
  return uint32_t(std::stoul(s, nullptr, 16));
}

float FloatOfHex(std::string const& s) { return numeric::FloatOf(Hex(s)); }

// 逗号分隔的一串 32 位模式。
std::vector<float> Floats(std::string const& s) {
  std::vector<float> out;
  if (s == "-") return out;
  std::istringstream is(s);
  std::string tok;
  while (std::getline(is, tok, ',')) out.push_back(FloatOfHex(tok));
  return out;
}

std::vector<uint8_t> Bytes(std::string const& s) {
  std::vector<uint8_t> out;
  if (s == "-") return out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    out.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
  }
  return out;
}

numeric::DataType TypeOf(std::string const& name) {
  if (name == "bf16") return numeric::DataType::kBf16;
  if (name == "mxfp8") return numeric::DataType::kMxfp8;
  if (name == "mxfp4") return numeric::DataType::kMxfp4;
  if (name == "nvfp4") return numeric::DataType::kNvfp4;
  return numeric::DataType::kFp32;
}

// NaN 有多个编码，比位模式会因为两侧选了不同的 NaN 载荷而误判。约定：两侧都是
// NaN 就算一致。
bool SameBits(float a, float b) {
  uint32_t x = numeric::BitsOf(a), y = numeric::BitsOf(b);
  if (x == y) return true;
  auto nan = [](uint32_t v) {
    return (v & 0x7F800000u) == 0x7F800000u && (v & 0x007FFFFFu) != 0;
  };
  return nan(x) && nan(y);
}

}  // namespace

// 四种标量格式的编码：同一个输入，两份实现编出同一个码。
TEST(NumericCross, ScalarEncodeMatches) {
  std::vector<std::string> lines = ReadLines("scalar.txt");
  ASSERT_FALSE(lines.empty()) << "比对向量没生成，先跑 reference/vectors.py";
  uint64_t checked = 0;
  for (std::string const& line : lines) {
    std::vector<std::string> f = Split(line);
    ASSERT_EQ(f.size(), 4u) << line;
    std::string const& codec = f[0];
    float in = FloatOfHex(f[1]);
    uint32_t want = Hex(f[2]);
    if (codec == "fp8e5m2") continue;   // 这一档只验解码

    uint32_t got = 0;
    if (codec == "bf16") {
      got = numeric::ToBf16(in);
    } else if (codec == "fp8e4m3") {
      got = numeric::ToFp8E4m3(in);
    } else if (codec == "fp4e2m1") {
      got = numeric::ToFp4E2m1(in);
    } else if (codec == "e8m0") {
      got = numeric::ToE8m0(in);
    } else {
      FAIL() << "不认识的格式 " << codec;
    }
    EXPECT_EQ(got, want) << codec << " 编 " << f[1];
    ++checked;
  }
  EXPECT_GT(checked, 500u);
}

// 五种标量格式的解码：同一个码，两份实现解出同一个值。
TEST(NumericCross, ScalarDecodeMatches) {
  std::vector<std::string> lines = ReadLines("scalar.txt");
  ASSERT_FALSE(lines.empty());
  for (std::string const& line : lines) {
    std::vector<std::string> f = Split(line);
    std::string const& codec = f[0];
    uint32_t code = Hex(f[2]);
    float want = FloatOfHex(f[3]);

    float got = 0.0f;
    if (codec == "bf16") {
      got = numeric::FromBf16(uint16_t(code));
    } else if (codec == "fp8e4m3") {
      got = numeric::FromFp8E4m3(uint8_t(code));
    } else if (codec == "fp8e5m2") {
      got = numeric::FromFp8E5m2(uint8_t(code));
    } else if (codec == "fp4e2m1") {
      got = numeric::FromFp4E2m1(uint8_t(code));
    } else if (codec == "e8m0") {
      got = numeric::FromE8m0(uint8_t(code));
    }
    EXPECT_TRUE(SameBits(got, want))
        << codec << " 解 " << f[2] << "：得 " << numeric::BitsOf(got)
        << " 期望 " << numeric::BitsOf(want);
  }
}

// MX 的分块：一组值算出来的 scale、编出来的字节、解回来的值三样都要一致。
TEST(NumericCross, BlockCodecMatches) {
  std::vector<std::string> lines = ReadLines("block.txt");
  ASSERT_FALSE(lines.empty());
  for (std::string const& line : lines) {
    std::vector<std::string> f = Split(line);
    ASSERT_EQ(f.size(), 7u) << line;
    numeric::DataType t = TypeOf(f[0]);
    std::vector<uint8_t> want_scale = Bytes(f[2]);
    std::vector<uint8_t> want_data = Bytes(f[3]);
    std::vector<float> in = Floats(f[4]);
    std::vector<float> want_dsc = Floats(f[5]);
    std::vector<float> want_back = Floats(f[6]);

    std::vector<float> scale = numeric::MakeScale(t, in);
    std::vector<uint8_t> sbytes = numeric::EncodeScale(t, scale);
    if (want_scale.empty()) {
      EXPECT_TRUE(sbytes.empty()) << f[0] << "：这一档不该有 scale";
    } else {
      EXPECT_EQ(sbytes, want_scale) << f[0] << " 的 scale 字节";
    }
    std::vector<uint8_t> data =
        numeric::Encode(t, in, scale, numeric::RoundMode::kRne);
    EXPECT_EQ(data, want_data) << f[0] << " 的数据字节";

    std::vector<float> dsc = numeric::DecodeScale(t, sbytes, scale.size());
    ASSERT_EQ(dsc.size(), want_dsc.size()) << f[0];
    for (size_t i = 0; i < dsc.size(); ++i) {
      EXPECT_TRUE(SameBits(dsc[i], want_dsc[i])) << f[0] << " scale[" << i << "]";
    }
    std::vector<float> back = numeric::Decode(t, data, in.size());
    ASSERT_EQ(back.size(), want_back.size()) << f[0];
    for (size_t i = 0; i < back.size(); ++i) {
      EXPECT_TRUE(SameBits(back[i], want_back[i])) << f[0] << "[" << i << "]";
    }
  }
}

// 三条累加顺序：浮点加法不满足结合律，顺序错了就差一个 bit。
TEST(NumericCross, AccumOrderMatches) {
  std::vector<std::string> lines = ReadLines("accum.txt");
  ASSERT_FALSE(lines.empty());
  for (std::string const& line : lines) {
    std::vector<std::string> f = Split(line);
    ASSERT_EQ(f.size(), 5u) << line;
    std::string const& kind = f[0];
    uint64_t arg = std::stoull(f[1]);
    std::vector<float> in = Floats(f[2]);
    std::vector<float> extra = Floats(f[3]);
    float want = FloatOfHex(f[4]);

    float got = 0.0f;
    if (kind == "in_order") {
      got = numeric::AccumInOrder(in);
    } else if (kind == "by_block") {
      got = numeric::AccumByScaleBlock(in, extra, arg);
    } else if (kind == "reduce_tree") {
      got = numeric::ReduceTree(in, arg);
    } else if (kind == "clamp") {
      got = numeric::ClampNanInf(in.at(0));
    } else {
      FAIL() << "不认识的累加 " << kind;
    }
    EXPECT_TRUE(SameBits(got, want))
        << kind << " arg=" << arg << "：得 " << numeric::BitsOf(got)
        << " 期望 " << numeric::BitsOf(want);
  }
}

namespace {

// 逐 bit 比一组值。NaN 只比「是不是 NaN」。
void ExpectSame(std::vector<float> const& got, std::vector<float> const& want,
                std::string const& what) {
  ASSERT_EQ(got.size(), want.size()) << what;
  for (size_t i = 0; i < got.size(); ++i) {
    EXPECT_TRUE(SameBits(got[i], want[i]))
        << what << "[" << i << "]：得 " << numeric::BitsOf(got[i])
        << " 期望 " << numeric::BitsOf(want[i]);
  }
}

// MU 的一条原语：一个 1×K 的 token 乘一个 K×N 的权重块。
//
// 乘积先逐个算出来存下再加：写成 acc += a[i] * b[i] 编译器会合成积和融合，
// 少一次舍入，与硬件先乘后加差一个 bit。
std::vector<float> Gemm(numeric::DataType t, std::vector<uint8_t> const& token,
                        std::vector<uint8_t> const& weight,
                        std::vector<uint8_t> const& scale, uint64_t k,
                        uint64_t count_n, bool out_bf16) {
  uint64_t block = numeric::ScaleBlockOf(t);
  uint64_t elem_bits = numeric::ElemBitsOf(t);
  std::vector<float> a = numeric::Decode(t, token, k);
  std::vector<float> sc;
  if (block != 0) sc = numeric::DecodeScale(t, scale, k / block);

  std::vector<float> out;
  uint64_t col_bytes = k * elem_bits / 8;
  for (uint64_t j = 0; j < count_n; ++j) {
    std::vector<uint8_t> col(weight.begin() + j * col_bytes,
                             weight.begin() + (j + 1) * col_bytes);
    std::vector<float> b = numeric::Decode(t, col, k);
    std::vector<float> prod(k, 0.0f);
    for (uint64_t i = 0; i < k; ++i) prod[i] = a[i] * b[i];
    float acc = block == 0 ? numeric::AccumInOrder(prod)
                           : numeric::AccumByScaleBlock(prod, sc, block);
    float r = numeric::ClampNanInf(acc);
    if (out_bf16) r = numeric::FromBf16(numeric::ToBf16(r));
    out.push_back(r);
  }
  return out;
}

// 逗号分隔的几组，组之间用分号。
std::vector<std::vector<float>> FloatGroups(std::string const& s) {
  std::vector<std::vector<float>> out;
  std::istringstream is(s);
  std::string tok;
  while (std::getline(is, tok, ';')) out.push_back(Floats(tok));
  return out;
}

}  // namespace

// 步 11 的判据打底：FFN 那几档算子与参考实现逐 bit 对齐。
TEST(NumericCross, FfnOperatorsMatch) {
  std::vector<std::string> lines = ReadLines("ffn.txt");
  ASSERT_FALSE(lines.empty()) << "比对向量没生成，先跑 reference/vectors.py";
  uint64_t gemms = 0;
  for (std::string const& line : lines) {
    std::vector<std::string> f = Split(line);
    std::string const& kind = f[0];

    if (kind == "gemm") {
      ASSERT_EQ(f.size(), 9u) << line;
      numeric::DataType t = TypeOf(f[1]);
      uint64_t k = std::stoull(f[2]);
      uint64_t cnt = std::stoull(f[3]);
      bool out_bf16 = f[4] == "1";
      std::vector<float> got =
          Gemm(t, Bytes(f[5]), Bytes(f[6]), Bytes(f[7]), k, cnt, out_bf16);
      ExpectSame(got, Floats(f[8]), "gemm " + f[1] + " K=" + f[2]);
      ++gemms;
    } else if (kind == "elemwise") {
      ASSERT_EQ(f.size(), 6u) << line;
      std::vector<float> a = Floats(f[3]);
      std::vector<float> b = Floats(f[4]);
      std::vector<float> got;
      for (size_t i = 0; i < a.size(); ++i) {
        if (f[1] == "add") got.push_back(a[i] + b[i]);
        else if (f[1] == "sub") got.push_back(a[i] - b[i]);
        else if (f[1] == "mul") got.push_back(a[i] * b[i]);
        else if (f[1] == "max") got.push_back(a[i] > b[i] ? a[i] : b[i]);
        else if (f[1] == "min") got.push_back(a[i] < b[i] ? a[i] : b[i]);
        else FAIL() << "不认识的逐元素运算 " << f[1];
      }
      ExpectSame(got, Floats(f[5]), "elemwise " + f[1]);
    } else if (kind == "reduce_experts") {
      ASSERT_EQ(f.size(), 5u) << line;
      std::vector<std::vector<float>> parts = FloatGroups(f[3]);
      ASSERT_FALSE(parts.empty());
      // 专家间求和按分量到达的先后顺序加。
      std::vector<float> got = parts[0];
      for (size_t p = 1; p < parts.size(); ++p) {
        for (size_t i = 0; i < got.size(); ++i) got[i] += parts[p][i];
      }
      ExpectSame(got, Floats(f[4]), "reduce_experts " + f[1]);
    } else if (kind == "vu_reduce") {
      // 这一条由 VU 那一侧拿真模块跑，这里只验 numeric:: 的归约顺序与它一致。
      ASSERT_EQ(f.size(), 5u) << line;
      uint64_t lanes = std::stoull(f[1]);
      std::vector<float> in = Floats(f[3]);
      float got = numeric::ClampNanInf(0.0f + numeric::ReduceTree(in, lanes));
      EXPECT_TRUE(SameBits(got, FloatOfHex(f[4]))) << "vu_reduce VL=" << f[2];
    } else {
      FAIL() << "不认识的算子 " << kind;
    }
  }
  EXPECT_GT(gemms, 20u);
}
