#ifndef _LATCH_BACH_TABLES_TASK_TABLE_
#define _LATCH_BACH_TABLES_TASK_TABLE_

// 每个 Core 一张顺序任务表。它是 Map 编译的产物，构造期读入，运行期只读。
//
// 一个条目就是中间文件里的一条 TASK 记录。字段的可读范围跟着 unit 走，读错位置
// 会拿到一个有值但无意义的数：
//
//   up_cid    只有 RETIRE 行有意义，-1 表示上游是 Host，此时 dst 是 Host 的坐标
//   down_cid  只有 DTE 行有意义，目标是外部节点时它是 -1，看 down_ext
//   dst       路由只看这一项，坐标落在核阵列外就是某个外部节点
//
// TaskScheduler 的取指循环靠 Has() 判断表跑完了没有，对应 Bach 里 lookup 抛
// IndexError 那个出口。

#include <cstdint>
#include <vector>

#include "base/log.h"
#include "bach/common/packet.h"

namespace latch {
namespace bach {

// 没有外部节点目标时 down_ext 取这个值。
constexpr uint32_t kNoExtNode = 0xffff'ffffu;

struct TaskEntry {
  UnitType unit = UnitType::kSkip;
  Opcode opcode = Opcode::kDontCare;
  uint64_t tag = 0;
  int64_t up_cid = 0;
  int64_t down_cid = -1;
  uint32_t down_ext = kNoExtNode;
  Coord dst;
  uint64_t time_or_vol = 0;

  bool TargetsExtNode() const { return down_ext != kNoExtNode; }
};

class CoreTaskTable {
 public:
  CoreTaskTable() = default;

  void Append(TaskEntry const& e) { entries.push_back(e); }

  uint64_t Size() const { return entries.size(); }
  bool Has(uint64_t task_id) const { return task_id < entries.size(); }

  TaskEntry const& At(uint64_t task_id) const {
    LOGCHECK(task_id < entries.size(), "CoreTaskTable: task id out of range.");
    return entries[task_id];
  }

  // 只给构造期回填前向引用用，运行期一律走 At。
  TaskEntry& Mutable(uint64_t task_id) {
    LOGCHECK(task_id < entries.size(), "CoreTaskTable: task id out of range.");
    return entries[task_id];
  }

  std::vector<TaskEntry> const& All() const { return entries; }

 private:
  std::vector<TaskEntry> entries;
};

}
}

#endif
