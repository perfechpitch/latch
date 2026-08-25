// 在 Scalar（CpuRv32 + 本地内存 + router 上的 UART）上整程跑 kernel.elf：
// 内核往 UART 打 A..Z，做一轮 load/store 自检后把 t0 写成 0x33。

#include "rv32/scalar.h"

#include <gtest/gtest.h>

#include "rv32/rv32.h"
#include "spdlog/spdlog.h"

using namespace latch;
using namespace latch::rv32;

TEST(Rv32Scalar, RunKernelElf) {
  spdlog::set_level(spdlog::level::info);

  // 建核需要入口地址、但此时 cpu 内存尚未就绪，先用 Program::PeekEntry 解析出入口（不加载）。
  uint64_t entry = Program::PeekEntry("./kernel.elf");

  auto scalar = std::make_shared<Scalar>();
  scalar->AddCore(entry);
  scalar->SetUart(0x10000000, 0x10000005, "UartOutput.log");
  scalar->SetLocalMemoryMap(0, entry, 0x100000);

  auto cpu = scalar->GetCpu(0);
  Program prog;
  ASSERT_TRUE(prog.LoadElf("./kernel.elf", cpu->system->memorySystem));
  scalar->Launch(0);

  for (int i = 0; i < 20000; ++i) {
    scalar->Cycle();
  }

  uint32_t t0 = cpu->ReadSr(5, 0);
  EXPECT_EQ(t0, 0x33u) << "Kernel run with error result.";
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
