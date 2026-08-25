#ifndef _LATCH_ISA_
#define _LATCH_ISA_

#include <cstdint>

namespace latch {

class ISA {
 public:
  virtual ~ISA() = default;
  virtual uint64_t GetInstMaxByteWidth() = 0;
};

}  // namespace latch

#endif
