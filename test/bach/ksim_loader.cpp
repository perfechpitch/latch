// kernel 文件的读入。覆盖字段落位、层号跟随、以及各条拒绝路径。

#include <fstream>
#include <string>

#include "bach/ksim/loader.h"
#include "gtest/gtest.h"

using namespace latch::bach::ksim;

namespace {

std::string Fixture() {
  return std::string(PROJECT_TEST_DIR) + "/bach/fixture/tiny.bachk";
}

// 把一份文本写成临时文件，供拒绝路径用。
std::string WriteTemp(std::string const& body, std::string const& name) {
  const std::string path = "/tmp/ksim_" + name + ".bachk";
  std::ofstream out(path);
  out << body;
  return path;
}

}  // namespace

TEST(KsimLoader, ReadsFixture) {
  KernelImage image;
  std::string err;
  ASSERT_TRUE(TryLoadKernels(Fixture(), &image, &err)) << err;

  EXPECT_EQ(image.cores.size(), 2u);
  EXPECT_EQ(image.meta.at("model"), "tiny");
  EXPECT_EQ(image.OpCount(), 12u);
  EXPECT_EQ(image.feeds.size(), 2u);
  EXPECT_EQ(image.drains.size(), 2u);

  auto const* c0 = image.Find(0);
  ASSERT_NE(c0, nullptr);
  ASSERT_EQ(c0->ops.size(), 9u);

  // 第一条是 Recv，端口与落点按字段顺序落位。
  EXPECT_EQ(c0->ops[0].kind, OpKind::kRecv);
  EXPECT_EQ(c0->ops[0].port, 0u);
  EXPECT_EQ(c0->ops[0].dst, 0x1000u);
  EXPECT_EQ(c0->ops[0].length, 2048u);
  EXPECT_EQ(c0->ops[0].layer, 0u);

  // Gemm 的三个操作数地址不能串位，dtype 与 cycles 各就各位。
  EXPECT_EQ(c0->ops[1].kind, OpKind::kGemm);
  EXPECT_EQ(c0->ops[1].m, 8u);
  EXPECT_EQ(c0->ops[1].k, 64u);
  EXPECT_EQ(c0->ops[1].n, 128u);
  EXPECT_EQ(c0->ops[1].a, 0x1000u);
  EXPECT_EQ(c0->ops[1].b, 0x0u);
  EXPECT_EQ(c0->ops[1].c, 0x2000u);
  EXPECT_EQ(c0->ops[1].dtype, DType::kFp4);
  EXPECT_EQ(c0->ops[1].cycles, 96u);

  // Elem 的源地址是变长的，落在 srcs 里，dst 与 limit 不受它影响。
  EXPECT_EQ(c0->ops[3].kind, OpKind::kElem);
  EXPECT_EQ(c0->ops[3].elem, ElemOp::kSwiglu);
  EXPECT_EQ(c0->ops[3].n, 1024u);
  EXPECT_EQ(c0->ops[3].dst, 0x3000u);
  EXPECT_DOUBLE_EQ(c0->ops[3].limit, 10.0);
  ASSERT_EQ(c0->ops[3].srcs.size(), 2u);
  EXPECT_EQ(c0->ops[3].srcs[0], 0x2000u);
  EXPECT_EQ(c0->ops[3].srcs[1], 0x2800u);

  EXPECT_EQ(c0->ops[5].kind, OpKind::kSend);
  EXPECT_EQ(c0->ops[5].src, 0x3800u);
  EXPECT_EQ(c0->ops[5].dst_core, 1);
  EXPECT_EQ(c0->ops[5].port, 0u);
}

TEST(KsimLoader, LayerFollowsTheMarker) {
  KernelImage image;
  std::string err;
  ASSERT_TRUE(TryLoadKernels(Fixture(), &image, &err)) << err;
  auto const* c0 = image.Find(0);
  ASSERT_NE(c0, nullptr);
  for (std::size_t i = 0; i < 6; ++i) EXPECT_EQ(c0->ops[i].layer, 0u) << i;
  for (std::size_t i = 6; i < 9; ++i) EXPECT_EQ(c0->ops[i].layer, 1u) << i;
  // 层号在每条 CORE 记录处归零，不跨核累加。
  EXPECT_EQ(image.Find(1)->ops[0].layer, 0u);
}

TEST(KsimLoader, FeedAndDrain) {
  KernelImage image;
  std::string err;
  ASSERT_TRUE(TryLoadKernels(Fixture(), &image, &err)) << err;
  EXPECT_EQ(image.feeds[0].cycle, 100u);
  EXPECT_EQ(image.feeds[0].core, 0u);
  EXPECT_EQ(image.feeds[0].length, 2048u);
  EXPECT_EQ(image.drains[1].core, 1u);
  EXPECT_EQ(image.drains[1].port, 1u);
}

TEST(KsimLoader, RejectsUnknownRecord) {
  const std::string p = WriteTemp("BACHK 1\nCORE 0\nWOBBLE 1 2\n", "unknown");
  KernelImage image;
  std::string err;
  EXPECT_FALSE(TryLoadKernels(p, &image, &err));
  EXPECT_NE(err.find("WOBBLE"), std::string::npos) << err;
}

TEST(KsimLoader, RejectsUnknownDtype) {
  const std::string p =
      WriteTemp("BACHK 1\nCORE 0\nGEMM 1 2 3 0 0 0 fp3 4\n", "dtype");
  KernelImage image;
  std::string err;
  EXPECT_FALSE(TryLoadKernels(p, &image, &err));
  EXPECT_NE(err.find("fp3"), std::string::npos) << err;
}

TEST(KsimLoader, RejectsWrongFieldCount) {
  const std::string p = WriteTemp("BACHK 1\nCORE 0\nSEND 0x10 2048 1\n", "count");
  KernelImage image;
  std::string err;
  EXPECT_FALSE(TryLoadKernels(p, &image, &err));
}

TEST(KsimLoader, RejectsOpBeforeCore) {
  const std::string p = WriteTemp("BACHK 1\nRECV 0 0x10 8\n", "nocore");
  KernelImage image;
  std::string err;
  EXPECT_FALSE(TryLoadKernels(p, &image, &err));
  EXPECT_NE(err.find("CORE"), std::string::npos) << err;
}

TEST(KsimLoader, RejectsEmptyImage) {
  const std::string p = WriteTemp("BACHK 1\nMETA a b\n", "empty");
  KernelImage image;
  std::string err;
  EXPECT_FALSE(TryLoadKernels(p, &image, &err));
}
