#ifndef _LATCH_RV32_SCALAR_H_
#define _LATCH_RV32_SCALAR_H_

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "isa/memory.h"
#include "isa/memory_system.h"
#include "module/cpu.h"
#include "rv32/rv32.h"

namespace latch::rv32 {

using namespace latch;
using systeml::MemoryPort;
using systeml::MemoryRouter;

#define THREAD_NUM 1

// 功能模型的逐拍执行器：每拍取指、译码、同步跑完一条指令（无协程、无时序）。
class CpuRv32 : public CpuBase {
 public:
  CpuRv32(uint64_t id, std::shared_ptr<SystemRv32> sys) : CpuBase(id, THREAD_NUM), system(sys) {
    for (uint64_t i = 0; i < thread_num; i++) {
      launch_sts.emplace_back(0);
      exePcs.push_back(0);
    }
  }
  ~CpuRv32() {}

  std::shared_ptr<SystemRv32> getSystem() { return system; }

  bool IsAllThreadHalted() override {
    for (uint64_t i = 0; i < thread_num; i++) {
      if (launch_sts.at(i) == 1) {
        return false;
      }
    }
    return true;
  }

  bool IsStHalted(uint32_t tid) override {
    if (launch_sts.at(tid) == 0) {
      return true;
    }
    return false;
  }

  void Launch(uint64_t pc, uint32_t tid) override {
    launch_sts.at(tid) = 2;
    system->SetPC(pc, tid);
    exePcs[tid] = system->GetPC(tid);
  }

  void Launch(uint32_t tid) override {
    launch_sts.at(tid) = 2;
    exePcs[tid] = system->GetPC(tid);
  }

  void Launch() override {
    for (uint64_t tid = 0; tid < thread_num; tid++) {
      launch_sts.at(tid) = 2;
      exePcs[tid] = system->GetPC(tid);
    }
  }

  void Halt(uint32_t tid) override { launch_sts.at(tid) = 0; }

  void Halt() override {
    for (uint64_t tid = 0; tid < thread_num; tid++) {
      Halt(tid);
    }
  }

  uint64_t ReadSr(uint64_t sr_idx, uint32_t tid) override { return system->GetSR(sr_idx, tid); }

  void WriteSr(uint64_t sr_idx, uint64_t val, uint32_t tid) override { system->SetSR(sr_idx, val, tid); }

  uint64_t ReadSpr(uint64_t spr_idx, uint32_t tid) override { return system->GetSPR(spr_idx, tid); }

  void WriteSpr(uint64_t spr_idx, uint64_t val, uint32_t tid) override { system->SetSPR(spr_idx, val, tid); }

  void EnableExecLog(std::string fileName) override { system->EnableFuncInstLog(fileName); }

  void Cycle() {
    for (uint64_t tid = 0; tid < thread_num; tid++) {
      if (launch_sts.at(tid) == 0) {
        continue;
      }
      if (launch_sts.at(tid) == 2) {
        launch_sts.at(tid) = 1;
      }

      auto pc = exePcs[tid];
      auto inst_bin = system->ReadMemory<uint32_t>(pc, 1);
      auto inst_binary_ptr = std::make_shared<InstBinary>(inst_bin[0]);
      auto inst = system->Decode(inst_binary_ptr);
      LOGCHECK(inst != nullptr, "Instruction fetch/decode failed.");

      spdlog::debug("Cycle Run Inst thread:{}  inst:{}  PC: 0x{:X}", tid,
                    inst->GetAttribute(Rv32KEYS::mnemonic), exePcs[tid]);

      InstanceRv32 instance(inst, system.get());
      system->Step(tid);
      system->SetPC(pc, tid);
      instance.SetLocalPc(static_cast<uint32_t>(pc));

      Register& srFile = system->GetRegFile(REG_SR, tid);
      instance.SnapshotSr([&](uint32_t i) { return srFile[i].U32(); });

      instance.Run();

      instance.CommitSr([&](uint32_t i, uint32_t v) { srFile[i] = v; });

      auto bw = instance.GetInst()->GetByteWidth();

      if (instance.IsBranched()) {
        system->SetPC(instance.LocalPc(), tid);
      } else {
        system->PcIncrease(tid, bw);
      }
      if (instance.Halts()) system->SetHaltState(true, tid);
      exePcs[tid] = system->GetPC(tid);

      if (system->GetHaltState(tid) || BreakpointHit(exePcs[tid], tid)) {
        launch_sts.at(tid) = 0;
      }
    }
  }

 public:
  std::shared_ptr<SystemRv32> system;

  std::vector<uint64_t> exePcs;

  std::vector<uint32_t> launch_sts;
};

static constexpr int kRouterPrioExternal = 0;
static constexpr int kRouterPrioPeripheral = 1;

// 最简 UART：写数据寄存器 → 打到 stdout（可同时落盘）；读状态寄存器恒为“发送空闲、无接收数据”。
class Uart : public MemoryPort {
 public:
  Uart(uint64_t data_reg_addr, uint64_t status_reg_addr, std::string log = "UartOutput.log")
      : data_reg(data_reg_addr), sts_reg(status_reg_addr) {
    if (!log.empty()) {
      logfile = std::make_shared<std::ofstream>(log);
      if (!logfile->is_open()) {
        logfile.reset();
      } else {
        spdlog::info("Uart log to disk filename : " + log);
      }
    }
  }
  ~Uart() {
    if (logfile != nullptr) logfile->close();
  }

  uint64_t DataRegAddr() const { return data_reg; }
  uint64_t StatusRegAddr() const { return sts_reg; }

  uint64_t Size() override {
    return std::max(data_reg, sts_reg) + 1 - std::min(data_reg, sts_reg);
  }

  int Read(uint8_t* dataPtr, uint64_t addr, uint64_t byteWidth) override {
    LOGCHECK(byteWidth == 1, "Uart only supports 1-byte access");
    if (addr == sts_reg) {
      *dataPtr = 0x40 | 0x20;
      return 0;
    }
    if (addr == data_reg) {
      *dataPtr = 0;
      return 0;
    }
    LOGCHECK(false, "Uart::Read: address outside data/status regs");
    return -1;
  }

  int Write(uint8_t* dataPtr, uint64_t addr, uint64_t byteWidth) override {
    LOGCHECK(byteWidth == 1, "Uart only supports 1-byte access");
    if (addr == data_reg) {
      if (logfile != nullptr) *logfile << (*((char*)dataPtr)) << std::flush;
      std::cout << *reinterpret_cast<char*>(dataPtr) << std::flush;
      return 0;
    }
    return 0;
  }

 private:
  uint64_t data_reg;
  uint64_t sts_reg;
  std::shared_ptr<std::ofstream> logfile;
};

// 核内本地内存 + 核外经 router 转发的存储系统。
class MemorySysRouted : public MemoryBase {
 public:
  friend class Scalar;

  explicit MemorySysRouted(std::shared_ptr<MemoryRouter> r)
      : router(std::move(r)) {}
  ~MemorySysRouted() {}

  bool IsAddrOutOfCore(uint64_t deviceAddr) const {
    return deviceAddr < localMemStart || deviceAddr >= localMemEnd;
  }

  int Read(uint8_t* dataPtr, uint64_t deviceAddr, uint64_t byteWidth) override {
    if (IsAddrOutOfCore(deviceAddr)) {
      return router->Read(dataPtr, deviceAddr, byteWidth);
    }
    return MemoryBase::Read(dataPtr, deviceAddr, byteWidth);
  }

  int Write(uint8_t* dataPtr, uint64_t deviceAddr, uint64_t byteWidth) override {
    if (IsAddrOutOfCore(deviceAddr)) {
      return router->Write(dataPtr, deviceAddr, byteWidth);
    }
    return MemoryBase::Write(dataPtr, deviceAddr, byteWidth);
  }

  std::shared_ptr<MemoryRouter> Router() const { return router; }

 private:
  std::shared_ptr<MemoryRouter> router;
  uint64_t localMemStart = 0x80000000;
  uint64_t localMemEnd = 0x80100000;
};

// 标量核系统：若干 CpuRv32 + 各自的本地内存 + 共享 router（外部内存、UART、其他设备）。
class Scalar {
 public:
  Scalar() : isa(std::make_shared<Rv32>()),
             router(std::make_shared<MemoryRouter>()) {
    extMem = std::make_shared<systeml::MemoryBase>();
    router->AddPort(extMem, 0, UINT64_MAX, kRouterPrioExternal, 0, "extMem");
  }
  ~Scalar() {}

  void AddCore(uint64_t pc = 0x80000000) {
    auto sys = std::make_shared<SystemRv32>(isa, pc);
    auto memory = std::make_shared<MemorySysRouted>(router);
    sys->memorySystem = memory;
    auto cpu = std::make_shared<CpuRv32>(cpus.size(), sys);
    cpus.push_back(cpu);
    memorys.push_back(memory);
  }

  void Launch(uint32_t core) { cpus.at(core)->Launch(); }

  void SetLocalMemoryMap(uint32_t core, uint64_t start, uint64_t size) {
    auto ms = memorys.at(core);
    ms->localMemStart = start;
    ms->localMemEnd = start + size;
  }

  void SetUart(uint64_t data_reg_addr, uint64_t status_reg_addr,
               std::string log = "UartOutput.log") {
    auto u = std::make_shared<Uart>(data_reg_addr, status_reg_addr, log);
    uint64_t lo = std::min(data_reg_addr, status_reg_addr);
    uint64_t hi = std::max(data_reg_addr, status_reg_addr) + 1;
    router->AddPort(u, lo, hi - lo, kRouterPrioPeripheral, 0, "uart");
    uart = u;
  }

  std::shared_ptr<systeml::MemoryBase> ExternalMemory() const { return extMem; }

  void MapDevice(std::string name, uint64_t start, uint64_t size,
                 std::shared_ptr<MemoryPort> dev) {
    router->AddPort(std::move(dev), start, size, kRouterPrioPeripheral, 0, std::move(name));
  }

  void Cycle() {
    for (auto cpu : cpus) {
      cpu->Cycle();
    }
  }

  std::shared_ptr<CpuRv32> GetCpu(uint32_t core) { return cpus.at(core); }

  std::shared_ptr<MemoryRouter> router;

 private:
  std::shared_ptr<Rv32> isa = nullptr;
  std::vector<std::shared_ptr<CpuRv32>> cpus;
  std::vector<std::shared_ptr<MemorySysRouted>> memorys;
  std::shared_ptr<Uart> uart;
  std::shared_ptr<systeml::MemoryBase> extMem;
};

}  // namespace latch::rv32

#endif
