#ifndef _LATCH_LOGIC_
#define _LATCH_LOGIC_

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/log.h"
#include "base/object.h"
#include "base/runtime.h"
#include "base/time_stamp.h"

namespace latch {

class LogicBaseImpl {
 public:
  virtual ~LogicBaseImpl() = default;
  virtual void Reset() = 0;
  virtual void DoAssign(LogicBaseImpl const& r) = 0;
  virtual bool DoEqual(LogicBaseImpl const& r) const = 0;
};

namespace detail {
template <typename T, typename = void>
struct has_equal_op : std::false_type {};
template <typename T>
struct has_equal_op<T, std::void_t<decltype(std::declval<T const&>() ==
                                            std::declval<T const&>())>>
    : std::true_type {};
}

template <typename T>
class LeafImpl : public LogicBaseImpl {
 public:
  using value_type = T;

  virtual T ReadValue() const = 0;
  virtual void WriteValue(T v) = 0;

  void DoAssign(LogicBaseImpl const& r) override {
    auto* leaf = dynamic_cast<LeafImpl<T> const*>(&r);
    LOGCHECK(leaf != nullptr, "LeafImpl::DoAssign: rhs is not LeafImpl<T>");
    WriteValue(leaf->ReadValue());
  }
  bool DoEqual(LogicBaseImpl const& r) const override {
    auto* leaf = dynamic_cast<LeafImpl<T> const*>(&r);
    if (leaf == nullptr) return false;
    if constexpr (detail::has_equal_op<T>::value) {
      return ReadValue() == leaf->ReadValue();
    } else {
      LOGCHECK(false, "LeafImpl::DoEqual: T has no operator==");
      return false;
    }
  }
};

class LogicBase : public Object {
 public:
  virtual ~LogicBase() = default;

  virtual std::shared_ptr<LogicBaseImpl> SharedImpl() const = 0;

  void Reset() { SharedImpl()->Reset(); }
  void AssignAs(LogicBase const& r) {
    SharedImpl()->DoAssign(*r.SharedImpl());
  }
  bool EqualsTo(LogicBase const& r) const {
    return SharedImpl()->DoEqual(*r.SharedImpl());
  }
};

template <typename T>
class LeafBase : public LogicBase {
 public:
  using value_type = T;

  T ReadValue() const {
    auto leaf = std::static_pointer_cast<LeafImpl<T>>(SharedImpl());
    return leaf->ReadValue();
  }
  void WriteValue(T v) {
    auto leaf = std::static_pointer_cast<LeafImpl<T>>(SharedImpl());
    leaf->WriteValue(v);
  }
};

template <typename U>
constexpr bool is_logic_base_v = std::is_base_of_v<LogicBase, U>;

namespace detail {

// Logic 系列的底层时序存储，不直接使用；对外一律通过 LogicCell / Logic64 /
// LogicPtr 构建。
template <typename T>
class Latch : public LeafBase<T> {
 private:

  static constexpr uint64_t INVALID_TIME = 0xffff'ffffull;

  struct Slot {
    T        data;
    uint64_t tid_time;
  };

  struct Ring {
    uint32_t                          cap;
    std::vector<std::array<Slot, 2>>  bucket;
    explicit Ring(uint32_t c) : cap(c), bucket(c) {}
  };

  struct Impl : LeafImpl<T> {
    ClockPtr                            clk;

    std::atomic<Ring*>                  cur;
    std::vector<std::unique_ptr<Ring>>  allRings;

    mutable std::shared_mutex           grow_mutex;

    Time                                clkStart;
    Time                                clkStride;

    static constexpr Time kCacheNone = std::numeric_limits<Time>::max();
    mutable std::atomic<uint32_t> cacheSeq{0};
    std::atomic<bool>             cacheBad{false};
    std::atomic<uint64_t>         cacheWriterTid{U64MAX};
    Time cacheLastT = kCacheNone, cachePrevT = kCacheNone;
    T    cacheLastV{}, cachePrevV{};

    explicit Impl(ClockPtr clock, uint32_t initial_cap)
        : clk(clock), clkStart(clock->Start()), clkStride(clock->Stride()) {
      allRings.push_back(std::make_unique<Ring>(initial_cap));
      ClearAll(*allRings.back());
      cur.store(allRings.back().get(), std::memory_order_release);
    }

    static uint64_t PackTidTime(uint64_t tid, Time t) {
      LOGCHECK(t < INVALID_TIME, "Latch: exceed max time (32-bit limit).");
      return (tid << 32) | (t & 0xffff'ffffull);
    }
    static uint64_t UnpackTime(uint64_t v) { return v & 0xffff'ffffull; }
    static uint64_t UnpackTid (uint64_t v) { return v >> 32; }

    uint32_t IndexOf(Time t, uint32_t cap) const {
      return static_cast<uint32_t>(((t - clkStart) / clkStride) & (cap - 1));
    }

    static void ClearAll(Ring& r) {
      for (auto& b : r.bucket) {
        for (auto& slot : b) {
          slot.data = T();
          slot.tid_time = INVALID_TIME;
        }
      }
    }

    void Set(T v, Time t) {
      LOGCHECK(Base   <= t, "Latch::Set: t < Base.");
      LOGCHECK(t <= Target, "Latch::Set: t > Target.");

      while (true) {
        Time bas = Base;
        {
          std::shared_lock<std::shared_mutex> lck(grow_mutex);
          Ring* r = cur.load(std::memory_order_relaxed);
          if ((t - bas) < static_cast<uint64_t>(r->cap) * clkStride) {
            WriteSlot(*r, IndexOf(t, r->cap), v, t);
            CacheWrite(v, t);
            return;
          }
        }
        Grow(t, bas);
      }
    }

    void Set(T v) { Set(v, GetTarget()); }

    void WriteSlot(Ring& r, uint32_t index, T const& v, Time t) {
      auto& bkt = r.bucket[index];
      uint64_t oldest_time = INVALID_TIME - 1;
      uint8_t  pos = 0xff;
      for (uint32_t i = 0; i < 2; ++i) {
        uint64_t packed = bkt[i].tid_time;
        uint64_t wr_time = UnpackTime(packed);
        uint64_t wr_tid  = UnpackTid (packed);
        if (wr_time == t && wr_tid == ThreadId) {
          pos = i;
          break;
        }
        LOGCHECK(!(wr_time == t && wr_tid != ThreadId),
                 "Latch: two threads writing same Latch at same time.");
        if (wr_time == INVALID_TIME) {
          pos = i;
          break;
        }
        if (wr_time < oldest_time) {
          oldest_time = wr_time;
          pos = i;
        }
      }
      LOGCHECK(pos < 2, "Latch: no slot available (bug).");
      bkt[pos].data     = v;
      bkt[pos].tid_time = PackTidTime(ThreadId, t);
    }

    void CacheWrite(T const& v, Time t) {
      if (cacheBad.load(std::memory_order_relaxed)) return;
      bool poison = false;
      const uint64_t wt = cacheWriterTid.load(std::memory_order_relaxed);
      if (wt == U64MAX) {
        cacheWriterTid.store(ThreadId, std::memory_order_relaxed);
      } else if (wt != ThreadId) {
        poison = true;
      }
      if (!poison && cacheLastT != kCacheNone && t < cacheLastT) {
        poison = true;
      }
      const uint32_t s = cacheSeq.load(std::memory_order_relaxed);
      cacheSeq.store(s + 1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      if (poison) {
        cacheLastT = kCacheNone;
        cachePrevT = kCacheNone;
      } else if (t == cacheLastT) {
        cacheLastV = v;
      } else {
        cachePrevT = cacheLastT;
        cachePrevV = cacheLastV;
        cacheLastT = t;
        cacheLastV = v;
      }
      std::atomic_thread_fence(std::memory_order_release);
      cacheSeq.store(s + 2, std::memory_order_release);
      if (poison) cacheBad.store(true, std::memory_order_release);
    }

    bool CacheRead(Time t, T& out) const {
      if (cacheBad.load(std::memory_order_acquire)) return false;
      uint32_t s1, s2;
      Time lt, pt;
      T lv, pv;
      for (;;) {
        s1 = cacheSeq.load(std::memory_order_acquire);
        if (s1 & 1u) {
#if defined(__x86_64__) || defined(__i386__)
          __builtin_ia32_pause();
#endif
          continue;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        lt = cacheLastT; pt = cachePrevT; lv = cacheLastV; pv = cachePrevV;
        std::atomic_thread_fence(std::memory_order_acquire);
        s2 = cacheSeq.load(std::memory_order_acquire);
        if (s1 == s2) break;
      }
      if (lt != kCacheNone && t >= lt) { out = lv; return true; }
      if (pt != kCacheNone && t >= pt) { out = pv; return true; }
      return false;
    }

    T Get(Time t) const {
      { T cached; if (CacheRead(t, cached)) return cached; }
      Ring* r = cur.load(std::memory_order_acquire);
      const uint32_t cap = r->cap;

      const uint64_t cap_span = static_cast<uint64_t>(cap) * clkStride;
      T        data = T();
      uint64_t last_wr_time = 0;
      bool     found_in_cap = false;
      uint32_t start_idx = IndexOf(t, cap);
      for (uint32_t i = 0; i < cap; ++i) {
        uint32_t cur_idx = (start_idx + cap - i) & (cap - 1);
        auto const& bkt = r->bucket[cur_idx];
        for (uint32_t j = 0; j < 2; ++j) {
          auto const& sd = bkt[j];
          uint64_t wr_time = UnpackTime(sd.tid_time);
          if (wr_time == INVALID_TIME) continue;
          if ((wr_time > last_wr_time && wr_time <= t) ||
              (wr_time == 0 && last_wr_time == 0)) {
            last_wr_time = wr_time;
            data = sd.data;
            if ((t - wr_time) < cap_span) {
              found_in_cap = true;
              break;
            }
          }
        }
        if (found_in_cap) break;
      }
      return data;
    }

    T Get() const {
      Time tgt = Target;
      Time bas = Base;
      LOGCHECK((bas <= tgt) || (tgt == 0),
               "Latch::Get: malformed time (Base > Target).");

      if (cacheWriterTid.load(std::memory_order_relaxed) == ThreadId ||
          cacheBad.load(std::memory_order_relaxed)) {
        Ring* r = cur.load(std::memory_order_acquire);
        uint32_t index = IndexOf(tgt, r->cap);
        auto const& bkt = r->bucket[index];
        for (auto const& sd : bkt) {
          uint64_t wr_tid  = UnpackTid (sd.tid_time);
          uint64_t wr_time = UnpackTime(sd.tid_time);
          if (wr_time == tgt && wr_tid == ThreadId) {
            return sd.data;
          }
        }
      }
      return Get(bas);
    }

    void Grow(Time t, Time bas) {
      std::unique_lock<std::shared_mutex> lck(grow_mutex);
      Ring* old = cur.load(std::memory_order_relaxed);
      if ((t - bas) < static_cast<uint64_t>(old->cap) * clk->Stride()) return;

      uint64_t needed = (t - bas) / clk->Stride() + 1;
      uint32_t new_cap = old->cap;
      while (new_cap < needed) new_cap *= 2;

      auto nr = std::make_unique<Ring>(new_cap);
      ClearAll(*nr);

      for (auto const& old_bucket : old->bucket) {
        for (auto const& slot : old_bucket) {
          uint64_t wr_time = UnpackTime(slot.tid_time);
          if (wr_time == INVALID_TIME) continue;
          uint32_t new_idx = IndexOf(wr_time, new_cap);
          auto& new_bucket = nr->bucket[new_idx];
          uint64_t oldest = INVALID_TIME - 1;
          uint8_t  pos = 0xff;
          for (uint32_t i = 0; i < 2; ++i) {
            uint64_t cur_t = UnpackTime(new_bucket[i].tid_time);
            if (cur_t == INVALID_TIME) { pos = i; break; }
            if (cur_t < oldest) { oldest = cur_t; pos = i; }
          }
          LOGCHECK(pos < 2, "Latch::Grow: no slot in new bucket (bug).");
          new_bucket[pos] = slot;
        }
      }

      Ring* np = nr.get();
      allRings.push_back(std::move(nr));
      cur.store(np, std::memory_order_release);
    }

    T    ReadValue()  const override { return Get(); }
    void WriteValue(T v)    override { Set(v); }
    void Reset()            override {
      std::unique_lock<std::shared_mutex> lck(grow_mutex);
      ClearAll(*cur.load(std::memory_order_relaxed));

      const uint32_t s = cacheSeq.load(std::memory_order_relaxed);
      cacheSeq.store(s + 1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      cacheLastT = kCacheNone;
      cachePrevT = kCacheNone;
      std::atomic_thread_fence(std::memory_order_release);
      cacheSeq.store(s + 2, std::memory_order_release);
      cacheWriterTid.store(U64MAX, std::memory_order_release);
      cacheBad.store(false, std::memory_order_release);
    }
  };

  std::shared_ptr<Impl> p;

 public:
  static constexpr uint32_t kInitialCap = 4;

  static uint32_t RoundUpPow2(uint32_t v) {
    if (v <= 2) return 2;
    return 1u << (32 - __builtin_clz(v - 1));
  }

  explicit Latch(ClockPtr clk)
      : p(std::make_shared<Impl>(clk, kInitialCap)) {}
  Latch(ClockPtr clk, uint32_t hint)
      : p(std::make_shared<Impl>(clk, RoundUpPow2(hint))) {}
  Latch(Latch const&)            = default;
  Latch(Latch&&) noexcept         = default;

  std::shared_ptr<LogicBaseImpl> SharedImpl() const override { return p; }

  void Set(T v)         { p->Set(v); }
  void Set(T v, Time t) { p->Set(v, t); }
  T    Get()       const { return p->Get(); }
  T    Get(Time t) const { return p->Get(t); }

  Latch& operator=(T v) { p->Set(v); return *this; }
  Latch& operator=(Latch const& r) {
    if (this != &r) this->AssignAs(r);
    return *this;
  }
  Latch& operator=(LogicBase const& r) {
    if (static_cast<LogicBase const*>(this) != &r) this->AssignAs(r);
    return *this;
  }
  operator T() const { return Get(); }
};

}

class Logic : public LogicBase {
 protected:
  struct Impl : LogicBaseImpl {
    std::vector<std::shared_ptr<LogicBaseImpl>> children;

    void Reset() override {
      for (auto& c : children) c->Reset();
    }
    void DoAssign(LogicBaseImpl const& r) override {
      auto* o = dynamic_cast<Impl const*>(&r);
      LOGCHECK(o != nullptr, "Logic::DoAssign: rhs is not a Logic");
      LOGCHECK(children.size() == o->children.size(),
               "Logic::DoAssign: children count mismatch");
      for (size_t i = 0; i < children.size(); ++i)
        children[i]->DoAssign(*o->children[i]);
    }
    bool DoEqual(LogicBaseImpl const& r) const override {
      auto* o = dynamic_cast<Impl const*>(&r);
      if (o == nullptr) return false;
      if (children.size() != o->children.size()) return false;
      for (size_t i = 0; i < children.size(); ++i)
        if (!children[i]->DoEqual(*o->children[i])) return false;
      return true;
    }
  };

  std::shared_ptr<Impl> p;

 public:
  Logic() : p(std::make_shared<Impl>()) {}
  Logic(Logic const&)            = default;
  Logic(Logic&&) noexcept         = default;

  std::shared_ptr<LogicBaseImpl> SharedImpl() const override { return p; }

  template <typename... Cs>
  void Fields(Cs&... cs) {
    static_assert((std::is_base_of_v<LogicBase, Cs> && ...),
                  "Fields(): every argument must derive from LogicBase");
    (p->children.push_back(cs.SharedImpl()), ...);
  }

  Logic& operator=(Logic const& r) {
    if (this != &r) this->AssignAs(r);
    return *this;
  }
  Logic& operator=(LogicBase const& r) {
    if (static_cast<LogicBase const*>(this) != &r) this->AssignAs(r);
    return *this;
  }

  uint64_t       ChildCount() const  { return p->children.size(); }
  LogicBaseImpl& At(uint64_t i)      { return *p->children.at(i); }
};

template <typename T>
class ValueCell : public LeafBase<T> {
 private:
  struct Impl : LeafImpl<T> {
    T v{};
    T    ReadValue()  const override { return v; }
    void WriteValue(T val)  override { v = val; }
    void Reset()            override { v = T(); }
  };
  std::shared_ptr<Impl> p;

 public:
  ValueCell()     : p(std::make_shared<Impl>()) {}
  ValueCell(T val) : p(std::make_shared<Impl>()) { p->v = val; }
  ValueCell(ValueCell const&) = default;

  std::shared_ptr<LogicBaseImpl> SharedImpl() const override { return p; }

  void Set(T v)    { p->WriteValue(v); }
  T    Get() const { return p->ReadValue(); }

  ValueCell& operator=(T v) { Set(v); return *this; }
  ValueCell& operator=(ValueCell const& r) {
    if (this != &r) this->AssignAs(r);
    return *this;
  }
  ValueCell& operator=(LogicBase const& r) {
    if (static_cast<LogicBase const*>(this) != &r) this->AssignAs(r);
    return *this;
  }
  operator T() const { return Get(); }

  template <typename U = T>
  std::enable_if_t<std::is_arithmetic_v<U>, ValueCell&> operator++() {
    Set(Get() + 1); return *this;
  }
  template <typename U = T>
  std::enable_if_t<std::is_arithmetic_v<U>, ValueCell&> operator--() {
    Set(Get() - 1); return *this;
  }
  template <typename U = T>
  std::enable_if_t<std::is_arithmetic_v<U>, ValueCell&> operator+=(U v) {
    Set(Get() + v); return *this;
  }
  template <typename U = T>
  std::enable_if_t<std::is_arithmetic_v<U>, ValueCell&> operator-=(U v) {
    Set(Get() - v); return *this;
  }
  template <typename U = T>
  std::enable_if_t<std::is_arithmetic_v<U>, ValueCell&> operator*=(U v) {
    Set(Get() * v); return *this;
  }
  template <typename U = T>
  std::enable_if_t<std::is_arithmetic_v<U>, ValueCell&> operator/=(U v) {
    Set(Get() / v); return *this;
  }
};

// 带时间维度的单值单元：按时钟节拍保存多个版本，可按时间戳回读。
template <typename T>
class LogicCell : public Logic {
 private:
  detail::Latch<T> value;

 public:
  explicit LogicCell(ClockPtr clk) : value(clk) {
    Fields(value);
  }
  LogicCell(ClockPtr clk, uint32_t hint) : value(clk, hint) {
    Fields(value);
  }
  LogicCell(LogicCell const&) = default;

  void Set(T v)         { value.Set(v); }
  void Set(T v, Time t) { value.Set(v, t); }
  T    Get()       const { return value.Get(); }
  T    Get(Time t) const { return value.Get(t); }

  LogicCell& operator=(T v) { value.Set(v); return *this; }
  LogicCell& operator=(LogicCell const& r) {
    if (this != &r) this->AssignAs(r);
    return *this;
  }
  LogicCell& operator=(LogicBase const& r) {
    if (static_cast<LogicBase const*>(this) != &r) this->AssignAs(r);
    return *this;
  }
  operator T() const { return value.Get(); }

  template <typename U = T>
  std::enable_if_t<std::is_arithmetic_v<U>, LogicCell&> operator++() {
    Set(Get() + 1); return *this;
  }
  template <typename U = T>
  std::enable_if_t<std::is_arithmetic_v<U>, LogicCell&> operator--() {
    Set(Get() - 1); return *this;
  }
  template <typename U = T>
  std::enable_if_t<std::is_arithmetic_v<U>, LogicCell&> operator+=(U v) {
    Set(Get() + v); return *this;
  }
  template <typename U = T>
  std::enable_if_t<std::is_arithmetic_v<U>, LogicCell&> operator-=(U v) {
    Set(Get() - v); return *this;
  }
  template <typename U = T>
  std::enable_if_t<std::is_arithmetic_v<U>, LogicCell&> operator*=(U v) {
    Set(Get() * v); return *this;
  }
  template <typename U = T>
  std::enable_if_t<std::is_arithmetic_v<U>, LogicCell&> operator/=(U v) {
    Set(Get() / v); return *this;
  }
};

using Logic64 = LogicCell<uint64_t>;

template <typename T>
class LogicPtr : public LogicCell<std::shared_ptr<T>> {
 private:
  using Cell = LogicCell<std::shared_ptr<T>>;

 public:
  explicit LogicPtr(ClockPtr clk) : Cell(clk) {}
  LogicPtr(LogicPtr const&) = default;

  using Cell::operator=;
  LogicPtr& operator=(LogicPtr const& r) {
    if (this != &r) this->AssignAs(r);
    return *this;
  }
  LogicPtr& operator=(LogicBase const& r) {
    if (static_cast<LogicBase const*>(this) != &r) this->AssignAs(r);
    return *this;
  }

  T*       operator->() const    { return Cell::Get().get(); }
  explicit operator bool() const { return Cell::Get() != nullptr; }
};

template <typename T>
class LogicVec : public Logic {
  static_assert(std::is_base_of_v<LogicBase, T>,
                "LogicVec<T>: T must derive from LogicBase");
  std::vector<T> elems;

 public:
  template <typename... Args>
  LogicVec(uint64_t len, ClockPtr clk, Args&&... args) {
    elems.reserve(len);
    for (uint64_t i = 0; i < len; ++i)
      elems.emplace_back(clk, std::forward<Args>(args)...);
    for (auto& e : elems) Fields(e);
  }

  T&       operator[](uint64_t i)       { return elems[i]; }
  T const& operator[](uint64_t i) const { return elems[i]; }
  uint64_t size() const                 { return elems.size(); }
};

}

#endif
