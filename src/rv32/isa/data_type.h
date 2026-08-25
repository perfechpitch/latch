#ifndef _DATATYPE_
#define _DATATYPE_
#include <cmath>
#include <cstdint>

#include "base/log.h"

namespace latch {

union FP32_t {
  FP32_t() {}
  FP32_t(float val) { data = val; }

  uint32_t bits;
  struct {
    uint32_t man : 23;
    uint32_t exp : 8;
    uint32_t sign : 1;
  } field;
  float data;

  static FP32_t Create(uint32_t bits) {
    FP32_t f;
    f.bits = bits;
    return f;
  }

  uint32_t GetBinary() const { return bits; }

  operator float() { return data; }
#define FP32_AS_ARTHIMETIC(T) \
  explicit operator T() const { return (T)((float)(data)); }
  FP32_AS_ARTHIMETIC(uint32_t)
  FP32_AS_ARTHIMETIC(int32_t)
  FP32_AS_ARTHIMETIC(uint16_t)
  FP32_AS_ARTHIMETIC(int16_t)
  FP32_AS_ARTHIMETIC(uint8_t)
  FP32_AS_ARTHIMETIC(int8_t)
#undef FP32_AS_ARTHIMETIC

  static FP32_t Nan() {
    FP32_t nan;
    nan.field.sign = 0;
    nan.field.exp = 0xff;
    nan.field.man = 0x7f'ffff;
    return nan;
  }

  static FP32_t SNan() {
    FP32_t nan;
    nan.field.sign = 0;
    nan.field.exp = 0xff;
    nan.field.man = 0x3f'ffff;
    return nan;
  }

  static FP32_t QNan() {
    FP32_t nan;
    nan.field.sign = 0;
    nan.field.exp = 0xff;
    nan.field.man = 0x7f'ffff;
    return nan;
  }

  static FP32_t PosInf() {
    FP32_t inf;
    inf.data = std::numeric_limits<float>::infinity();
    return inf;
  }
  static FP32_t NegInf() {
    FP32_t inf;
    inf.data = -std::numeric_limits<float>::infinity();
    return inf;
  }
  static FP32_t Max() {
    FP32_t max;
    max.data = std::numeric_limits<float>::max();
    return max;
  }
  static FP32_t Min() {
    FP32_t min;
    min.data = std::numeric_limits<float>::lowest();
    return min;
  }

  bool IsSNaN() const { return field.exp == 0xff && field.man != 0 && !(field.man & 0x40'0000); }
  bool IsQNan() const { return field.exp == 0xff && field.man != 0 && (field.man & 0x40'0000); }

  bool IsNan() const { return std::isnan(data); }
  bool IsInf() const { return std::isinf(data); }
  bool IsPosInf() const { return IsInf() && data > 0; }
  bool IsNegInf() const { return IsInf() && data < 0; }

  FP32_t operator+(const FP32_t& other) const { return FP32_t(this->data + other.data); }
  FP32_t operator-(const FP32_t& other) const { return FP32_t(this->data - other.data); }
  FP32_t operator*(const FP32_t& other) const { return FP32_t(this->data * other.data); }
  FP32_t operator/(const FP32_t& other) const { return FP32_t(this->data / other.data); }
  bool operator==(const FP32_t& other) const { return this->data == other.data; }
  bool operator!=(const FP32_t& other) const { return this->data != other.data; }
  bool operator<(const FP32_t& other) const { return this->data < other.data; }
  bool operator>(const FP32_t& other) const { return this->data > other.data; }
  bool operator<=(const FP32_t& other) const { return this->data <= other.data; }
  bool operator>=(const FP32_t& other) const { return this->data >= other.data; }
};

typedef float fp32_t;

union fp8_e4m3 {
  uint8_t bits;
  struct {
    uint8_t man : 3;
    uint8_t exp : 4;
    uint8_t sign : 1;
  } field;

 public:
  fp8_e4m3() {}
  fp8_e4m3(float val, bool clamp = false) {
    FP32_t fp32;
    fp32.data = val;
    if (fp32.IsNan()) {
      bits = fp8_e4m3::Nan().bits;
    } else if (fp32.IsPosInf()) {
      bits = fp8_e4m3::PosInf().bits;
    } else if (fp32.IsNegInf()) {
      bits = fp8_e4m3::NegInf().bits;
    } else if (fp32.data > (float)fp8_e4m3::Max()) {
      if (clamp)
        bits = fp8_e4m3::Max().bits;
      else
        bits = fp8_e4m3::PosInf().bits;
    } else if (fp32.data < (float)fp8_e4m3::Min()) {
      if (clamp)
        bits = fp8_e4m3::Min().bits;
      else
        bits = fp8_e4m3::NegInf().bits;
    } else {
      if (fp32.field.exp == 0) {
        bits = 0;
        field.sign = fp32.field.sign;
      } else {
        int32_t new_exp = fp32.field.exp - 127 + 7;
        if (new_exp <= 0) {
          field.sign = fp32.field.sign;
          field.exp = 0;
          uint32_t man = (fp32.field.man | 0x800000) >> (1 - new_exp);
          field.man = man >> 20;

          if (HasRoundCarry(man)) {
            uint32_t new_man = (man >> 20) + 1;
            if (new_man == (1 << 3)) {
              field.exp += 1;
              new_man = 0;
            }
            field.man = new_man;
          }
        } else {
          field.sign = fp32.field.sign;
          field.exp = new_exp;
          uint32_t man = fp32.field.man;
          field.man = man >> 20;

          if (HasRoundCarry(man)) {
            uint32_t new_man = (man >> 20) + 1;
            if (new_man == (1 << 3)) {
              field.exp += 1;
              new_man = 0;
            }
            field.man = new_man;
          }
        }
      }
    }
  }

  static fp8_e4m3 Create(uint8_t bits) {
    fp8_e4m3 f;
    f.bits = bits;
    return f;
  }

  uint8_t GetBinary() const { return bits; }

  static fp8_e4m3 Nan() {
    fp8_e4m3 nan;
    nan.field.sign = 0;
    nan.field.exp = 0b1111;
    nan.field.man = 0b111;
    return nan;
  }
  static fp8_e4m3 PosInf() {
    fp8_e4m3 inf;
    inf.field.sign = 0;
    inf.field.exp = 0b1111;
    inf.field.man = 0b111;
    return inf;
  }
  static fp8_e4m3 NegInf() {
    fp8_e4m3 inf;
    inf.field.sign = 1;
    inf.field.exp = 0b1111;
    inf.field.man = 0b111;
    return inf;
  }
  static fp8_e4m3 Max() {
    fp8_e4m3 max;
    max.field.sign = 0;
    max.field.exp = 0b1111;
    max.field.man = 0b110;
    return max;
  }
  static fp8_e4m3 Min() {
    fp8_e4m3 min;
    min.field.sign = 1;
    min.field.exp = 0b1111;
    min.field.man = 0b110;
    return min;
  }

  bool IsNan() const { return field.exp == 0b1111 && field.man == 0b111; }
  bool IsInf() const { return false; }
  bool IsPosInf() const { return false; }
  bool IsNegInf() const { return false; }
  // 同 fp16_t::HasRoundCarry:留高 3 位、丢 bits[19:0],sticky 覆盖 round 位以下全部 bits[18:0]。
  bool HasRoundCarry(uint32_t f32_man) const {
    bool guard = (f32_man >> 20) & 0x1;
    bool round = (f32_man >> 19) & 0x1;
    bool sticky = (f32_man & 0x7FFFF) != 0;
    if ((round && sticky) || (guard && round && !sticky)) {
      return true;
    }
    return false;
  }

  operator float() const {
    if (IsNan()) return FP32_t::Nan().data;
    if (IsPosInf()) return FP32_t::PosInf().data;
    if (IsNegInf()) return FP32_t::NegInf().data;

    if (bits == 0) {
      return 0;
    }
    if (bits == 0x80) {
      return -0.0;
    }
    if (field.exp == 0) {
      int32_t exp_fp32 = 1 - 7;
      uint32_t mantissa = field.man;
      while ((mantissa & 0b1000) == 0) {
        mantissa <<= 1;
        exp_fp32--;
      }
      mantissa &= 0b111;
      FP32_t fp32;
      fp32.field.sign = field.sign;
      fp32.field.exp = exp_fp32 + 127;
      fp32.field.man = mantissa << 20;
      return fp32.data;
    }

    FP32_t fp32;
    fp32.field.sign = field.sign;
    fp32.field.exp = ((int)field.exp) - 7 + 127;
    fp32.field.man = (uint32_t)field.man << (23 - 3);
    return fp32.data;
  }

#define FP8_E4M3_AS_ARTHIMETIC(T) \
  explicit operator T() const { return (T)((float)(*this)); }
  FP8_E4M3_AS_ARTHIMETIC(uint32_t)
  FP8_E4M3_AS_ARTHIMETIC(int32_t)
  FP8_E4M3_AS_ARTHIMETIC(uint16_t)
  FP8_E4M3_AS_ARTHIMETIC(int16_t)
  FP8_E4M3_AS_ARTHIMETIC(uint8_t)
  FP8_E4M3_AS_ARTHIMETIC(int8_t)
#undef FP8_E4M3_AS_ARTHIMETIC

#define FP8_E4M3_ARTHIMETIC_OPERATION(op) \
  fp8_e4m3 operator op(const fp8_e4m3 rhs) { return fp8_e4m3(float(*this) op float(rhs)); }

  FP8_E4M3_ARTHIMETIC_OPERATION(+)
  FP8_E4M3_ARTHIMETIC_OPERATION(-)
  FP8_E4M3_ARTHIMETIC_OPERATION(*)
  FP8_E4M3_ARTHIMETIC_OPERATION(/)
#undef FP8_E4M3_ARTHIMETIC_OPERATION

  bool operator==(fp8_e4m3 const& v) const { return GetBinary() == v.GetBinary(); }
  bool operator!=(fp8_e4m3 const& v) const { return GetBinary() != v.GetBinary(); }
  bool operator>(fp8_e4m3 const& v) const { return float(*this) > float(v); }
  bool operator>=(fp8_e4m3 const& v) const { return float(*this) >= float(v); }
  bool operator<(fp8_e4m3 const& v) const { return float(*this) < float(v); }
  bool operator<=(fp8_e4m3 const& v) const { return float(*this) <= float(v); }
};

union fp8_e5m2 {
  uint8_t bits;
  struct {
    uint8_t man : 2;
    uint8_t exp : 5;
    uint8_t sign : 1;
  } field;

 public:
  fp8_e5m2() {}
  fp8_e5m2(float val, bool clamp = false) {
    FP32_t fp32;
    fp32.data = val;
    if (fp32.IsNan()) {
      bits = fp8_e5m2::Nan().bits;
    } else if (fp32.IsPosInf()) {
      bits = fp8_e5m2::PosInf().bits;
    } else if (fp32.IsNegInf()) {
      bits = fp8_e5m2::NegInf().bits;
    } else if (fp32.data > (float)fp8_e5m2::Max()) {
      if (clamp)
        bits = fp8_e5m2::Max().bits;
      else
        bits = fp8_e5m2::PosInf().bits;
    } else if (fp32.data < (float)fp8_e5m2::Min()) {
      if (clamp)
        bits = fp8_e5m2::Min().bits;
      else
        bits = fp8_e5m2::NegInf().bits;
    } else {
      if (fp32.field.exp == 0) {
        bits = 0;
        field.sign = fp32.field.sign;
      } else {
        int32_t new_exp = fp32.field.exp - 127 + 15;
        if (new_exp <= 0) {
          field.sign = fp32.field.sign;
          field.exp = 0;
          uint32_t man = (fp32.field.man | 0x800000) >> (1 - new_exp);
          field.man = man >> 21;

          if (HasRoundCarry(man)) {
            uint32_t new_man = (man >> 21) + 1;
            if (new_man == (1 << 2)) {
              field.exp += 1;
              new_man = 0;
            }
            field.man = new_man;
          }
        } else {
          field.sign = fp32.field.sign;
          field.exp = new_exp;
          uint32_t man = fp32.field.man;
          field.man = man >> 21;

          if (HasRoundCarry(man)) {
            uint32_t new_man = (man >> 21) + 1;
            if (new_man == (1 << 2)) {
              field.exp += 1;
              new_man = 0;
            }
            field.man = new_man;
          }
        }
      }
    }
  }

  static fp8_e5m2 Create(uint8_t bits) {
    fp8_e5m2 f;
    f.bits = bits;
    return f;
  }

  uint8_t GetBinary() const { return bits; }

  static fp8_e5m2 Nan() {
    fp8_e5m2 nan;
    nan.field.sign = 0;
    nan.field.exp = 0b1'1111;
    nan.field.man = 0b11;
    return nan;
  }
  static fp8_e5m2 PosInf() {
    fp8_e5m2 inf;
    inf.field.sign = 0;
    inf.field.exp = 0b1'1111;
    inf.field.man = 0;
    return inf;
  }
  static fp8_e5m2 NegInf() {
    fp8_e5m2 inf;
    inf.field.sign = 1;
    inf.field.exp = 0b1'1111;
    inf.field.man = 0;
    return inf;
  }
  static fp8_e5m2 Max() {
    fp8_e5m2 max;
    max.field.sign = 0;
    max.field.exp = 0b1'1110;
    max.field.man = 0b11;
    return max;
  }
  static fp8_e5m2 Min() {
    fp8_e5m2 min;
    min.field.sign = 1;
    min.field.exp = 0b1'1110;
    min.field.man = 0b11;
    return min;
  }

  bool IsNan() const { return field.exp == 0b1'1111 && field.man != 0; }
  bool IsInf() const { return field.exp == 0b1'1111 && field.man == 0; }
  bool IsPosInf() const { return IsInf() && field.sign == 0; }
  bool IsNegInf() const { return IsInf() && field.sign != 0; }
  // 同 fp16_t::HasRoundCarry:留高 2 位、丢 bits[20:0],sticky 覆盖 round 位以下全部 bits[19:0]。
  bool HasRoundCarry(uint32_t f32_man) const {
    bool guard = (f32_man >> 21) & 0x1;
    bool round = (f32_man >> 20) & 0x1;
    bool sticky = (f32_man & 0xFFFFF) != 0;
    if ((round && sticky) || (guard && round && !sticky)) {
      return true;
    }
    return false;
  }

  operator float() const {
    if (IsNan()) return FP32_t::Nan().data;
    if (IsPosInf()) return FP32_t::PosInf().data;
    if (IsNegInf()) return FP32_t::NegInf().data;

    if (bits == 0) {
      return 0;
    }
    if (bits == 0x80) {
      return -0.0;
    }
    if (field.exp == 0) {
      int32_t exp_fp32 = 1 - 15;
      uint32_t mantissa = field.man;
      while ((mantissa & 0b100) == 0) {
        mantissa <<= 1;
        exp_fp32--;
      }
      mantissa &= 0b11;
      FP32_t fp32;
      fp32.field.sign = field.sign;
      fp32.field.exp = exp_fp32 + 127;
      fp32.field.man = mantissa << 21;
      return fp32.data;
    }

    FP32_t fp32;
    fp32.field.sign = field.sign;
    fp32.field.exp = ((int)field.exp) - 15 + 127;
    fp32.field.man = (uint32_t)field.man << (23 - 2);
    return fp32.data;
  }

#define FP8_E5M2_AS_ARTHIMETIC(T) \
  explicit operator T() const { return (T)((float)(*this)); }
  FP8_E5M2_AS_ARTHIMETIC(uint32_t)
  FP8_E5M2_AS_ARTHIMETIC(int32_t)
  FP8_E5M2_AS_ARTHIMETIC(uint16_t)
  FP8_E5M2_AS_ARTHIMETIC(int16_t)
  FP8_E5M2_AS_ARTHIMETIC(uint8_t)
  FP8_E5M2_AS_ARTHIMETIC(int8_t)
#undef FP8_E5M2_AS_ARTHIMETIC

#define FP8_E5M2_ARTHIMETIC_OPERATION(op) \
  fp8_e5m2 operator op(const fp8_e5m2 rhs) { return fp8_e5m2(float(*this) op float(rhs)); }

  FP8_E5M2_ARTHIMETIC_OPERATION(+)
  FP8_E5M2_ARTHIMETIC_OPERATION(-)
  FP8_E5M2_ARTHIMETIC_OPERATION(*)
  FP8_E5M2_ARTHIMETIC_OPERATION(/)
#undef FP8_E5M2_ARTHIMETIC_OPERATION

  bool operator==(fp8_e5m2 const& v) const { return GetBinary() == v.GetBinary(); }
  bool operator!=(fp8_e5m2 const& v) const { return GetBinary() != v.GetBinary(); }
  bool operator>(fp8_e5m2 const& v) const { return float(*this) > float(v); }
  bool operator>=(fp8_e5m2 const& v) const { return float(*this) >= float(v); }
  bool operator<(fp8_e5m2 const& v) const { return float(*this) < float(v); }
  bool operator<=(fp8_e5m2 const& v) const { return float(*this) <= float(v); }
};

union fp16_t {
  uint16_t bits;
  struct {
    uint16_t man : 10;
    uint16_t exp : 5;
    uint16_t sign : 1;
  } field;

 public:
  fp16_t() {}
  fp16_t(float val, bool clamp = false) {
    FP32_t fp32;
    fp32.data = val;
    if (fp32.IsSNaN()) {
      bits = fp16_t::SNan().bits;
    } else if (fp32.IsQNan()) {
      bits = fp16_t::QNan().bits;
    } else if (fp32.IsPosInf()) {
      bits = fp16_t::PosInf().bits;
    } else if (fp32.IsNegInf()) {
      bits = fp16_t::NegInf().bits;
    // **上溢的门槛是「舍入之后」超不超**,不是「原值大不大于 Max」:IEEE 就近舍入下,
    //   落在 (Max, 中点) 之间的值该舍回 Max,只有到了中点(65520 = (65504+65536)/2)
    //   才进位成 Inf。按原值比 Max 的话 65505..65519 会被直接判成 Inf,比 IEEE 大一档。
    //   这里只拦 f32 指数已经大到「f16 指数字段必然溢出」的那些(new_exp ≥ 31,即
    //   fp32.exp ≥ 143 ⇒ |x| ≥ 65536),其余交给下面的正常路径 —— 它的进位逻辑
    //   (new_man 满了就 exp += 1)自然会在该给 Inf 的时候给出 exp = 31。
    //   `clamp` 语义不变:要夹的话仍夹到 Max / Min。
    } else if ((int32_t)fp32.field.exp - 127 + 15 >= 0x1F) {
      if (clamp)
        bits = fp32.field.sign ? fp16_t::Min().bits : fp16_t::Max().bits;
      else
        bits = fp32.field.sign ? fp16_t::NegInf().bits : fp16_t::PosInf().bits;
    } else if (clamp && fp32.data > (float)fp16_t::Max()) {
      bits = fp16_t::Max().bits;
    } else if (clamp && fp32.data < (float)fp16_t::Min()) {
      bits = fp16_t::Min().bits;
    } else {
      if (fp32.field.exp == 0) {
        bits = 0;
        field.sign = fp32.field.sign;
      } else {
        int32_t new_exp = fp32.field.exp - 127 + 15;
        if (new_exp <= 0) {
          field.sign = fp32.field.sign;
          field.exp = 0;
          // **右移丢掉的低位要并进 sticky**：只把 `full >> sh` 交给 HasRoundCarry 的话，
          //   移出去的那 sh 位就没人看了 —— 「略大于正中点」会被当成「正好在正中点」、
          //   按 LSB 取偶舍掉，结果比 IEEE 就近舍入小一个 ulp。实测:x=2.98e-08(略大于
          //   f16 最小次正规数的一半)本该进位成 0x0001,舍成了 0x0000;40 万个 f32 里 55 个
          //   踩到,全集中在指数 102(= 2^-25 附近)。把丢掉的位压成一位塞进 bit0 即可 ——
          //   HasRoundCarry 的 sticky 取 `man & 0xFFF`,会连带看到它;guard/round 位不受影响。
          //   sh ≥ 32 时移位是未定义行为,单独判(结果必然全丢,sticky 取整个尾数)。
          const uint32_t sh = (uint32_t)(1 - new_exp);
          const uint32_t full = fp32.field.man | 0x800000;
          uint32_t man = sh >= 32 ? 0u : (full >> sh);
          const uint32_t lost = sh >= 32 ? full : (full & ((1u << sh) - 1u));
          if (lost) man |= 1u;
          field.man = man >> 13;

          if (HasRoundCarry(man)) {
            uint32_t new_man = (man >> 13) + 1;
            if (new_man == (1 << 10)) {
              field.exp += 1;
              new_man = 0;
            }
            field.man = new_man;
          }
        } else {
          field.sign = fp32.field.sign;
          field.exp = new_exp;
          uint32_t man = fp32.field.man;
          field.man = man >> 13;

          if (HasRoundCarry(man)) {
            uint32_t new_man = (man >> 13) + 1;
            if (new_man == (1 << 10)) {
              field.exp += 1;
              new_man = 0;
            }
            field.man = new_man;
          }
        }
      }
    }
  }

  static fp16_t Create(uint16_t bits) {
    fp16_t f;
    f.bits = bits;
    return f;
  }

  uint16_t GetBinary() const { return bits; }

  static fp16_t SNan() {
    fp16_t nan;
    nan.field.sign = 0;
    nan.field.exp = 0x1f;
    nan.field.man = 0x01ff;
    return nan;
  }

  static fp16_t QNan() {
    fp16_t nan;
    nan.field.sign = 0;
    nan.field.exp = 0x1f;
    nan.field.man = 0x03ff;
    return nan;
  }

  static fp16_t Nan() {
    fp16_t nan;
    nan.field.sign = 0;
    nan.field.exp = 0x1f;
    nan.field.man = 0x03ff;
    return nan;
  }
  static fp16_t PosInf() {
    fp16_t inf;
    inf.field.sign = 0;
    inf.field.exp = 0b1'1111;
    inf.field.man = 0;
    return inf;
  }
  static fp16_t NegInf() {
    fp16_t inf;
    inf.field.sign = 1;
    inf.field.exp = 0b1'1111;
    inf.field.man = 0;
    return inf;
  }
  static fp16_t Max() {
    fp16_t max;
    max.field.sign = 0;
    max.field.exp = 0b1'1110;
    max.field.man = 0x3ff;
    return max;
  }
  static fp16_t Min() {
    fp16_t min;
    min.field.sign = 1;
    min.field.exp = 0b1'1110;
    min.field.man = 0x3ff;
    return min;
  }

  bool IsSNaN() const { return field.exp == 0x1F && field.man != 0 && !(field.man & 0x200); }
  bool IsQNan() const { return field.exp == 0x1F && field.man != 0 && (field.man & 0x200); }

  bool IsNan() const { return field.exp == 0x1F && field.man != 0; }
  bool IsInf() const { return field.exp == 0b1'1111 && field.man == 0; }
  bool IsPosInf() const { return IsInf() && field.sign == 0; }
  bool IsNegInf() const { return IsInf() && field.sign != 0; }
  // 就近舍入、逢中取偶:f32 的 23 位尾数留高 10 位,丢 bits[12:0]。round = 最高丢弃位 bit12,
  //   sticky = 它**以下全部**丢弃位 bits[11:0],guard = 留下来那部分的最低位(结果的 LSB)。
  //   sticky 少算一位就会漏掉「bit12=1 且只有 bit11 非零」这一档:它不是正中点,该进位却当成
  //   了正中点、按 LSB 取偶,结果偏小一个 ulp。
  bool HasRoundCarry(uint32_t f32_man) const {
    bool guard = (f32_man >> 13) & 0x1;
    bool round = (f32_man >> 12) & 0x1;
    bool sticky = (f32_man & 0xFFF) != 0;
    if ((round && sticky) || (guard && round && !sticky)) {
      return true;
    }
    return false;
  }

  operator float() const {
    if (IsSNaN()) {
      return FP32_t::SNan().data;
    }
    if (IsQNan()) {
      return FP32_t::QNan().data;
    }

    if (IsPosInf()) return FP32_t::PosInf().data;
    if (IsNegInf()) return FP32_t::NegInf().data;

    if (bits == 0) {
      return 0;
    }
    if (bits == 0x8000) {
      return -0.0;
    }
    if (field.exp == 0) {
      int32_t exp_fp32 = -14;
      uint32_t mantissa = field.man;
      while ((mantissa & 0x400) == 0) {
        mantissa <<= 1;
        exp_fp32--;
      }
      mantissa &= 0x3FF;
      FP32_t fp32;
      fp32.field.sign = field.sign;
      fp32.field.exp = exp_fp32 + 127;
      fp32.field.man = mantissa << 13;
      return fp32.data;
    }

    FP32_t fp32;
    fp32.field.sign = field.sign;
    fp32.field.exp = ((int)field.exp) - 15 + 127;
    fp32.field.man = (uint32_t)field.man << (23 - 10);
    return fp32.data;
  }

#define FP16_AS_ARTHIMETIC(T) \
  explicit operator T() const { return (T)((float)(*this)); }
  FP16_AS_ARTHIMETIC(uint32_t)
  FP16_AS_ARTHIMETIC(int32_t)
  FP16_AS_ARTHIMETIC(uint16_t)
  FP16_AS_ARTHIMETIC(int16_t)
  FP16_AS_ARTHIMETIC(uint8_t)
  FP16_AS_ARTHIMETIC(int8_t)
#undef FP16_AS_ARTHIMETIC

#define FP16_ARTHIMETIC_OPERATION(op) \
  fp16_t operator op(const fp16_t rhs) { return fp16_t(float(*this) op float(rhs)); }

  FP16_ARTHIMETIC_OPERATION(+)
  FP16_ARTHIMETIC_OPERATION(-)
  FP16_ARTHIMETIC_OPERATION(*)
  FP16_ARTHIMETIC_OPERATION(/)
#undef FP16_ARTHIMETIC_OPERATION

  bool operator==(fp16_t const& v) const { return GetBinary() == v.GetBinary(); }
  bool operator!=(fp16_t const& v) const { return GetBinary() != v.GetBinary(); }
  bool operator>(fp16_t const& v) const { return float(*this) > float(v); }
  bool operator>=(fp16_t const& v) const { return float(*this) >= float(v); }
  bool operator<(fp16_t const& v) const { return float(*this) < float(v); }
  bool operator<=(fp16_t const& v) const { return float(*this) <= float(v); }
};

union bf16_t {
  uint16_t bits;
  struct {
    uint16_t man : 7;
    uint16_t exp : 8;
    uint16_t sign : 1;
  } field;

 public:
  bf16_t() {}
  bf16_t(float val, bool clamp = false) {
    FP32_t fp32;
    fp32.data = val;
    if (fp32.IsSNaN()) {
      bits = bf16_t::SNan().bits;
    } else if (fp32.IsQNan()) {
      bits = bf16_t::QNan().bits;
    } else if (fp32.IsPosInf()) {
      bits = bf16_t::PosInf().bits;
    } else if (fp32.IsNegInf()) {
      bits = bf16_t::NegInf().bits;
    } else if (fp32.data > (float)bf16_t::Max()) {
      if (clamp)
        bits = bf16_t::Max().bits;
      else
        bits = bf16_t::PosInf().bits;
    } else if (fp32.data < (float)bf16_t::Min()) {
      if (clamp)
        bits = bf16_t::Min().bits;
      else
        bits = bf16_t::NegInf().bits;
    } else {
      field.sign = fp32.field.sign;
      field.exp = fp32.field.exp;
      uint32_t man = fp32.field.man;
      field.man = man >> 16;

      if (HasRoundCarry(man)) {
        uint32_t new_man = (man >> 16) + 1;
        if (new_man == (1 << 7)) {
          field.exp += 1;
          new_man = 0;
        }
        field.man = new_man;
      }
    }
  }

  static bf16_t Create(uint16_t bits) {
    bf16_t f;
    f.bits = bits;
    return f;
  }

  uint16_t GetBinary() const { return bits; }

  static bf16_t SNan() {
    bf16_t nan;
    nan.field.sign = 0;
    nan.field.exp = 0xFF;
    nan.field.man = 0b0011'1111;
    return nan;
  }
  static bf16_t QNan() {
    bf16_t nan;
    nan.field.sign = 0;
    nan.field.exp = 0xFF;
    nan.field.man = 0b0111'1111;
    return nan;
  }

  static bf16_t Nan() {
    bf16_t nan;
    nan.field.sign = 0;
    nan.field.exp = 0xFF;
    nan.field.man = 0b0111'1111;
    return nan;
  }
  static bf16_t PosInf() {
    bf16_t inf;
    inf.field.sign = 0;
    inf.field.exp = 0xff;
    inf.field.man = 0;
    return inf;
  }
  static bf16_t NegInf() {
    bf16_t inf;
    inf.field.sign = 1;
    inf.field.exp = 0xff;
    inf.field.man = 0;
    return inf;
  }
  static bf16_t Max() {
    bf16_t max;
    max.field.sign = 0;
    max.field.exp = 0b1111'1110;
    max.field.man = 0b111'1111;
    return max;
  }
  static bf16_t Min() {
    bf16_t min;
    min.field.sign = 1;
    min.field.exp = 0b1111'1110;
    min.field.man = 0b111'1111;
    return min;
  }

  bool IsSNaN() const { return field.exp == 0xFF && field.man != 0 && !(field.man & 0x40); }
  bool IsQNan() const { return field.exp == 0xFF && field.man != 0 && (field.man & 0x40); }

  bool IsNan() const { return field.exp == 0xFF && field.man != 0; }
  bool IsInf() const { return field.exp == 0xff && field.man == 0; }
  bool IsPosInf() const { return IsInf() && field.sign == 0; }
  bool IsNegInf() const { return IsInf() && field.sign != 0; }
  // 同 fp16_t::HasRoundCarry:留高 7 位、丢 bits[15:0],sticky 覆盖 round 位以下全部 bits[14:0]。
  bool HasRoundCarry(uint32_t f32_man) const {
    bool guard = (f32_man >> 16) & 0x1;
    bool round = (f32_man >> 15) & 0x1;
    bool sticky = (f32_man & 0x7FFF) != 0;
    if ((round && sticky) || (guard && round && !sticky)) {
      return true;
    }
    return false;
  }

  operator float() const {
    if (IsSNaN()) {
      return FP32_t::SNan().data;
    }

    if (IsQNan()) {
      return FP32_t::QNan().data;
    }

    if (IsPosInf()) return FP32_t::PosInf().data;
    if (IsNegInf()) return FP32_t::NegInf().data;

    FP32_t fp32;
    fp32.field.sign = field.sign;
    fp32.field.exp = field.exp;
    fp32.field.man = ((uint32_t)field.man) << (23 - 7);
    return fp32.data;
  }

#define BF16_AS_ARTHIMETIC(T) \
  explicit operator T() const { return (T)((float)(*this)); }
  BF16_AS_ARTHIMETIC(uint32_t)
  BF16_AS_ARTHIMETIC(int32_t)
  BF16_AS_ARTHIMETIC(uint16_t)
  BF16_AS_ARTHIMETIC(int16_t)
  BF16_AS_ARTHIMETIC(uint8_t)
  BF16_AS_ARTHIMETIC(int8_t)
#undef BF16_AS_ARTHIMETIC

#define BF16_ARTHIMETIC_OPERATION(op) \
  bf16_t operator op(const bf16_t rhs) { return bf16_t(float(*this) op float(rhs)); }

  BF16_ARTHIMETIC_OPERATION(+)
  BF16_ARTHIMETIC_OPERATION(-)
  BF16_ARTHIMETIC_OPERATION(*)
  BF16_ARTHIMETIC_OPERATION(/)
#undef BF16_ARTHIMETIC_OPERATION

  bool operator==(bf16_t const& v) const { return GetBinary() == v.GetBinary(); }
  bool operator!=(bf16_t const& v) const { return GetBinary() != v.GetBinary(); }
  bool operator>(bf16_t const& v) const { return float(*this) > float(v); }
  bool operator>=(bf16_t const& v) const { return float(*this) >= float(v); }
  bool operator<(bf16_t const& v) const { return float(*this) < float(v); }
  bool operator<=(bf16_t const& v) const { return float(*this) <= float(v); }
};

union ef32_t {
  uint32_t bits;
  struct {
    uint32_t zero : 12;
    uint32_t man : 11;
    uint32_t exp : 8;
    uint32_t sign : 1;
  } field;

 public:
  ef32_t() {}

  ef32_t(float val, bool clamp = false) {
    FP32_t fp32;
    fp32.data = val;
    if (fp32.IsNan()) {
      bits = ef32_t::Nan().bits;
    } else if (fp32.IsPosInf()) {
      bits = ef32_t::PosInf().bits;
    } else if (fp32.IsNegInf()) {
      bits = ef32_t::NegInf().bits;
    } else if (fp32.data > (float)ef32_t::Max()) {
      if (clamp)
        bits = ef32_t::Max().bits;
      else
        bits = ef32_t::PosInf().bits;
    } else if (fp32.data < (float)ef32_t::Min()) {
      if (clamp)
        bits = ef32_t::Min().bits;
      else
        bits = ef32_t::NegInf().bits;
    } else {
      if (fp32.bits == 0) {
        bits = 0;
      } else if (fp32.bits == 0x80000000) {
        bits = 0x80000000;
      } else {
        field.sign = fp32.field.sign;
        field.exp = fp32.field.exp;
        field.man = fp32.field.man >> (23 - 11);
        field.zero = 0;
      }
    }
  }

  uint32_t GetBinary() const { return bits; }
  static ef32_t Create(uint32_t bits) {
    ef32_t f;
    f.bits = bits;
    return f;
  }

  static ef32_t Nan() {
    ef32_t nan;
    nan.field.sign = 0;
    nan.field.exp = 0xff;
    nan.field.man = 0x7ff;
    nan.field.zero = 0;
    return nan;
  }
  static ef32_t PosInf() {
    ef32_t inf;
    inf.field.sign = 0;
    inf.field.exp = 0xff;
    inf.field.man = 0;
    inf.field.zero = 0;
    return inf;
  }
  static ef32_t NegInf() {
    ef32_t inf;
    inf.field.sign = 1;
    inf.field.exp = 0xff;
    inf.field.man = 0;
    inf.field.zero = 0;
    return inf;
  }
  static ef32_t Max() {
    ef32_t max;
    max.field.sign = 0;
    max.field.exp = 0b1111'1110;
    max.field.man = 0x7ff;
    max.field.zero = 0;
    return max;
  }
  static ef32_t Min() {
    ef32_t min;
    min.field.sign = 1;
    min.field.exp = 0b1111'1110;
    min.field.man = 0x7ff;
    min.field.zero = 0;
    return min;
  }

  bool IsNan() const { return field.exp == 0xff && field.man != 0; }
  bool IsInf() const { return field.exp == 0xff && field.man == 0; }
  bool IsPosInf() const { return IsInf() && field.sign == 0; }
  bool IsNegInf() const { return IsInf() && field.sign != 0; }

  operator float() const {
    if (IsNan()) return FP32_t::Nan().data;
    if (IsPosInf()) return FP32_t::PosInf().data;
    if (IsNegInf()) return FP32_t::NegInf().data;

    if (bits == 0) {
      return 0;
    }
    if (bits == 0x80000000) {
      return -0.0;
    }
    FP32_t fp32;
    fp32.field.sign = field.sign;
    fp32.field.exp = field.exp;
    fp32.field.man = (uint32_t)field.man << (23 - 11);
    return fp32.data;
  }

#define EF32_AS_ARTHIMETIC(T) \
  explicit operator T() const { return (T)((float)(*this)); }
  EF32_AS_ARTHIMETIC(uint32_t)
  EF32_AS_ARTHIMETIC(int32_t)
  EF32_AS_ARTHIMETIC(uint16_t)
  EF32_AS_ARTHIMETIC(int16_t)
  EF32_AS_ARTHIMETIC(uint8_t)
  EF32_AS_ARTHIMETIC(int8_t)
#undef EF32_AS_ARTHIMETIC

#define EF32_ARTHIMETIC_OPERATION(op) \
  ef32_t operator op(const ef32_t rhs) { return ef32_t(float(*this) op float(rhs)); }

  EF32_ARTHIMETIC_OPERATION(+)
  EF32_ARTHIMETIC_OPERATION(-)
  EF32_ARTHIMETIC_OPERATION(*)
  EF32_ARTHIMETIC_OPERATION(/)
#undef EF32_ARTHIMETIC_OPERATION

  bool operator==(ef32_t const& v) const { return GetBinary() == v.GetBinary(); }
  bool operator!=(ef32_t const& v) const { return GetBinary() != v.GetBinary(); }
  bool operator>(ef32_t const& v) const { return float(*this) > float(v); }
  bool operator>=(ef32_t const& v) const { return float(*this) >= float(v); }
  bool operator<(ef32_t const& v) const { return float(*this) < float(v); }
  bool operator<=(ef32_t const& v) const { return float(*this) <= float(v); }
};

class Bits {
 public:
  explicit Bits(uint64_t data) {
    bits = data;
    length = 64;
  }
  explicit Bits(int32_t data) {
    bits = static_cast<uint64_t>(static_cast<uint32_t>(data));
    length = 32;
  }
  explicit Bits(uint32_t data) {
    bits = static_cast<uint64_t>(data);
    length = 32;
  }
  explicit Bits(uint16_t data) {
    bits = static_cast<uint64_t>(data);
    length = 16;
  }
  explicit Bits(int16_t data) {
    bits = static_cast<uint64_t>(static_cast<uint16_t>(data));
    length = 16;
  }
  explicit Bits(uint8_t data) {
    bits = static_cast<uint8_t>(data);
    length = 8;
  }
  explicit Bits(int8_t data) {
    bits = static_cast<uint64_t>(static_cast<uint8_t>(data));
    length = 8;
  }
  explicit Bits(uint64_t data, uint32_t len) {
    const uint64_t one = 1;
    bits = data & (len >= 64 ? 0xFFFFFFFFFFFFFFFF : ((one << len) - 1));
    length = len;
  }

  template <typename T>
  explicit Bits(const T* data, uint32_t len) {
    LOGCHECK(len <= 64, "Data was dropped!");
    len = std::min(len, (uint32_t)(sizeof(bits) * 8));
    const uint64_t one = 1;
    size_t bytes = len / 8;
    if (bytes == 0 && len > 0) {
      bytes = 1;
    }
    bits = 0;
    memcpy(&bits, data, bytes);
    bits = bits & (len >= 64 ? 0xFFFFFFFFFFFFFFFF : ((one << len) - 1));
    length = len;
  }

  operator uint64_t() const { return bits; }

  Bits Sext64() {
    uint64_t m = 1UL << (length - 1);
    return Bits((bits ^ m) - m, 64);
  }
  Bits Uext64() { return Bits(bits, 64); }
  Bits Sext32() {
    LOGCHECK(length <= 32, "Data was dropped!");
    uint64_t m = 1u << (length - 1);
    int32_t data = (bits ^ m) - m;
    return Bits(data);
  }
  Bits Uext32() {
    LOGCHECK(length <= 32, "Data was dropped!");
    return Bits(bits, 32);
  }
  Bits Sext16() {
    LOGCHECK(length <= 16, "Data was dropped!");
    uint16_t m = 1u << (length - 1);
    int16_t data = (bits ^ m) - m;
    return Bits(data);
  }
  Bits Uext16() {
    LOGCHECK(length <= 16, "Data was dropped!");
    return Bits(bits, 16);
  }

  Bits Chop8() { return Bits(bits & 0xFF, 8); }

  Bits Chop16() { return Bits(bits & 0xFFFF, 16); }

  Bits Chop32() { return Bits(bits & 0xFFFFFFFF, 32); }

  uint64_t CstU64() {
    LOGCHECK(length == 64, "Data length error!");
    return bits;
  }
  int64_t CstS64() {
    LOGCHECK(length == 64, "Data length error!");
    return static_cast<int64_t>(bits);
  }
  uint32_t CstU32() {
    LOGCHECK(length == 32, "Data length error!");
    return static_cast<uint32_t>(bits & 0xffffffff);
  }
  int32_t CstS32() {
    LOGCHECK(length == 32, "Data length error!");
    return static_cast<int32_t>(static_cast<uint32_t>(bits & 0xffffffff));
  }
  uint16_t CstU16() {
    LOGCHECK(length == 16, "Data length error!");
    return static_cast<uint16_t>(bits & 0xffff);
  }
  int16_t CstS16() {
    LOGCHECK(length == 16, "Data length error!");
    return static_cast<int16_t>(static_cast<uint16_t>(bits & 0xffff));
  }
  uint8_t CstU8() {
    LOGCHECK(length == 8, "Data length error!");
    return static_cast<uint8_t>(bits & 0xff);
  }
  int8_t CstS8() {
    LOGCHECK(length == 8, "Data length error!");
    return static_cast<int8_t>(static_cast<uint16_t>(bits & 0xff));
  }

  Bits Concat(const Bits& f) {
    uint64_t flen = f.GetLength();
    uint64_t bin = (GetBinary() << flen) | f.GetBinary();
    return Bits(bin, GetLength() + flen);
  }

  static Bits ConcatFields(std::vector<Bits> fs) {
    Bits result = fs[0];
    for (auto it = fs.begin() + 1; it != fs.end(); ++it) {
      result = result.Concat(*it);
    }
    return result;
  }

  uint64_t GetBinary() const { return bits; }
  uint64_t GetLength() const { return length; }

  int8_t S8() const {
    LOGCHECK(length <= 8, "Bits wider than 8; Chop8() first");
    uint8_t v = static_cast<uint8_t>(bits & 0xff);
    if (length < 8) {
      uint8_t m = static_cast<uint8_t>(1u << (length - 1));
      v = static_cast<uint8_t>((v ^ m) - m);
    }
    return static_cast<int8_t>(v);
  }
  uint8_t U8() const {
    LOGCHECK(length <= 8, "Bits wider than 8; Chop8() first");
    return static_cast<uint8_t>(bits & 0xff);
  }
  int16_t S16() const {
    LOGCHECK(length <= 16, "Bits wider than 16; Chop16() first");
    uint16_t v = static_cast<uint16_t>(bits & 0xffff);
    if (length < 16) {
      uint16_t m = static_cast<uint16_t>(1u << (length - 1));
      v = static_cast<uint16_t>((v ^ m) - m);
    }
    return static_cast<int16_t>(v);
  }
  uint16_t U16() const {
    LOGCHECK(length <= 16, "Bits wider than 16; Chop16() first");
    return static_cast<uint16_t>(bits & 0xffff);
  }
  int32_t S32() const {
    LOGCHECK(length <= 32, "Bits wider than 32; Chop32() first");
    uint32_t v = static_cast<uint32_t>(bits & 0xffffffff);
    if (length < 32) {
      uint32_t m = 1u << (length - 1);
      v = (v ^ m) - m;
    }
    return static_cast<int32_t>(v);
  }
  uint32_t U32() const {
    LOGCHECK(length <= 32, "Bits wider than 32; Chop32() first");
    return static_cast<uint32_t>(bits & 0xffffffff);
  }
  int64_t S64() const {
    if (length == 64) return static_cast<int64_t>(bits);
    uint64_t m = 1ULL << (length - 1);
    return static_cast<int64_t>((bits ^ m) - m);
  }
  uint64_t U64() const { return bits; }

  float F32() const {
    LOGCHECK(length == 32, "F32 requires exact 32 bits");
    return FP32_t::Create(static_cast<uint32_t>(bits & 0xffffffff)).data;
  }
  fp16_t F16() const {
    LOGCHECK(length == 16, "F16 requires exact 16 bits");
    return fp16_t::Create(static_cast<uint16_t>(bits & 0xffff));
  }
  bf16_t BF16() const {
    LOGCHECK(length == 16, "BF16 requires exact 16 bits");
    return bf16_t::Create(static_cast<uint16_t>(bits & 0xffff));
  }
  fp8_e4m3 FP8E4M3() const {
    LOGCHECK(length == 8, "FP8E4M3 requires exact 8 bits");
    return fp8_e4m3::Create(static_cast<uint8_t>(bits & 0xff));
  }
  fp8_e5m2 FP8E5M2() const {
    LOGCHECK(length == 8, "FP8E5M2 requires exact 8 bits");
    return fp8_e5m2::Create(static_cast<uint8_t>(bits & 0xff));
  }

 protected:
  uint64_t bits;
  uint32_t length;
};

template <typename T>
inline bool IsAddOverflow(T a, T b) {
  static_assert(std::is_integral<T>::value, "T must be an integer type.");
  if (std::is_unsigned<T>::value) {
    return a > std::numeric_limits<T>::max() - b;
  } else {
    if ((b > 0) && (a > std::numeric_limits<T>::max() - b)) {
      return true;
    }
    if ((b < 0) && (a < std::numeric_limits<T>::min() - b)) {
      return true;
    }
  }
  return false;
}

template <typename T>
inline bool IsSubOverflow(T a, T b) {
  static_assert(std::is_integral<T>::value, "T must be an integer type.");
  if (std::is_unsigned<T>::value) {
    return a < b;
  } else {
    if ((b > 0) && (a < std::numeric_limits<T>::min() + b)) {
      return true;
    }
    if ((b < 0) && (a > std::numeric_limits<T>::max() + b)) {
      return true;
    }
  }
  return false;
}

template <typename T>
inline bool IsMulOverflow(T a, T b) {
  static_assert(std::is_integral<T>::value, "T must be an integer type");

  if (a == 0 || b == 0) {
    return false;
  }

  if (std::is_unsigned<T>::value) {
    return a > std::numeric_limits<T>::max() / b;
  } else {
    if (a == -1 && b == std::numeric_limits<T>::min()) {
      return true;
    }
    if (b == -1 && a == std::numeric_limits<T>::min()) {
      return true;
    }

    volatile T result = a * b;
    return result / b != a;
  }
}

template <typename T>
inline bool IsSllOverflow(T a, T b) {
  static_assert(std::is_unsigned<T>::value, "T must be an integer type");
  if (std::pow(2, b) >= std::numeric_limits<T>::max()) {
    return true;
  }
  T pow_b = std::pow(2, b);
  return a > std::numeric_limits<T>::max() / pow_b;
}

enum class RoundMode { RoundToEven, RoundToZero, RoundUp, RoundDown };

inline float F2IRounding(float data, RoundMode mode) {
  switch (mode) {
    case RoundMode::RoundToEven: {
      float rounded = std::round(data);
      if (std::fabs(rounded - data) == 0.5) {
        data = 2.0 * std::round(data / 2.0);
      } else {
        data = rounded;
      }
      break;
    }
    case RoundMode::RoundToZero: {
      data = std::trunc(data);
      break;
    }
    case RoundMode::RoundUp: {
      data = std::ceil(data);
      break;
    }
    case RoundMode::RoundDown: {
      data = std::floor(data);
      break;
    }
    default:
      break;
  }
  return data;
}

}  // namespace latch
#endif
