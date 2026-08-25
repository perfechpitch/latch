#ifndef _LATCH_INSTRUCTION_
#define _LATCH_INSTRUCTION_

#include <functional>
#include <map>

#include "isa/field.h"
#include "isa/program.h"

namespace latch {
class System;
class Instance;

class Instruction {
 public:
  Instruction(std::string name, uint64_t width) : uniName(name), bitWidth(width), fields(){};
  virtual ~Instruction() = default;

  InstBinary GetBinary() {
    std::vector<std::pair<uint64_t, uint64_t>> fieldsWidthVal;
    for (auto const& f : fields) {
      fieldsWidthVal.push_back({f.GetLength(), f.GetBinary()});
    }
    return InstBinary(fieldsWidthVal, bitWidth);
  }
  std::string ToStr() {
    std::string ss = "Inst: " + uniName + " Binary: " + GetBinary().ToStr() + "\n";
    for (auto a : attribute) {
      ss = ss + "  " + a.second;
    }
    ss = ss + "\n";
    for (auto f : fields) {
      ss = ss + "  " + f.ToStr() + "\n";
    }
    return ss;
  }

  uint64_t GetBitWidth() { return bitWidth; }
  uint64_t GetByteWidth() {
    LOGCHECK(bitWidth % 8 == 0, "Instruction bitWidth error.");
    return bitWidth / 8;
  }

  void AppendField(const Field& f) { fields.push_back(f); }

  std::string GetAttribute(int attrId) const {
    auto it = attribute.find(attrId);
    return it == attribute.end() ? "" : it->second;
  }

  virtual void RunOnInstance(Instance* instance) = 0;
  std::string GetUniName() { return uniName; }

 protected:
  std::string uniName;
  uint64_t bitWidth;
  std::vector<Field> fields;
  std::map<int, std::string> attribute;
};

}  // namespace latch

#endif