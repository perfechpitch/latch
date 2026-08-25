#ifndef _LATCH_REGISTER_SYSTEM_
#define _LATCH_REGISTER_SYSTEM_

#include <memory>
#include <numeric>

#include "isa/base_def.h"
#include "register.h"

namespace latch {

class RegisterSystem {
 public:
  RegisterSystem(uint32_t thread = 1) : maxThread(thread), regFiles(thread), regNames(), regShapes(), regDataType() {}

  Register AppendRegister(std::string name, uint64_t regType, Register reg, uint32_t thread) {
    LOGCHECK(regFiles.size() > thread, "Register thread exceed");
    ensureSlot(regType);
    LOGCHECK(regFiles[thread][regType].IsEmpty(), "Register appended");
    regFiles[thread][regType] = reg;
    regNames[regType] = name;
    regShapes[regType] = reg.GetShape();
    regDataType[regType] = reg.GetDataType();
    return reg;
  }

  Register& GetRegFile(uint64_t regType, uint32_t thread = 0) { return regFiles[thread][regType]; }
  std::string GetRegName(uint64_t regType) { return regNames[regType]; }
  std::vector<uint64_t> GetRegShape(uint64_t regType) { return regShapes[regType]; }
  DataType GetDataType(uint64_t regType) { return regDataType[regType]; }

  void Reset() {
    for (auto& treg : regFiles) {
      for (auto& reg : treg) {
        if (!reg.IsEmpty()) reg.Reset();
      }
    }
  }

 private:
  void ensureSlot(uint64_t regType) {
    for (auto& t : regFiles) {
      if (t.size() <= regType) t.resize(regType + 1);
    }
    if (regNames.size() <= regType) {
      regNames.resize(regType + 1);
      regShapes.resize(regType + 1);
      regDataType.resize(regType + 1, DataType::uint32);
    }
  }

  std::vector<std::vector<Register>> regFiles;
  std::vector<std::string> regNames;
  std::vector<std::vector<uint64_t>> regShapes;
  std::vector<DataType> regDataType;
  uint32_t maxThread;
};

}  // namespace latch

#endif
