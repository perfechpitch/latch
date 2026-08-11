#ifndef _LATCH_CACHE_
#define _LATCH_CACHE_

#error "module/cache.h is pre-refactor; see comment above before including."

#include "base/clock.h"
#include "base/logic.h"
#include "base/module.h"
#include "base/runtime.h"

namespace latch {

union CtrlBits {
  CtrlBits(uint64_t data = 0) : bits(data) {}
  uint64_t bits;
  struct {
    uint8_t valid : 1;
    uint8_t pnd : 1;
    uint8_t lock : 1;
    uint8_t dirty : 1;
    uint16_t refcount : 16;
  } fields;

  bool IsPending() const {
    return fields.valid == 0 && fields.pnd == 1 && fields.lock == 1;
  }
  bool IsValid() const { return fields.valid == 1 && fields.pnd == 0; }
  bool IsDirty() const { return fields.dirty == 1; }
  bool IsLocked() const { return fields.lock == 1; }

  bool IsBlank() const { return bits == 0; }
  bool IsPendClean() const { return IsPending() && !IsDirty(); }
  bool IsPendDirty() const { return IsPending() && IsDirty(); }
  bool IsValidClean() const { return IsValid() && !IsDirty(); }
  bool IsValidDirty() const { return IsValid() && IsDirty(); }
  bool IsValidLock() const { return IsValid() && IsLocked(); }
  bool IsValidUnlock() const { return IsValid() && !IsLocked(); }

  void ToDirty() {
    LOGCHECK(!IsBlank(), "Can not set dirty");
    fields.dirty = 1;
  }
  void ToClean() {
    LOGCHECK(!IsBlank() && !IsPendDirty(), "Can not set clean");
    fields.dirty = 0;
  }
  void ToLock() {
    LOGCHECK(!IsBlank(), "Can not set lock");
    fields.lock = 1;
  }
  void ToUnlock() {
    LOGCHECK(!IsBlank() && !IsPending(), "Can not set unlock");
    fields.lock = 0;
  }

  void ToPending() {
    LOGCHECK(!IsBlank(), "Can not set pnd");
    fields.valid = 0;
    fields.pnd = 1;
    fields.lock = 1;
  }
  void ToPendClean() {
    LOGCHECK(IsPendClean() || IsBlank() || IsValidUnlock(),
             "Can not set pending");
    fields.valid = 0;
    fields.pnd = 1;
    fields.lock = 1;
    fields.dirty = 0;
  }
  void ToValidLock() {
    LOGCHECK(IsValidLock() || IsPending() || IsValidUnlock(),
             "Can not set valid lock");
    fields.valid = 1;
    fields.pnd = 0;
    fields.lock = 1;
  }
  void ToValidUnlock() {
    LOGCHECK(IsValidUnlock() || IsValidLock() || IsPending(),
             "Can not set valid unlock");
    fields.valid = 1;
    fields.pnd = 0;
    fields.lock = 0;
  }
  void ToBlank() {
    LOGCHECK(IsBlank() || IsValidUnlock(), "Can not set blank");
    bits = 0;
  }

  void AdjustRefcount(int32_t delta) {
    int32_t count = fields.refcount + delta;
    LOGCHECK(count >= 0, "refcount can not be negative");
    fields.refcount = count;
  }

  void Reset() { bits = 0; }
};

template <uint64_t numStruct, typename T>
class CacheStructData : public LogicPtr<T> {
 public:
  CacheStructData(ClockPtr clk = nullptr) : LogicPtr<T>(clk) {
    for (uint64_t i = 0; i < numStruct; i++) {
      data.push_back(LogicPtr<T>(clk));
      this->AppendChildLogic(data.at(i));
    }
  }

  void Write(uint64_t index, std::unique_ptr<T> d) {
    LOGCHECK(d != nullptr, "data is nullptr");
    auto d_copy = std::shared_ptr<T>(std::move(d));
    data.at(index) = d_copy;
  }

  std::shared_ptr<const T> Read(uint64_t index) {
    std::shared_ptr<T> d = data.at(index);
    return d;
  }

 private:
  std::vector<LogicPtr<T>> data;
};

template <uint64_t bankNum, uint64_t bankWidth>
using CacheBankData = CacheStructData<bankNum, std::array<uint8_t, bankWidth>>;

enum class Action { Dirty, Clean, Pending, Unpending };

template <typename TTAG, typename TDATA>
class LogicCache {
 public:
  static_assert(std::is_base_of<Logic, TTAG>::value ||
                    is_logic_ptr<TTAG>::value,
                "TTAG must be a subclass of Logic or LogicPtr");
  static_assert(is_derived_from_logicbase<TDATA>::value,
                "TDATA must be a subclass of Logic");
  struct Cacheline {
    Cacheline(Logic Ctrl, TTAG Tag, TDATA Data)
        : ctrl(Ctrl), tag(Tag), data(Data) {}
    Logic ctrl;
    TTAG tag;
    TDATA data;
  };

  struct LookupResult {
    LookupResult(bool Find, uint64_t Way) : find(Find), way(Way) {}
    bool find;
    uint64_t way;
  };

  struct VictimResult {
    VictimResult(bool Find, uint64_t Way, std::shared_ptr<Cacheline> Victim)
        : find(Find), way(Way), victim(Victim) {}
    bool find;
    uint64_t way;
    std::shared_ptr<Cacheline> victim;
  };

  LogicCache(ClockPtr clock, uint64_t setNum, uint64_t wayNum)
      : setNum(setNum), wayNum(wayNum), lruSts(setNum) {
    for (uint64_t i = 0; i < setNum; i++) {
      for (uint64_t j = 0; j < wayNum; j++) {
        Logic ctrl(clock);
        TTAG tag;
        TDATA data;
        data.SetLatchClock(clock);
        cache.push_back({ctrl, tag, data});

        lruSts[i].push_back(j);
      }
    }
  }

  LookupResult Lookup(uint64_t set, TTAG const& tag,
                      bool only_check_undirty = false) {
    for (uint32_t i = 0; i < wayNum; i++) {
      CtrlBits ctrlBits;
      ctrlBits.bits = cache.at(set * wayNum + i).ctrl;
      bool is_dirtry = ctrlBits.IsDirty();
      bool dirty_check = only_check_undirty ? !is_dirtry : true;
      if (!ctrlBits.IsBlank() && dirty_check &&
          Equals(cache.at(set * wayNum + i).tag, tag)) {
        UpdateLruStatus(set, i);
        return {true, i};
      }
    }
    return {false, 0};
  }

  VictimResult Victim(uint64_t set, TTAG const& new_tag) {
    CtrlBits ctrlBits;
    std::shared_ptr<Cacheline> cacheline = nullptr;

    for (auto&& i : GetVictimOrder(set)) {
      UpdateLruStatus(set, i);
      ctrlBits.bits = cache.at(set * wayNum + i).ctrl;
      if (ctrlBits.IsDirty()) {
        Logic ctrl;
        TTAG tag;
        TDATA data;
        ctrl.AssignAs(cache.at(set * wayNum + i).ctrl);
        tag.AssignAs(cache.at(set * wayNum + i).tag);
        data.AssignAs(cache.at(set * wayNum + i).data);
        cacheline = std::make_shared<Cacheline>(ctrl, tag, data);
      }

      ctrlBits.ToPendClean();
      cache.at(set * wayNum + i).ctrl = ctrlBits.bits;
      cache.at(set * wayNum + i).tag.AssignAs(new_tag);
      return {true, i, cacheline};
    }
    return {false, 0, cacheline};
  }

  void DecreaseRefcount(uint64_t set, uint64_t way,
                        uint16_t numAccessCompleted) {
    CtrlBits ctrlBits;
    ctrlBits.bits = cache.at(set * wayNum + way).ctrl;
    LOGCHECK(ctrlBits.IsValidLock() || ctrlBits.IsPending(),
             "cacheline read/write access must be locked.");
    ctrlBits.AdjustRefcount(-numAccessCompleted);
    if (ctrlBits.fields.refcount == 0 && ctrlBits.IsValidLock()) {
      ctrlBits.ToValidUnlock();
    }
    cache.at(set * wayNum + way).ctrl = ctrlBits.bits;
  }
  void IncreaseRefcount(uint64_t set, uint64_t way, uint16_t numAccessRequest) {
    CtrlBits ctrlBits;
    ctrlBits.bits = cache.at(set * wayNum + way).ctrl;
    LOGCHECK(!ctrlBits.IsBlank(),
             "cacheline read/write access must not be blank.");
    ctrlBits.AdjustRefcount(numAccessRequest);
    if (ctrlBits.fields.refcount > 0 && ctrlBits.IsValidUnlock()) {
      ctrlBits.ToValidLock();
    }
    cache.at(set * wayNum + way).ctrl = ctrlBits.bits;
  }

  void UpdateCtrlBits(uint64_t set, uint64_t way, Action action) {
    CtrlBits ctrlBits;
    ctrlBits.bits = cache.at(set * wayNum + way).ctrl;
    switch (action) {
      case Action::Dirty: {
        ctrlBits.ToDirty();
        cache.at(set * wayNum + way).ctrl = ctrlBits.bits;
        break;
      }
      case Action::Clean: {
        ctrlBits.ToClean();
        cache.at(set * wayNum + way).ctrl = ctrlBits.bits;
        break;
      }
      case Action::Pending: {
        ctrlBits.ToPending();
        cache.at(set * wayNum + way).ctrl = ctrlBits.bits;
        break;
      }
      case Action::Unpending: {
        if (ctrlBits.fields.refcount > 0) {
          ctrlBits.ToValidLock();
        } else {
          ctrlBits.ToValidUnlock();
        }
        cache.at(set * wayNum + way).ctrl = ctrlBits.bits;
        break;
      }
      default:
        break;
    }
  }

  TDATA& Data(uint64_t set, uint64_t way) {
    return cache.at(set * wayNum + way).data;
  }
  TTAG& Tag(uint64_t set, uint64_t way) {
    return cache.at(set * wayNum + way).tag;
  }
  CtrlBits Ctrl(uint64_t set, uint64_t way) {
    CtrlBits ctrlBits;
    ctrlBits.bits = cache.at(set * wayNum + way).ctrl;
    return ctrlBits;
  }

  void Reset() {
    TTAG tag;
    for (uint64_t i = 0; i < setNum; i++) {
      for (uint64_t j = 0; j < wayNum; j++) {
        cache.at(i * wayNum + j).ctrl = 0;
        cache.at(i * wayNum + j).tag.AssignAs(tag);
        cache.at(i * wayNum + j).data.Reset();
      }
      lruSts.at(i).sort();
    }
  }
  void Reset(uint64_t set, uint64_t way) {
    TTAG tag;
    cache.at(set * wayNum + way).ctrl = 0;
    cache.at(set * wayNum + way).tag.AssignAs(tag);
    cache.at(set * wayNum + way).data.Reset();
    auto&& setLru = lruSts.at(set);
    auto it = std::find(setLru.begin(), setLru.end(), way);
    if (it != setLru.end()) {
      setLru.splice(setLru.begin(), setLru, it);
    } else {
      LOGCHECK(false, "way not found in lru");
    }
  }

 protected:
  void UpdateLruStatus(uint64_t set, uint64_t way) {
    auto&& setLru = lruSts.at(set);
    auto it = std::find(setLru.begin(), setLru.end(), way);
    if (it != setLru.end()) {
      setLru.splice(setLru.end(), setLru, it);
    } else {
      LOGCHECK(false, "way not found in lru");
    }
  }

  virtual std::vector<uint64_t> GetVictimOrder(uint64_t set) {
    std::vector<uint64_t> victim_order;
    for (auto&& way : lruSts.at(set)) {
      if (IsEvictable(set, way)) {
        victim_order.push_back(way);
      }
    }
    return victim_order;
  }

 private:
  bool IsEvictable(uint64_t set, uint64_t way) {
    CtrlBits ctrlBits;
    ctrlBits.bits = cache.at(set * wayNum + way).ctrl;
    return ctrlBits.IsValidUnlock() || ctrlBits.IsBlank();
  }

  template <typename U = TTAG>
  std::enable_if_t<std::is_base_of<Logic, U>::value, bool> Equals(
      TTAG const& left, TTAG const& right) const {
    return left == right;
  }

  template <typename U = TTAG>
  std::enable_if_t<is_logic_ptr<U>::value, bool> Equals(
      TTAG const& left, TTAG const& right) const {
    auto left_ptr = left.Get();
    auto right_ptr = right.Get();
    if (left_ptr == nullptr || right_ptr == nullptr) {
      return false;
    }
    return *left_ptr == *right_ptr;
  }

  uint64_t setNum;
  uint64_t wayNum;
  std::vector<Cacheline> cache;
  std::vector<std::list<uint64_t>> lruSts;
};

}
#endif
