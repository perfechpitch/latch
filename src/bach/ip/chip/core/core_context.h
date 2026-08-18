#ifndef _LATCH_BACH_IP_CHIP_CORE_CORE_CONTEXT_
#define _LATCH_BACH_IP_CHIP_CORE_CORE_CONTEXT_

// 一个 Core 里各单元共用的只读上下文，在节点共有的那几样之上加了四张表。
//
// 装配期由 Core 填好，运行期各单元只读，谁也不改它。任务表、元数据、SKIP 来源与
// credit 表都是 Map 编译的产物，一次 run 内不变；参数表同理。
//
// 三张表分开放而不合并进任务表，是因为它们互不推导：任务表管每个 Core 按序干什么，
// credit 表管谁向谁申请与归还额度，元数据管若干运行时分支。

#include <cstdint>

#include "base/log.h"
#include "bach/ip/node_context.h"
#include "bach/tables/credit_table.h"
#include "bach/tables/task_meta.h"
#include "bach/tables/task_table.h"

namespace latch {
namespace bach {

struct CoreContext : NodeContext {
  // 本核属于哪个 EPGroup。dense 的核没有 group，写 -1。它决定这个核认不认某个包：
  // HitMap 里有它才算这次激活，MoEBitMap 里那一项才是它的计算倍率。
  int32_t group_id = -1;

  CoreTaskTable const* tasks = nullptr;
  TaskMetaTable const* metas = nullptr;
  SkipSourceTable const* skips = nullptr;
  CreditTable const* credits = nullptr;

  uint64_t CoreId() const { return node_id; }
  bool HasGroup() const { return group_id >= 0; }

  void Validate() const {
    ValidateNode();
    LOGCHECK(tasks != nullptr, "CoreContext: task table is null.");
    LOGCHECK(metas != nullptr, "CoreContext: meta table is null.");
    LOGCHECK(skips != nullptr, "CoreContext: skip source table is null.");
    LOGCHECK(credits != nullptr, "CoreContext: credit table is null.");
  }

  bool HasTask(uint64_t tid) const { return tasks->Has(tid); }
  TaskEntry const& Task(uint64_t tid) const { return tasks->At(tid); }
  TaskMeta const& Meta(uint64_t tid) const { return metas->Get(node_id, tid); }
  SkipSource const* Skip(uint64_t tid) const { return skips->Get(node_id, tid); }
  std::vector<int64_t> const* CreditTargets(uint64_t tid) const {
    return credits->Targets(node_id, tid);
  }
};

}
}

#endif
