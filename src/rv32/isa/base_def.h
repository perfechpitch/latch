#ifndef _LATCH_BASE_DEF_
#define _LATCH_BASE_DEF_

#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

#include "isa/data_type.h"

namespace latch {
enum DataType {
  bit = 0,
  rf32,
  float32,
  float16,
  bfloat16,
  fp8e4m3,
  fp8e5m2,
  double64,
  uint8,
  int8,
  uint16,
  int16,
  uint32,
  int32,
  uint64,
  int64
};

inline size_t DataType2Bw(DataType dataType) {
  switch (dataType) {
    case uint64:
    case int64:
      return 8;
    case uint32:
    case int32:
    case float32:
    case rf32:
      return 4;
      break;
    case uint16:
    case int16:
    case float16:
    case bfloat16:
      return 2;
      break;
    case uint8:
    case int8:
    case fp8e4m3:
    case fp8e5m2:
      return 1;
      break;
    default:
      LOGCHECK(0, "Error Reg type! ");
      break;
  }
  return 0;
}

inline size_t DataType2Bitw(DataType dataType) {
  switch (dataType) {
    case uint64:
    case int64:
      return 64;
    case uint32:
    case int32:
    case float32:
    case rf32:
      return 32;
    case uint16:
    case int16:
    case float16:
    case bfloat16:
      return 16;
    case uint8:
    case int8:
    case fp8e4m3:
    case fp8e5m2:
      return 8;
    case bit:
      return 1;
    default:
      LOGCHECK(0, "Error Reg type! ");
      break;
  }
  return 0;
}

inline bool IsInteger(DataType dataType) {
  switch (dataType) {
    case uint32:
    case int32:
    case uint16:
    case int16:
    case uint8:
    case int8:
    case uint64:
    case int64:
      return true;
      break;
    case float32:
    case rf32:
    case float16:
    case bfloat16:
    case fp8e4m3:
    case fp8e5m2:
      return false;
      break;
    default:
      LOGCHECK(0, "Error Reg type! ");
      break;
  }
  return false;
}

inline bool IsFloat(DataType dataType) { return !IsInteger(dataType); }

inline bool IsSignedInteger(DataType dataType) {
  switch (dataType) {
    case int32:
    case int16:
    case int8:
    case int64:
      return true;
      break;
    default:
      return false;
      break;
  }
  return false;
}

inline bool IsUnsignedInteger(DataType dataType) {
  switch (dataType) {
    case uint32:
    case uint16:
    case uint8:
    case uint64:
      return true;
      break;
    default:
      return false;
      break;
  }
  return false;
}

template <typename T>
struct DataTypeMap {};

template <>
struct DataTypeMap<bool> {
  static const DataType type = DataType::bit;
};
template <>
struct DataTypeMap<uint64_t> {
  static const DataType type = DataType::uint64;
};
template <>
struct DataTypeMap<long long unsigned int> {
  static const DataType type = DataType::uint64;
};
template <>
struct DataTypeMap<int64_t> {
  static const DataType type = DataType::int64;
};
template <>
struct DataTypeMap<uint32_t> {
  static const DataType type = DataType::uint32;
};
template <>
struct DataTypeMap<int32_t> {
  static const DataType type = DataType::int32;
};
template <>
struct DataTypeMap<uint16_t> {
  static const DataType type = DataType::uint16;
};
template <>
struct DataTypeMap<int16_t> {
  static const DataType type = DataType::int16;
};
template <>
struct DataTypeMap<uint8_t> {
  static const DataType type = DataType::uint8;
};
template <>
struct DataTypeMap<int8_t> {
  static const DataType type = DataType::int8;
};
template <>
struct DataTypeMap<fp32_t> {
  static const DataType type = DataType::float32;
};
template <>
struct DataTypeMap<ef32_t> {
  static const DataType type = DataType::rf32;
};
template <>
struct DataTypeMap<fp16_t> {
  static const DataType type = DataType::float16;
};
template <>
struct DataTypeMap<bf16_t> {
  static const DataType type = DataType::bfloat16;
};
template <>
struct DataTypeMap<fp8_e4m3> {
  static const DataType type = DataType::fp8e4m3;
};
template <>
struct DataTypeMap<fp8_e5m2> {
  static const DataType type = DataType::fp8e5m2;
};

template <DataType T>
struct DataTypeLimits;

template <>
struct DataTypeLimits<uint8> {
  static const auto min() { return std::numeric_limits<uint8_t>::min(); }
  static const auto max() { return std::numeric_limits<uint8_t>::max(); }
};

template <>
struct DataTypeLimits<int8> {
  static const auto min() { return std::numeric_limits<int8_t>::min(); }
  static const auto max() { return std::numeric_limits<int8_t>::max(); }
};

template <>
struct DataTypeLimits<uint16> {
  static const auto min() { return std::numeric_limits<uint16_t>::min(); }
  static const auto max() { return std::numeric_limits<uint16_t>::max(); }
};

template <>
struct DataTypeLimits<int16> {
  static const auto min() { return std::numeric_limits<int16_t>::min(); }
  static const auto max() { return std::numeric_limits<int16_t>::max(); }
};

template <>
struct DataTypeLimits<uint32> {
  static const auto min() { return std::numeric_limits<uint32_t>::min(); }
  static const auto max() { return std::numeric_limits<uint32_t>::max(); }
};

template <>
struct DataTypeLimits<int32> {
  static const auto min() { return std::numeric_limits<int32_t>::min(); }
  static const auto max() { return std::numeric_limits<int32_t>::max(); }
};

template <>
struct DataTypeLimits<uint64> {
  static const auto min() { return std::numeric_limits<uint64_t>::min(); }
  static const auto max() { return std::numeric_limits<uint64_t>::max(); }
};

template <>
struct DataTypeLimits<int64> {
  static const auto min() { return std::numeric_limits<int64_t>::min(); }
  static const auto max() { return std::numeric_limits<int64_t>::max(); }
};

template <>
struct DataTypeLimits<float32> {
  static const auto min() { return std::numeric_limits<float>::lowest(); }
  static const auto max() { return std::numeric_limits<float>::max(); }
};

template <>
struct DataTypeLimits<double64> {
  static const auto min() { return std::numeric_limits<double>::lowest(); }
  static const auto max() { return std::numeric_limits<double>::max(); }
};

template <>
struct DataTypeLimits<float16> {
  static const fp16_t min() { return fp16_t::Min(); }
  static const fp16_t max() { return fp16_t::Max(); }
};

template <>
struct DataTypeLimits<bfloat16> {
  static const bf16_t min() { return bf16_t::Min(); }
  static const bf16_t max() { return bf16_t::Max(); }
};

template <>
struct DataTypeLimits<fp8e4m3> {
  static const fp8_e4m3 min() { return fp8_e4m3::Min(); }
  static const fp8_e4m3 max() { return fp8_e4m3::Max(); }
};

template <>
struct DataTypeLimits<fp8e5m2> {
  static const fp8_e5m2 min() { return fp8_e5m2::Min(); }
  static const fp8_e5m2 max() { return fp8_e5m2::Max(); }
};

template <>
struct DataTypeLimits<rf32> {
  static const ef32_t min() { return ef32_t::Min(); }
  static const ef32_t max() { return ef32_t::Max(); }
};
}  // namespace latch

#endif