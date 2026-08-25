#ifndef _LATCH_FIELD_
#define _LATCH_FIELD_

#include "isa/data_type.h"
#include "base/log.h"

#include <iostream>
#include <vector>

namespace latch {

class Field : public Bits {
 public:
  Field(uint64_t len, uint64_t bin, std::string n = "") : name(n), Bits(bin, len) {
    LOGCHECK(len <= 64, "");
    bool is_negative_imm = (((int64_t)bin) >> (len - 1) == -1);
    bool is_positive_imm = (bin >> len == 0);
    if (!(is_negative_imm || is_positive_imm || len == 64)) {
      spdlog::error("Field overflow: name='{}' len={} bin={:#x} ({})", name, len, bin, bin);
      LOGCHECK(false, "len is error for data");
    }
    if (len == 64) {
      bits = bits;
    } else {
      uint64_t m = ((uint64_t)1 << len) - 1;
      bits = bits & m;
    }
  }

  Field Concat(const Field& f) {
    uint64_t len = GetLength() + f.GetLength();
    uint64_t bin = (GetBinary() << f.GetLength()) | f.GetBinary();
    return Field(len, bin);
  }

  static Field Concat(std::vector<Field> fs) {
    LOGCHECK(fs.size() > 1, "");
    Field result(0, 0);
    for (auto f : fs) result = result.Concat(f);
    return result;
  }

  ~Field() {}
  std::string GetName() const { return name; }
  uint64_t SpliceBinary(uint64_t src) { return ((src << length) | bits); }
  operator uint64_t() { return static_cast<uint64_t>(bits); }

  std::string ToStr() { return GetName() + " : " + std::to_string((uint64_t)bits); }

 private:
  std::string name;
};

class Config : public Field {
 public:
  Config(uint64_t len, uint64_t bin, std::string n = "") : Field(len, bin, n) {}
  ~Config() {}
};

class ConstField : public Field {
 public:
  ConstField(uint64_t len, uint64_t bin, std::string n = "") : Field(len, bin, n) {}
  ~ConstField() {}
};

}  // namespace latch

#define CONCAT(...) ::latch::Field::Concat(std::vector<::latch::Field>{__VA_ARGS__})

#endif