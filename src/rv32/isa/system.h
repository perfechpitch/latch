#ifndef _LATCH_SYSTEM_
#define _LATCH_SYSTEM_

#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <vector>

#include "isa/isa.h"
#include "isa/memory_system.h"
#include "isa/program.h"
#include "isa/register_system.h"

namespace latch {

class Instance;
class Instruction;

class System {
 public:
  // Engine 经 friend 读写每线程退休计数（单拍逻辑 StepContext 在 engine.h）；System 自身不对外 run 指令。
  friend class Engine;

  struct RegInfo {
    std::string name;
    bool threadLocal;
    std::vector<uint64_t> shape;
    DataType eleType;
    int shadowReg;
    int shadowIndex;
  };

  struct MemRegion {
    std::string name;
    uint64_t base;
    uint64_t size;
    MemKind kind;
    uint8_t fill;
  };

 public:
  System(std::shared_ptr<ISA> isa, uint64_t pc, uint32_t thread = 1)
      : isaPtr(isa), initPc(pc), maxThread(thread), regInfo() {
    registerSystem = std::make_shared<RegisterSystem>(thread);
    memorySystem = std::make_shared<MemoryBase>();
    inflight.resize(thread);
  };
  ~System() {
    if (logFile.is_open()) logFile.close();
  };

  void InitRegister() {
    for (auto reg : regInfo) {
      auto r = reg.first;
      if (reg.second.shadowReg < 0) {
        for (int i = 0; i < maxThread; i++) {
          if (GetRegThreadLocal(r) || i == 0) {
            Register rgis(GetRegShape(r), GetRegEleType(r));
            registerSystem->AppendRegister(GetRegName(r), r, rgis, i);
          } else {
            registerSystem->AppendRegister(GetRegName(r), r, GetRegFile(r, 0), i);
          }
        }
      }
    }
    for (auto reg : regInfo) {
      auto r = reg.first;
      if (reg.second.shadowReg >= 0) {
        for (int i = 0; i < maxThread; i++) {
          Register rgis = GetRegFile(reg.second.shadowReg, i)[reg.second.shadowIndex]
                              .Retype(GetRegEleType(r))
                              .Reshape(GetRegShape(r));
          registerSystem->AppendRegister(GetRegName(r), r, rgis, i);
        }
      }
    }
  }

  void InitMemory() {
    if (!memoryDeclared) return;
    auto routed = std::make_shared<RoutedMemorySystem>();
    for (auto& kv : memRegion) {
      auto& r = kv.second;
      routed->AddRegion(r.base, r.size, r.kind, r.fill);
    }
    memorySystem = routed;
  }

  virtual void Reset() {
    registerSystem->Reset();
    memorySystem->Clear();
    ResetInflight();
    for (int i = 0; i < maxThread; i++) SetPC(-1u, i);
  }

  void EnableFuncInstLog(std::string fileName) {
    if (logFile.is_open()) {
      logFile.close();
    }
    logFile.open(fileName);
    std::cout << "FuncInstLog:" + fileName << std::endl;
  }

  void LogStr(std::string str) { logFile << str << std::endl; }

  virtual uint64_t GetInstWidth() { return isaPtr->GetInstMaxByteWidth(); }

  uint64_t EffectiveWidth(const std::shared_ptr<Instruction>& inst) {
    uint64_t w = inst->GetByteWidth();
    return w ? w : GetInstWidth();
  }

  std::shared_ptr<Instruction> FetchInst(uint64_t pc) {
    auto it = instSlots.find(pc);
    if (it != instSlots.end()) return it->second;
    return Decode(GetInstBinary(pc));
  }

  // System 不对外 run 指令——功能跑指令统一由 Engine 的 Cycle/Continue 驱动（isa/engine.h）。这里只留
  // “取指 / 造 Instance / 快照-提交 / 加载”等原语；单拍推进逻辑在 Engine（StepContext，engine.h），经
  // friend 读写下面的在飞状态 inflight。

  // 把一条（通常无编码的虚拟）指令放进 pc 处的指针槽——“直接构造执行”的加载原语（vpu 那种无取指系统用）。
  void PlaceInst(uint64_t pc, std::shared_ptr<Instruction> inst) { instSlots[pc] = inst; }

  // 本 thread 已退休指令数（Engine 据此判断“跑完一条”）。
  uint64_t Retired(uint32_t thread) const { return inflight[thread].retired; }

  // 清零各 thread 的退休计数（Reset 调；子类重写 Reset 时一并调）。
  void ResetInflight() {
    for (auto& f : inflight) f.retired = 0;
  }

  // 造本 ISA 的 Instance 子类；默认造基类 Instance（无寄存器影子）。定义见 instance.h。
  virtual std::unique_ptr<Instance> MakeInstance(std::shared_ptr<Instruction> inst);

  // 指令体跑前 / 退休时的钩子，默认空。带寄存器影子的 ISA 在此快照 / 提交（如 RV32 的 SR）。
  virtual void PrepareInstance(Instance&, uint32_t /*thread*/) {}
  virtual void RetireInstance(Instance&, uint32_t /*thread*/) {}

  virtual std::unique_ptr<Program> NewProgram() { return std::make_unique<Program>(); }

  std::shared_ptr<Instruction> MakeInst(const std::string& name,
                                        const std::vector<uint64_t>& args) {
    if (!instFactory) instFactory = NewProgram();
    return instFactory->MakeInst(name, args);
  }

  std::unique_ptr<Program> LoadProgramFile(const std::string& path) {
    auto prog = NewProgram();
    prog->Load(path, memorySystem);
    return prog;
  }
  std::unique_ptr<Program> LoadProgramText(const std::string& text) {
    auto prog = NewProgram();
    prog->LoadText(text);
    return prog;
  }

  uint64_t LoadToMemory(Program& prog, uint64_t base, bool assembleEncoded = true) {
    uint64_t addr = base;
    for (auto& inst : prog.GetInstructions()) {
      uint64_t w = EffectiveWidth(inst);
      if (assembleEncoded && inst->GetByteWidth() > 0) {
        auto bytes = inst->GetBinary().Convert2Vec();
        memorySystem->Write(bytes.data(), addr, bytes.size());
      } else {
        instSlots[addr] = inst;
      }
      addr += w;
    }
    return base;
  }

  // RunProgram / ExecuteSystem 已移除：功能跑指令统一走 Engine 的 Continue（PC + 停机驱动，isa/engine.h）。

  virtual uint64_t GetPC(uint32_t thread = 0) = 0;
  virtual void SetPC(uint64_t pc = -1u, uint32_t thread = 0) = 0;

  virtual bool GetHaltState(uint32_t thread = 0) = 0;
  virtual void SetHaltState(bool halt, uint32_t thread = 0) = 0;

  void AppendRegInfo(uint32_t reg, RegInfo rInfo) { regInfo[reg] = rInfo; }
  std::string GetRegName(uint32_t reg) { return regInfo[reg].name; }
  bool GetRegThreadLocal(uint32_t reg) { return regInfo[reg].threadLocal; }
  std::vector<uint64_t> GetRegShape(uint32_t reg) { return regInfo[reg].shape; }
  DataType GetRegEleType(uint32_t reg) { return regInfo[reg].eleType; }
  DataType GetDataType(uint64_t regType) { return registerSystem->GetDataType(regType); }
  Register& GetRegFile(uint64_t i, uint32_t thread = 0) { return registerSystem->GetRegFile(i, thread); }

  void MarkMemoryDeclared() { memoryDeclared = true; }
  void AppendMemRegion(uint32_t id, MemRegion r) { memRegion[id] = r; }

  // virtual:派生类可以把它接到「本条命令的输入锁存」上 —— 引擎的功能 body 要读的是
  //   数据离开存储那一刻的值,不是 body 真跑那一刻内存里的值(gmp 侧 `SystemGmp::ReadMemory`)。
  virtual void ReadMemory(uint64_t deviceAddr, uint64_t length, void* dst) {
    memorySystem->Read((uint8_t*)(dst), deviceAddr, length);
  }
  template <typename T>
  std::vector<T> ReadMemory(uint64_t deviceAddr, uint64_t size) {
    std::vector<T> value(size);
    memorySystem->Read((uint8_t*)(value.data()), deviceAddr, size * sizeof(T));
    return value;
  }

  void WriteMemory(uint64_t deviceAddr, uint64_t length, const void* data) {
    memorySystem->Write((uint8_t*)(data), deviceAddr, length);
  }
  template <typename T>
  void WriteMemory(uint64_t deviceAddr, const std::vector<T>& value) {
    memorySystem->Write<T>(deviceAddr, value);
  }

  void ReadMemToReg(Register reg, uint64_t deviceAddr, uint64_t bitWidth = 0) {
    uint64_t memByte = reg.GetByteWidth();
    if (bitWidth != 0) {
      LOGCHECK(bitWidth % 8 == 0 && bitWidth >= 8, "");
      memByte = bitWidth / 8;
      LOGCHECK(memByte <= reg.GetByteWidth(), "");
    }
    memorySystem->Read(reg.GetStartAddr(), deviceAddr, memByte);
  }
  void WriteMemFromReg(Register reg, uint64_t deviceAddr, uint64_t bitWidth = 0) {
    uint64_t memByte = reg.GetByteWidth();
    if (bitWidth != 0) {
      LOGCHECK(bitWidth % 8 == 0, "");
      memByte = bitWidth / 8;
      LOGCHECK(memByte <= reg.GetByteWidth(), "");
    }
    memorySystem->Write(reg.GetStartAddr(), deviceAddr, memByte);
  }

  // 指令体用的标量访存原语。
  template <typename T>
  T LoadMem(uint64_t deviceAddr) {
    T v{};
    memorySystem->Read(reinterpret_cast<uint8_t*>(&v), deviceAddr, sizeof(T));
    return v;
  }
  template <typename T>
  void StoreMem(uint64_t deviceAddr, T value) {
    memorySystem->Write(reinterpret_cast<uint8_t*>(&value), deviceAddr, sizeof(T));
  }

  template <typename T>
  void SetMemory(uint64_t deviceAddr, T value) {
    Register tmpReg({1}, DataTypeMap<T>::type);
    tmpReg.Set<T>(value);
    WriteMemFromReg(tmpReg, deviceAddr);
  }

  std::shared_ptr<ISA> GetIsa() { return isaPtr; }

 protected:
  std::shared_ptr<InstBinary> GetInstBinary(uint64_t pc) {
    uint64_t instByteWidth = isaPtr->GetInstMaxByteWidth();
    uint8_t buf[16];
    LOGCHECK(instByteWidth <= sizeof(buf), "instByteWidth exceeds buffer size");
    memorySystem->Read(buf, pc, instByteWidth);
    return std::make_shared<InstBinary>(buf, instByteWidth);
  }

  virtual std::shared_ptr<Instruction> Decode(std::shared_ptr<InstBinary> instBinary) const = 0;

 public:
  uint64_t initPc;
  std::shared_ptr<ISA> isaPtr;
  std::shared_ptr<RegisterSystem> registerSystem;
  std::shared_ptr<MemoryBase> memorySystem;

 protected:
  std::ofstream logFile;
  uint32_t maxThread;
  std::map<uint32_t, RegInfo> regInfo;
  std::map<uint32_t, MemRegion> memRegion;
  bool memoryDeclared = false;

  std::map<uint64_t, std::shared_ptr<Instruction>> instSlots;

  std::unique_ptr<Program> instFactory;

  // 每线程已退休指令数。单拍推进的逻辑在 Engine（friend，engine.h 的 StepContext）。
  struct Inflight {
    uint64_t retired = 0;
  };
  std::vector<Inflight> inflight;
};

}  // namespace latch

#endif
