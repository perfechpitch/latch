// CpuRv32 直接跑几条手工编码的 ADD：SystemRv32 自带的 MAIN 区段（mem.inc）覆盖整个地址空间。

#include "rv32/rv32.h"

#include <gtest/gtest.h>

#include "rv32/scalar.h"
#include "spdlog/spdlog.h"

using namespace latch;
using namespace latch::rv32;

class Rv32CpuTest : public testing::Test {
 protected:
  std::shared_ptr<Rv32> isa = nullptr;
  std::shared_ptr<SystemRv32> system = nullptr;
  std::shared_ptr<CpuRv32> rv32 = nullptr;

  void SetUp() override {
    spdlog::set_level(spdlog::level::info);
    isa = std::make_shared<Rv32>();
    system = std::make_shared<SystemRv32>(isa, 0x80010f1e);
    rv32 = std::make_shared<CpuRv32>(0, system);
  }

  void TearDown() override {
    rv32 = nullptr;
    system = nullptr;
    isa = nullptr;
  }
};

TEST_F(Rv32CpuTest, AddChain) {
  // add x7, x6, x5
  uint32_t add567 = (0) << 7;
  add567 = (add567 << 5) + 5;  // rs2
  add567 = (add567 << 5) + 6;  // rs1
  add567 = (add567 << 3) + 0;
  add567 = (add567 << 5) + 7;  // rd
  add567 = (add567 << 7) + 51;

  rv32->system->WriteMemory<uint32_t>(0x80010f1e, {add567, add567, add567});

  rv32->WriteSr(5, 10, 0);
  rv32->WriteSr(6, 11, 0);

  rv32->Launch();
  rv32->Cycle();
  rv32->Cycle();
  rv32->Cycle();

  EXPECT_EQ(rv32->ReadSr(7, 0), 21u);
  EXPECT_EQ(rv32->system->GetPC(0), 0x80010f1e + 12u);

  // SPR 与其影子视图 SCC（SPR_SCC 的 uint8[4] 视图）
  rv32->WriteSpr(SPR_SCC, 0x55, 0);
  EXPECT_EQ(rv32->ReadSpr(SPR_SCC, 0), 0x55u);
  EXPECT_EQ(rv32->system->SCC(0)[0].U8(), 0x55u);
  EXPECT_EQ(rv32->system->SCC(0)[1].U8(), 0x00u);
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
