#ifndef _LATCH_BACH_TABLES_CREDIT_TABLE_
#define _LATCH_BACH_TABLES_CREDIT_TABLE_

// credit 表：一条 CU 任务要向哪几个下游一次性验资。
//
// 它跟任务表是两张互不推导的表。任务表管每个 Core 按序干什么，credit 表管谁向谁
// 申请与归还额度，同一条 CU 行的全部下游要同时到位才放行。表里只有 CU 行有条目。

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "bach/tables/task_meta.h"

namespace latch {
namespace bach {

class CreditTable {
 public:
  void Set(uint64_t core_id, uint64_t task_id, std::vector<int64_t> targets) {
    items[detail::TaskKey(core_id, task_id)] = std::move(targets);
  }

  // 不是 CU 行就返回空指针。
  std::vector<int64_t> const* Targets(uint64_t core_id, uint64_t task_id) const {
    auto it = items.find(detail::TaskKey(core_id, task_id));
    return it == items.end() ? nullptr : &it->second;
  }

  uint64_t Size() const { return items.size(); }

 private:
  std::unordered_map<uint64_t, std::vector<int64_t>> items;
};

}
}

#endif
