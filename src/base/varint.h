#ifndef _LATCH_VARINT_
#define _LATCH_VARINT_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace latch {

inline void VarintEncode(std::vector<uint8_t>& buf, uint64_t v) {
  while (v >= 0x80) {
    buf.push_back(static_cast<uint8_t>(v) | 0x80);
    v >>= 7;
  }
  buf.push_back(static_cast<uint8_t>(v));
}

inline uint64_t VarintDecode(const uint8_t* data, std::size_t len,
                             std::size_t& consumed) {
  uint64_t v = 0;
  unsigned shift = 0;
  for (std::size_t i = 0; i < len; ++i) {
    uint8_t b = data[i];
    v |= static_cast<uint64_t>(b & 0x7f) << shift;
    if ((b & 0x80) == 0) {
      consumed = i + 1;
      return v;
    }
    shift += 7;
  }
  consumed = 0;
  return 0;
}

inline uint64_t ZigZagEncode(int64_t n) {
  return (static_cast<uint64_t>(n) << 1) ^
         static_cast<uint64_t>(n >> 63);
}

inline int64_t ZigZagDecode(uint64_t u) {
  return static_cast<int64_t>((u >> 1) ^ -(u & 1));
}

}

#endif
