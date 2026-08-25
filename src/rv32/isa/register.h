#ifndef _LATCH_REGISTER_
#define _LATCH_REGISTER_

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <numeric>

#include "isa/base_def.h"
#include "field.h"
#include "utils.h"

namespace latch {

// Register 专用的定容内联 shape。视图构造（operator[]/Reshape）在指令 body 的逐 lane 循环里高频
// 发生（如 OP_VOP 每 lane 索引 2~3 次），std::vector 的堆分配曾是整机第一热点——内联数组拷贝零分配。
class RegShape {
 public:
  static constexpr uint32_t kMaxDims = 6;

  RegShape() : n(0) {}
  RegShape(std::initializer_list<uint64_t> l) { assign(l.begin(), l.end()); }
  RegShape(std::vector<uint64_t> const& v) { assign(v.begin(), v.end()); }

  template <typename It>
  void assign(It first, It last) {
    n = 0;
    for (; first != last; ++first) {
      LOGCHECK(n < kMaxDims, "RegShape: too many dims.");
      d[n++] = *first;
    }
  }

  uint64_t*       begin()       { return d; }
  uint64_t*       end()         { return d + n; }
  uint64_t const* begin() const { return d; }
  uint64_t const* end()   const { return d + n; }
  uint64_t const* cbegin() const { return d; }
  uint64_t const* cend()   const { return d + n; }
  uint32_t size() const { return n; }
  bool empty() const { return n == 0; }
  uint64_t& operator[](uint32_t i)       { return d[i]; }
  uint64_t  operator[](uint32_t i) const { return d[i]; }
  uint64_t& back()       { return d[n - 1]; }
  uint64_t  back() const { return d[n - 1]; }
  operator std::vector<uint64_t>() const { return std::vector<uint64_t>(d, d + n); }

 private:
  uint64_t d[kMaxDims];
  uint32_t n;
};

// 共享字节缓冲上的带类型视图：operator[] 调 byteOffset 返回子视图、别名同一缓冲，不复制存储。
class Register {
 public:
  Register(RegShape s, DataType dtype)
      : dataType(dtype),
        shape(std::move(s)),
        eleByteWidth(DataType2Bw(dtype)),
        byteOffset(0),
        regData(std::make_shared<std::vector<uint8_t>>(
            std::accumulate(shape.begin(), shape.end(), 1ull,
                            std::multiplies<uint64_t>()) *
            eleByteWidth)) {}

  Register()
      : dataType(DataType::uint32),
        shape(),
        eleByteWidth(4),
        byteOffset(0),
        regData(nullptr) {}

  Register(Register const&) = default;
  Register(Register&&) = default;
  Register& operator=(Register const&) = default;
  Register& operator=(Register&&) = default;
  ~Register() = default;

  DataType GetDataType() const { return dataType; }
  std::vector<uint64_t> GetShape() const { return shape; }  // 冷路径:隐式转换拷出 vector
  uint64_t GetEleByteWidth() const { return eleByteWidth; }
  uint64_t GetByteWidth() const { return ShapeProduct() * eleByteWidth; }
  uint64_t GetBitWidth() const { return GetByteWidth() * 8; }
  uint64_t GetOffset() const { return byteOffset * 8; }
  uint8_t* GetStartAddr() const { return regData->data() + byteOffset; }

  bool IsScalar() const { return shape.size() == 1 && shape[0] == 1; }
  bool IsEmpty() const { return shape.size() == 0; }
  bool IsArray() const { return shape.size() == 1 && shape[0] > 1; }
  bool IsTensorOrArray() const { return shape.size() >= 1 && shape[0] > 1; }

  void Reset() { std::memset(GetStartAddr(), 0x00, GetByteWidth()); }

  Register Reshape(RegShape newShape) {
    LOGCHECK(std::accumulate(newShape.begin(), newShape.end(), 1ull,
                             std::multiplies<uint64_t>()) == ShapeProduct(),
             "");
    Register r(*this);
    r.shape = newShape;
    return r;
  }

  Register Retype(DataType dType) {
    auto oriEbw = eleByteWidth;
    auto newEbw = DataType2Bw(dType);
    auto totalBytes = shape.back() * oriEbw;
    LOGCHECK((totalBytes >= newEbw) && ((totalBytes % newEbw) == 0), "");
    shape.back() = totalBytes / newEbw;
    eleByteWidth = newEbw;
    dataType = dType;
    return *this;
  }

  int8_t   S8()      const { return ToBits().S8();      }
  uint8_t  U8()      const { return ToBits().U8();      }
  int16_t  S16()     const { return ToBits().S16();     }
  uint16_t U16()     const { return ToBits().U16();     }
  int32_t  S32()     const { return ToBits().S32();     }
  uint32_t U32()     const { return ToBits().U32();     }
  int64_t  S64()     const { return ToBits().S64();     }
  uint64_t U64()     const { return ToBits().U64();     }
  float    F32()     const { return ToBits().F32();     }
  fp16_t   F16()     const { return ToBits().F16();     }
  bf16_t   BF16()    const { return ToBits().BF16();    }
  fp8_e4m3 FP8E4M3() const { return ToBits().FP8E4M3(); }
  fp8_e5m2 FP8E5M2() const { return ToBits().FP8E5M2(); }

  template <typename T>
  void Set(T const& v) {
    LOGCHECK(GetEleByteWidth() >= sizeof(T), "Error element size when set.");
    *reinterpret_cast<T*>(GetStartAddr()) = v;
  }
  template <typename T>
  void operator=(T const& v) {
    LOGCHECK(GetEleByteWidth() >= sizeof(T), "Error element size when set.");
    *reinterpret_cast<T*>(GetStartAddr()) = v;
  }

  Register operator[](uint64_t index) const& {
    Register r;
    subView(r, index);
    r.regData = regData;
    return r;
  }
  // 右值链式索引（VREG[a][i] 的外层临时）：窃取 regData，省一对 shared_ptr 引用计数原子操作。
  Register operator[](uint64_t index) && {
    Register r;
    subView(r, index);
    r.regData = std::move(regData);
    return r;
  }

 private:
  // 填好除 regData 外的子视图字段（regData 由调用方按左值拷贝/右值窃取分别处理）。
  void subView(Register& r, uint64_t index) const {
    LOGCHECK(IsTensorOrArray(), "Register cannot be indexed");
    LOGCHECK(index < shape[0], "Access out of bounds!");
    r.dataType = dataType;
    r.eleByteWidth = eleByteWidth;
    if (IsArray()) {
      r.shape = {1};
      r.byteOffset = byteOffset + index * eleByteWidth;
    } else {
      r.shape.assign(shape.cbegin() + 1, shape.cend());
      uint64_t innerProduct =
          std::accumulate(r.shape.begin(), r.shape.end(), 1ull,
                          std::multiplies<uint64_t>());
      r.byteOffset = byteOffset + innerProduct * index * eleByteWidth;
    }
  }

  uint64_t ShapeProduct() const {
    return std::accumulate(shape.begin(), shape.end(), 1ull,
                           std::multiplies<uint64_t>());
  }

  Bits ToBits() const {
    LOGCHECK(IsScalar(), "ToBits requires a scalar register");
    LOGCHECK(GetBitWidth() <= 64, "ToBits supports up to 64 bits");
    return Bits(GetStartAddr(), static_cast<uint32_t>(GetBitWidth()));
  }

  DataType dataType;
  RegShape shape;
  uint64_t eleByteWidth;
  uint64_t byteOffset;
  std::shared_ptr<std::vector<uint8_t>> regData;
};

}  // namespace latch

#endif
