#ifndef _ISA_UTILS_
#define _ISA_UTILS_
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

#include "isa/data_type.h"

template <typename T>
inline std::string Int2Str(T integer, bool isHex = true) {
  uint64_t value = uint64_t(integer);
  std::ostringstream ss;
  if (isHex) {
    ss << std::setw(sizeof(T) * 2) << std::setfill('0') << std::uppercase << std::hex << value;
  } else {
    ss << value;
  }
  return ss.str();
}

#endif
