#ifndef _LATCH_BACH_TABLES_TASK_META_
#define _LATCH_BACH_TABLES_TASK_META_

// 任务元数据与 SKIP 行的来源表。两张都是稀疏的：只有取值非空的任务才有条目，
// 查不到就返回一份全默认的值。
//
// 元数据不参与时间计算，但决定若干运行时分支，所以它跟任务表是两张表，不能合并
// 进 TaskEntry 之后按位判断。SKIP 来源表另记每条屏障行的上游是谁、那条入边下面
// 挂着哪些 group，运行时用它判断这条入边在本次 HitMap 下会不会真的来包。

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace latch {
namespace bach {

// 任务元数据里 semantic_op 的取值。
enum class SemanticOp : uint32_t {
  kNone = 0,
  kMoeSend = 1,
  kRouterSoftmaxTopk = 2,
  kConcatRouterLogits = 3,
};

// 五路径模式下任务元数据显式声明的路径。
enum class DsaRoute : uint32_t {
  kNone = 0,
  kR2M = 1,
  kR2C = 2,
  kM2R = 3,
  kC2R = 4,
  kM2C = 5,
};

// M2C 要求端点是 Matrix 到 Core。
enum class Endpoint : uint32_t {
  kNone = 0,
  kMatrix = 1,
  kCore = 2,
};

struct TaskMeta {
  bool recv_init = false;
  bool no_credit_return = false;

  // 三个键都表示 RES_SUM 走本地计算分支，分开存是为了保住原样，不做归并。
  bool local = false;
  bool local_compute = false;
  bool res_sum_local = false;

  SemanticOp semantic_op = SemanticOp::kNone;
  bool require_dynamic_hitmap = false;
  bool dynamic_hitmap = false;
  std::string hitmap_source;

  DsaRoute dsa_route = DsaRoute::kNone;
  bool dsa_local_transfer = false;
  Endpoint source_endpoint = Endpoint::kNone;
  Endpoint destination_endpoint = Endpoint::kNone;

  uint64_t wire_tag = 0;
  std::string payload_role;

  bool IsLocalReduce() const { return local || local_compute || res_sum_local; }
};

struct SkipSource {
  int64_t sender = -1;
  std::string phase_type;
  uint32_t phase_idx = 0;
  std::vector<uint32_t> sender_group_ids;

  bool IsReductionPhase() const { return phase_type == "REDUCTION"; }
};

namespace detail {
inline uint64_t TaskKey(uint64_t core_id, uint64_t task_id) {
  return (core_id << 32) | (task_id & 0xffff'ffffull);
}
}

class TaskMetaTable {
 public:
  void Set(uint64_t core_id, uint64_t task_id, TaskMeta const& m) {
    items[detail::TaskKey(core_id, task_id)] = m;
  }

  TaskMeta* Find(uint64_t core_id, uint64_t task_id) {
    auto it = items.find(detail::TaskKey(core_id, task_id));
    return it == items.end() ? nullptr : &it->second;
  }

  // 查不到就给一份全默认的，调用方不需要先判存在。
  TaskMeta const& Get(uint64_t core_id, uint64_t task_id) const {
    static const TaskMeta kEmpty;
    auto it = items.find(detail::TaskKey(core_id, task_id));
    return it == items.end() ? kEmpty : it->second;
  }

  bool Has(uint64_t core_id, uint64_t task_id) const {
    return items.count(detail::TaskKey(core_id, task_id)) != 0;
  }

  uint64_t Size() const { return items.size(); }

 private:
  std::unordered_map<uint64_t, TaskMeta> items;
};

class SkipSourceTable {
 public:
  void Set(uint64_t core_id, uint64_t task_id, SkipSource const& s) {
    items[detail::TaskKey(core_id, task_id)] = s;
  }

  // 只有屏障 SKIP 行有来源，其余行返回空指针。
  SkipSource const* Get(uint64_t core_id, uint64_t task_id) const {
    auto it = items.find(detail::TaskKey(core_id, task_id));
    return it == items.end() ? nullptr : &it->second;
  }

  uint64_t Size() const { return items.size(); }

 private:
  std::unordered_map<uint64_t, SkipSource> items;
};

}
}

#endif
