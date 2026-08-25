#ifndef _LATCH_ISA_UNITTEST_H_
#define _LATCH_ISA_UNITTEST_H_

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

#include "isa/engine.h"

namespace latch {

template <typename IsaT, typename SystemT>
class ISAUnittestT : public ::testing::Test {
 protected:
  std::shared_ptr<IsaT> isa;
  std::shared_ptr<SystemT> system;
  void SetUp() override {
    isa.reset(new IsaT);
    system.reset(new SystemT(isa));
  }

  // 跑一条直接构造的指令：System 不对外 run，由 Engine 驱动——把指令放进当前 PC 的指针槽，单拍推进到它
  // 退休（FetchInst 命中该槽）。停机后门控。供生成的 RUN_INST 宏与手写 inc 用。
  void RunOne(std::shared_ptr<Instruction> inst) {
    if (system->GetHaltState(0)) return;
    system->PlaceInst(system->GetPC(0), inst);
    Engine eng;
    eng.Add(system.get(), 0);
    uint64_t before = system->Retired(0);
    uint64_t guard = 0;  // 安全上限：防异常时挂死（正常 1~几拍即退休）
    while (system->Retired(0) == before && !system->GetHaltState(0) &&
           guard++ < 100000)
      eng.Cycle();
  }
};

}  // namespace latch

#define InitPC system->initPc

#define CHECK_REGISTER_EQ(dtype, reg, val)                                 \
  {                                                                        \
    dtype expected = static_cast<dtype>(val);                              \
    dtype actual = *reinterpret_cast<dtype*>((reg).GetStartAddr());        \
    EXPECT_EQ(actual, expected);                                           \
  }

#define CHECK_MEMORY_EQ(dtype, addr, val)                                  \
  {                                                                        \
    Register tmpReg({1}, DataTypeMap<dtype>::type);                        \
    tmpReg.Reset();                                                        \
    system->ReadMemToReg(tmpReg, addr, sizeof(dtype) * 8);                 \
    dtype expected = static_cast<dtype>(val);                              \
    dtype actual = *reinterpret_cast<dtype*>(tmpReg.GetStartAddr());       \
    EXPECT_EQ(actual, expected);                                           \
  }

#define CLEAR_ALL() system->Reset();

#define TEST_CASE(desc)                                                      \
  if (::testing::ScopedTrace _tc_trace_##__LINE__{__FILE__, __LINE__, desc}; \
      (system->Reset(), true))

#endif
