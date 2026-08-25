// 程序加载/运行测试(rv32):同一套 Program(每架构派生的 Rv32Program)的 MakeInst 工厂 + LoadToMemory
// (汇编进 memory)+ Engine.Continue(取指循环跑到 EBREAK 停机),按 mnemonic 加载/运行编码指令(像汇编:
// 参数=输入字段)。
// (rv32 的取指循环整程跑见 kernel.elf 系列测试,现也经 Program::LoadElf 加载。)

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "isa/engine.h"
#include "isa/program.h"
#include "rv32/rv32.h"
#include "spdlog/spdlog.h"

using namespace latch;
using namespace latch::rv32;

class Rv32TextProgramTest : public ::testing::Test {
 protected:
  void SetUp() override {
    spdlog::set_level(spdlog::level::warn);
    isa = std::make_shared<Rv32>();
    sys = std::make_shared<SystemRv32>(isa);
  }
  std::shared_ptr<Rv32> isa;
  std::shared_ptr<SystemRv32> sys;
};

// 直接执行:编码指令按 mnemonic 加载,ADD 走真实 RV32I 编码指令类(参数 = 输入字段 rd/rs1/rs2)。
TEST_F(Rv32TextProgramTest, EncodedAddByMnemonic) {
  sys->SetSR(5, 10, 0);
  sys->SetSR(6, 32, 0);
  auto prog = sys->LoadProgramText("ADD 7 5 6\nEBREAK 0 0\n");  // SR[7] = SR[5] + SR[6]，EBREAK 停机
  sys->LoadToMemory(*prog, 0x80000000);
  sys->SetPC(0x80000000, 0);
  Engine eng(/*maxCyc=*/100);
  eng.Add(sys.get(), 0);
  eng.Continue();
  EXPECT_EQ(42u, sys->GetSR(7, 0));
}

// item key 形式也能识别(ISA_RV32I_ADD == ADD)。
TEST_F(Rv32TextProgramTest, EncodedByItemKey) {
  sys->SetSR(5, 7, 0);
  sys->SetSR(6, 8, 0);
  auto prog = sys->LoadProgramText("ISA_RV32I_ADD 7 5 6\nEBREAK 0 0\n");
  sys->LoadToMemory(*prog, 0x80000000);
  sys->SetPC(0x80000000, 0);
  Engine eng(/*maxCyc=*/100);
  eng.Add(sys.get(), 0);
  eng.Continue();
  EXPECT_EQ(15u, sys->GetSR(7, 0));
}

// 点4(方式一):编码指令汇编成真实字节写进 memory(LoadToMemory, assembleEncoded=true)。
// 验证内存里的字节 == 各指令的编码,且取指循环能从字节 Decode 回同一条指令(FetchInst)。
TEST_F(Rv32TextProgramTest, AssembleEncodedIntoMemory) {
  auto prog = sys->LoadProgramText("ADD 7 5 6\nSUB 8 7 6\n");
  uint64_t base = 0x2000;
  sys->LoadToMemory(*prog, base, /*assembleEncoded=*/true);

  uint64_t addr = base;
  for (auto& inst : prog->GetInstructions()) {
    auto bytes = inst->GetBinary().Convert2Vec();
    std::vector<uint8_t> inmem(bytes.size());
    sys->memorySystem->Read(inmem.data(), addr, bytes.size());
    EXPECT_EQ(inmem, bytes);  // 汇编进内存的字节与指令编码一致
    // 取指循环会从这些字节 Decode 回指令(非指针槽)。
    auto decoded = sys->FetchInst(addr);
    EXPECT_EQ(decoded->GetUniName(), inst->GetUniName());
    addr += inst->GetByteWidth();
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
