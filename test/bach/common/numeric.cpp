// numeric 这一层的基线：格式转换、舍入模式与两种累加顺序。
//
// 步 8 的判据是「逐条计算原语与参考实现逐 bit 比对」，比对的基准就是这一层。
// 所以这里先把这一层自己钉住：往返转换要么精确、要么落在该格式能表示的最近一
// 档；累加顺序换一下结果就变，那件事要能被看见 —— 顺序是结果的一部分。

#include <gtest/gtest.h>

#include <vector>

#include "bach/common/numeric/accum.h"
#include "bach/common/numeric/mx.h"
#include "bach/common/numeric/round.h"

using namespace latch::bach;
using namespace latch::bach::numeric;

namespace {

// ── BF16 ──

TEST(Numeric, Bf16RoundTripExact) {
  // 低 16 位是零的 FP32 在 BF16 里精确可表示，往返必须逐 bit 相同。
  for (uint32_t hi = 0; hi < 0x10000u; hi += 0x137u) {
    float v = FloatOf(hi << 16);
    if (v != v) continue;   // NaN 单独测
    EXPECT_EQ(BitsOf(FromBf16(ToBf16(v))), hi << 16) << "hi=" << hi;
  }
}

TEST(Numeric, Bf16RoundToNearestEven) {
  // 正好落在两档中间：尾数低 17 位是 0x08000，进位后取偶。
  float lo = FloatOf(0x3F800000u);            // 1.0，尾数偶
  float mid = FloatOf(0x3F808000u);           // 1.0 与下一档的正中间
  EXPECT_EQ(ToBf16(mid), ToBf16(lo));         // 取偶 → 落回 1.0
  float mid2 = FloatOf(0x3F818000u);          // 下一档的正中间，尾数奇
  EXPECT_EQ(ToBf16(mid2), uint16_t(0x3F82u)); // 取偶 → 进位
}

TEST(Numeric, Bf16NanStaysNan) {
  float nan = FloatOf(0x7FC00001u);
  uint16_t h = ToBf16(nan);
  float back = FromBf16(h);
  EXPECT_NE(back, back);
}

TEST(Numeric, RoundModes) {
  // 0x3F808000 正在 1.0 与 1.0078125 中间，五种模式各自落到哪一档。
  float mid = FloatOf(0x3F808000u);
  EXPECT_EQ(NarrowBf16(mid, RoundMode::kRne), uint16_t(0x3F80u));
  EXPECT_EQ(NarrowBf16(mid, RoundMode::kRtz), uint16_t(0x3F80u));
  EXPECT_EQ(NarrowBf16(mid, RoundMode::kRmm), uint16_t(0x3F81u));
  EXPECT_EQ(NarrowBf16(mid, RoundMode::kRup), uint16_t(0x3F81u));
  EXPECT_EQ(NarrowBf16(mid, RoundMode::kRdn), uint16_t(0x3F80u));

  // 负数：朝零截断与朝正无穷在同一档，朝负无穷远离零。
  float neg = FloatOf(0xBF808000u);
  EXPECT_EQ(NarrowBf16(neg, RoundMode::kRtz), uint16_t(0xBF80u));
  EXPECT_EQ(NarrowBf16(neg, RoundMode::kRup), uint16_t(0xBF80u));
  EXPECT_EQ(NarrowBf16(neg, RoundMode::kRdn), uint16_t(0xBF81u));
}

// ── FP8 与 FP4 ──

TEST(Numeric, Fp8E4m3RoundTrip) {
  // 256 个编码里除 NaN 外都要能原样往返。
  for (uint32_t b = 0; b < 256; ++b) {
    float v = FromFp8E4m3(uint8_t(b));
    if (v != v) continue;
    EXPECT_EQ(ToFp8E4m3(v), uint8_t(b)) << "code=" << b;
  }
}

TEST(Numeric, Fp4E2m1RoundTrip) {
  for (uint32_t b = 0; b < 16; ++b) {
    float v = FromFp4E2m1(uint8_t(b));
    EXPECT_EQ(ToFp4E2m1(v), uint8_t(b)) << "code=" << b;
  }
}

TEST(Numeric, E8m0IsPowerOfTwo) {
  // E8M0 只有 2 的幂这一档，往返必须精确。
  for (uint32_t b = 1; b < 255; ++b) {
    float v = FromE8m0(uint8_t(b));
    EXPECT_EQ(ToE8m0(v), uint8_t(b)) << "code=" << b;
  }
}

// ── 编解码 ──

TEST(Numeric, EncodeDecodeBf16) {
  std::vector<float> v;
  for (int i = 0; i < 64; ++i) v.push_back(FromBf16(uint16_t(0x3F00u + i)));
  std::vector<uint8_t> b = Encode(DataType::kBf16, v, {}, RoundMode::kRne);
  std::vector<float> back = Decode(DataType::kBf16, b, v.size());
  ASSERT_EQ(back.size(), v.size());
  for (size_t i = 0; i < v.size(); ++i) {
    EXPECT_EQ(BitsOf(back[i]), BitsOf(v[i])) << "i=" << i;
  }
}

TEST(Numeric, EncodeDecodeMxfp8WithScale) {
  // 一个 32 元素的块共用一个 E8M0 的 scale。往返后不必逐 bit 相同 —— MXFP8 只有
  // 8 位 —— 但要落在这一档能表示的最近一格：解出来再编回去必须是同一个字节。
  std::vector<float> v;
  for (int i = 0; i < 32; ++i) v.push_back(float(i + 1) * 3.25f);
  std::vector<float> scale = MakeScale(DataType::kMxfp8, v);
  ASSERT_EQ(scale.size(), 1u);
  std::vector<uint8_t> b = Encode(DataType::kMxfp8, v, scale, RoundMode::kRne);
  std::vector<uint8_t> sb = EncodeScale(DataType::kMxfp8, scale);
  std::vector<float> back = Decode(DataType::kMxfp8, b, v.size());
  std::vector<float> bs = DecodeScale(DataType::kMxfp8, sb, 1);
  for (size_t i = 0; i < v.size(); ++i) back[i] *= bs[0];

  std::vector<uint8_t> again = Encode(DataType::kMxfp8, back, scale,
                                      RoundMode::kRne);
  EXPECT_EQ(again, b);
}

TEST(Numeric, Fp4PacksTwoPerByte) {
  std::vector<float> v = {1.0f, 2.0f, -1.0f, 0.5f};
  std::vector<uint8_t> b = Encode(DataType::kMxfp4, v, {}, RoundMode::kRne);
  EXPECT_EQ(b.size(), 2u);
  std::vector<float> back = Decode(DataType::kMxfp4, b, 4);
  ASSERT_EQ(back.size(), 4u);
  for (size_t i = 0; i < v.size(); ++i) EXPECT_EQ(back[i], v[i]) << "i=" << i;
}

// ── 累加顺序 ──

TEST(Numeric, AccumOrderMatters) {
  // 一个大数配一串小数：顺序加会把小数吃掉，分块加不会。两个结果不同 bit ——
  // 这正是「顺序是结果的一部分」那件事，参考实现必须照抄硬件的顺序。
  std::vector<float> v;
  v.push_back(1.0e8f);
  for (int i = 0; i < 64; ++i) v.push_back(1.0f);

  float serial = AccumInOrder(v);
  std::vector<float> ones(v.size(), 1.0f);
  float blocked = AccumByScaleBlock(v, ones, 8);
  EXPECT_NE(BitsOf(serial), BitsOf(blocked));
}

TEST(Numeric, AccumByScaleBlockScalesOncePerBlock) {
  // 块内先加完再乘 scale：结果与「逐个乘 scale 再加」在一般情况下不同 bit。
  std::vector<float> prods;
  for (int i = 0; i < 64; ++i) prods.push_back(1.0f / float(i + 3));
  std::vector<float> sc = {0.125f, 8.0f};
  float got = AccumByScaleBlock(prods, sc, 32);

  // 与「块内先加再乘」逐 bit 相同。
  float want = 0.0f;
  for (int b = 0; b < 2; ++b) {
    float part = 0.0f;
    for (int i = b * 32; i < (b + 1) * 32; ++i) part += prods[i];
    want += part * sc[b];
  }
  EXPECT_EQ(BitsOf(got), BitsOf(want));
}

TEST(Numeric, ReduceTreeMatchesLaneThenTree) {
  // VU 的归约：LANES 内先加，再走 ceil(log2 SEG) 级树。
  std::vector<float> v;
  for (int i = 0; i < 128; ++i) v.push_back(1.0f / float(i + 1));
  float got = ReduceTree(v, 32);

  std::vector<float> seg;
  for (int b = 0; b < 4; ++b) {
    float s = 0.0f;
    for (int i = b * 32; i < (b + 1) * 32; ++i) s += v[i];
    seg.push_back(s);
  }
  float want = (seg[0] + seg[1]) + (seg[2] + seg[3]);
  EXPECT_EQ(BitsOf(got), BitsOf(want));
}

TEST(Numeric, ReduceTreeOddSegments) {
  // 段数不是 2 的幂时，最后一级的落单项直接带到下一级。
  std::vector<float> v(96, 1.0f);
  for (int i = 0; i < 96; ++i) v[i] = 1.0f / float(i + 1);
  float got = ReduceTree(v, 32);

  std::vector<float> seg;
  for (int b = 0; b < 3; ++b) {
    float s = 0.0f;
    for (int i = b * 32; i < (b + 1) * 32; ++i) s += v[i];
    seg.push_back(s);
  }
  float want = (seg[0] + seg[1]) + seg[2];
  EXPECT_EQ(BitsOf(got), BitsOf(want));
}

// ── 异常夹取 ──

TEST(Numeric, ClampNanInf) {
  float maxf = FloatOf(0x7F7FFFFFu);
  EXPECT_EQ(BitsOf(ClampNanInf(FloatOf(0x7F800000u))), BitsOf(maxf));
  EXPECT_EQ(BitsOf(ClampNanInf(FloatOf(0xFF800000u))), BitsOf(-maxf));
  EXPECT_EQ(BitsOf(ClampNanInf(FloatOf(0x7FC00000u))), BitsOf(maxf));
  // 有限值原样过。
  EXPECT_EQ(BitsOf(ClampNanInf(1.5f)), BitsOf(1.5f));
}

}  // namespace
