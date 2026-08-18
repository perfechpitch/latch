#ifndef _LATCH_BACH_OBSERVER_SPAN_RECORDER_
#define _LATCH_BACH_OBSERVER_SPAN_RECORDER_

// 观测事件的记录。模型的结论全建立在它发出的事件上，所以观测点是模型的一部分，
// 但它只能旁路记录，不得改变任何时序：这里的每个方法都只往裸 vector 里追加，
// 不申请资源、不推进时间、不影响任何判定。完成判据自己数完成的 user，不从这里读。
//
// 两层：
//
//   SpanRecorder  每个挂时钟的模块一个，是它的 Cycle 独占状态，所以用裸 vector，
//                 不需要任何同步
//   RunRecorder   主线程侧，JoinAll 之后把各模块的记录收上来、配对、排序
//
// 之所以不在仿真期落盘：Cycle 体内除起手的 DelayCycle(1) 外不能再让出，文件 IO
// 放进去会拖住整个 runtime 的每拍推进。事件量按 Bach 的实测大约是 35 乘 Core 数
// 乘 user 数，三十二核十六 user 在两万条量级，先驻留内存，规模上去了再考虑分片。
//
// 两条过滤规则照搬 Bach，写在发射端而不是消费端：
//
//   占用区间  end 小于 start 的丢弃；end 等于 start 且没有显式允许零长的丢弃
//   等待区间  end 不大于 start 的丢弃
//
// 零长的占用不是没有发生，而是没有可归因的时长，让它进桶会把占用率算高。

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "base/time_stamp.h"

namespace latch {
namespace bach {

// 事件挂在哪个单元名下。名字与 Bach 的 unit 字段对齐，占用分析按它归类。
enum class Unit : uint32_t {
  kScheduler = 0,
  kDte = 1,
  kMatrix = 2,
  kVector = 3,
  kCredit = 4,
  kMemory = 5,
  kBitmap = 6,
  kRouter = 7,
  kHost = 8,
  kOut = 9,
  kSwitch = 10,
  kEth = 11,
  kLane = 12,
  kSink = 13,
};

inline const char* UnitName(Unit u) {
  switch (u) {
    case Unit::kScheduler: return "TS";
    case Unit::kDte: return "DTE";
    case Unit::kMatrix: return "MC";
    case Unit::kVector: return "VC";
    case Unit::kCredit: return "CU";
    case Unit::kMemory: return "MEM";
    case Unit::kBitmap: return "BITMAP";
    case Unit::kRouter: return "ROUTER";
    case Unit::kHost: return "HOST";
    case Unit::kOut: return "OUT";
    case Unit::kSwitch: return "PCIE_SW";
    case Unit::kEth: return "ETH_SW";
    case Unit::kLane: return "PHASE1_LANE";
    default: return "PHASE1_SINK";
  }
}

// 这一段占用属于哪个阶段。setup 与 execution 分开记，是因为它们是两级流水，
// 合起来记就看不出准入窗与执行通道各自的压力。
enum class SpanState : uint32_t {
  kSetup = 0,
  kExecution = 1,
  kCompute = 2,
  kTransfer = 3,
  kFunction = 4,
  kMemoryService = 5,
  kDecode = 6,
};

inline const char* SpanStateName(SpanState s) {
  switch (s) {
    case SpanState::kSetup: return "setup";
    case SpanState::kExecution: return "execution";
    case SpanState::kCompute: return "compute";
    case SpanState::kTransfer: return "transfer";
    case SpanState::kFunction: return "function";
    case SpanState::kMemoryService: return "memory_service";
    default: return "decode";
  }
}

// 等待归因。分类照搬 Bach，这是瓶颈能定位到具体资源而不只是慢的原因。
// 前三类是依赖，后面是资源与反压，两者的处置完全不同：依赖等的是别人算完，
// 资源等的是容量不够。
enum class WaitReason : uint32_t {
  kOwnPrevTask = 0,        // 等自身前序任务
  kStreamPredecessor = 1,  // 等前一 stream 的对应任务
  kIncomingBarrier = 2,    // 等对端数据到达
  kStreamSlot = 3,         // 等 stream 槽位
  kSetupAhead = 4,         // 等准入令牌
  kCoreSetup = 5,          // 等 setup 单元
  kExecutionLane = 6,      // 等执行通道
  kFunctionUnit = 7,       // 等 FIFO 或 reduction 功能单元
  kMemoryArbiter = 8,      // 等内存仲裁器
  kBitmapAccess = 9,       // 等 MoEBitMap 访问口
  kCreditLock = 10,        // 等查账锁
  kDownstreamCredit = 11,  // 等下游 credit
  kHostCredit = 12,        // 等 Host credit
  kDsaRoute = 13,          // 等五路径仲裁放行
  kEthEgress = 14,         // 等以太网交换节点的出口发完上一包
  kPhaseJoin = 15,         // 两路先到的那一路等另一路
};

inline const char* WaitReasonName(WaitReason r) {
  switch (r) {
    case WaitReason::kOwnPrevTask: return "own_prev_task";
    case WaitReason::kStreamPredecessor: return "stream_predecessor";
    case WaitReason::kIncomingBarrier: return "incoming_barrier";
    case WaitReason::kStreamSlot: return "stream_slot";
    case WaitReason::kSetupAhead: return "setup_ahead";
    case WaitReason::kCoreSetup: return "core_setup";
    case WaitReason::kExecutionLane: return "execution_lane";
    case WaitReason::kFunctionUnit: return "function_unit";
    case WaitReason::kMemoryArbiter: return "memory_arbiter";
    case WaitReason::kBitmapAccess: return "bitmap_access";
    case WaitReason::kCreditLock: return "credit_lock";
    case WaitReason::kDownstreamCredit: return "downstream_credit";
    case WaitReason::kHostCredit: return "host_credit";
    case WaitReason::kDsaRoute: return "dsa_route";
    case WaitReason::kEthEgress: return "eth_egress";
    default: return "phase_join";
  }
}

// 等待归因是依赖还是资源。占用分析要分开看这两类，改参数只动得了资源那一半。
inline bool IsDependencyWait(WaitReason r) {
  return r == WaitReason::kOwnPrevTask ||
         r == WaitReason::kStreamPredecessor ||
         r == WaitReason::kIncomingBarrier;
}

struct UnitSpan {
  uint64_t node_id = 0;  // Core id，或外部节点的编号
  Unit unit = Unit::kDte;
  uint64_t uid = 0;
  uint64_t tid = 0;
  Time start = 0;
  Time end = 0;
  SpanState state = SpanState::kExecution;
  uint64_t volume = 0;
};

struct UnitWait {
  uint64_t node_id = 0;
  Unit unit = Unit::kDte;
  uint64_t uid = 0;
  uint64_t tid = 0;
  Time start = 0;
  Time end = 0;
  WaitReason reason = WaitReason::kOwnPrevTask;
};

// 注入记一条，收齐记一条，两边由不同的模块发出：注入源记起跑并带上这个 uid 该有几个
// 分片，汇聚点每收齐一个包记一条。一个 uid 的结果可能落在几个汇聚点上，谁也不知道
// 全局收够了没有，所以数分片这件事只能等汇总时在主线程做。
struct GlobalMark {
  uint64_t uid = 0;
  Time time = 0;
  bool is_end = false;
  uint64_t expected_fragments = 1;
};

struct GlobalLatency {
  uint64_t uid = 0;
  Time start = 0;
  Time end = 0;
  uint64_t expected_fragments = 1;
};

// 每个挂时钟的模块持有一个。只被该模块的 Cycle 触碰，所以内部是裸 vector。
class SpanRecorder {
 public:
  SpanRecorder() = default;
  explicit SpanRecorder(bool on) : enabled(on) {}

  void SetEnabled(bool on) { enabled = on; }
  bool Enabled() const { return enabled; }

  // allow_zero 只给那些确实存在零长语义的占用用，默认零长视为无可归因时长而丢弃。
  void Span(uint64_t node_id, Unit unit, uint64_t uid, uint64_t tid, Time start,
            Time end, SpanState state, uint64_t volume = 0,
            bool allow_zero = false) {
    if (!enabled) return;
    if (end < start) { ++dropped; return; }
    if (end == start && !allow_zero) { ++dropped; return; }
    UnitSpan s;
    s.node_id = node_id;
    s.unit = unit;
    s.uid = uid;
    s.tid = tid;
    s.start = start;
    s.end = end;
    s.state = state;
    s.volume = volume;
    spans.push_back(s);
  }

  void Wait(uint64_t node_id, Unit unit, uint64_t uid, uint64_t tid, Time start,
            Time end, WaitReason reason) {
    if (!enabled) return;
    if (end <= start) { ++dropped; return; }
    UnitWait w;
    w.node_id = node_id;
    w.unit = unit;
    w.uid = uid;
    w.tid = tid;
    w.start = start;
    w.end = end;
    w.reason = reason;
    waits.push_back(w);
  }

  void GlobalStart(uint64_t uid, Time t, uint64_t expected_fragments) {
    if (!enabled) return;
    GlobalMark m;
    m.uid = uid;
    m.time = t;
    m.is_end = false;
    m.expected_fragments = expected_fragments;
    marks.push_back(m);
  }

  void GlobalEnd(uint64_t uid, Time t) {
    if (!enabled) return;
    GlobalMark m;
    m.uid = uid;
    m.time = t;
    m.is_end = true;
    marks.push_back(m);
  }

  std::vector<UnitSpan> const& Spans() const { return spans; }
  std::vector<UnitWait> const& Waits() const { return waits; }
  std::vector<GlobalMark> const& Marks() const { return marks; }

  // 被两条过滤规则丢掉的条数。它不是错误，是用来核对"没记到"与"没发生"的区别。
  uint64_t Dropped() const { return dropped; }

  void Clear() {
    spans.clear();
    waits.clear();
    marks.clear();
    dropped = 0;
  }

 private:
  bool enabled = true;
  std::vector<UnitSpan> spans;
  std::vector<UnitWait> waits;
  std::vector<GlobalMark> marks;
  uint64_t dropped = 0;
};

// 一次 run 的可比对产物。它就是验收判据里"完成集合一致"那一层要比的东西。
struct RunResult {
  Time end_time = 0;
  uint64_t expected_users = 0;
  std::vector<uint64_t> completed_uids;  // 按完成先后
  std::vector<uint64_t> lost_uids;       // 有起跑记录但没有完成记录

  bool Succeeded() const {
    return lost_uids.empty() && completed_uids.size() == expected_users;
  }
};

// 主线程侧的汇总。仿真期不碰它，JoinAll 之后再把各模块的记录收上来。
class RunRecorder {
 public:
  void Collect(SpanRecorder const& r) {
    spans.insert(spans.end(), r.Spans().begin(), r.Spans().end());
    waits.insert(waits.end(), r.Waits().begin(), r.Waits().end());
    marks.insert(marks.end(), r.Marks().begin(), r.Marks().end());
    dropped += r.Dropped();
  }

  // 配对起跑与完成，然后按全序排一遍。排序不是为了好看：观测产物要能逐行 diff，
  // 收集顺序取决于线程调度，不排就不可比。
  void Finalize(uint64_t expected_users) {
    std::sort(spans.begin(), spans.end(), [](UnitSpan const& a, UnitSpan const& b) {
      if (a.node_id != b.node_id) return a.node_id < b.node_id;
      if (a.start != b.start) return a.start < b.start;
      if (a.uid != b.uid) return a.uid < b.uid;
      if (a.tid != b.tid) return a.tid < b.tid;
      if (a.unit != b.unit) return a.unit < b.unit;
      if (a.state != b.state) return a.state < b.state;
      return a.end < b.end;
    });
    std::sort(waits.begin(), waits.end(), [](UnitWait const& a, UnitWait const& b) {
      if (a.node_id != b.node_id) return a.node_id < b.node_id;
      if (a.start != b.start) return a.start < b.start;
      if (a.uid != b.uid) return a.uid < b.uid;
      if (a.tid != b.tid) return a.tid < b.tid;
      if (a.unit != b.unit) return a.unit < b.unit;
      return a.reason < b.reason;
    });

    std::sort(marks.begin(), marks.end(), [](GlobalMark const& a, GlobalMark const& b) {
      if (a.uid != b.uid) return a.uid < b.uid;
      if (a.time != b.time) return a.time < b.time;
      return a.is_end < b.is_end;
    });

    latency.clear();
    result = RunResult();
    result.expected_users = expected_users;

    std::vector<GlobalLatency> done;
    size_t i = 0;
    while (i < marks.size()) {
      const uint64_t uid = marks[i].uid;
      bool has_start = false;
      uint64_t need = 1;
      uint64_t got = 0;
      GlobalLatency g;
      g.uid = uid;
      while (i < marks.size() && marks[i].uid == uid) {
        if (marks[i].is_end) {
          ++got;
          // 收够那一刻才算完成，多出来的分片不再改结论
          if (got == need) g.end = marks[i].time;
        } else if (!has_start) {
          g.start = marks[i].time;
          need = marks[i].expected_fragments;
          g.expected_fragments = need;
          has_start = true;
        }
        ++i;
      }
      if (has_start && got >= need) done.push_back(g);
      else if (has_start) result.lost_uids.push_back(uid);
    }

    // 完成集合按完成先后排，这是它跟 Bach 对表时的顺序
    std::sort(done.begin(), done.end(), [](GlobalLatency const& a, GlobalLatency const& b) {
      if (a.end != b.end) return a.end < b.end;
      return a.uid < b.uid;
    });
    for (GlobalLatency const& g : done) {
      latency.push_back(g);
      result.completed_uids.push_back(g.uid);
      if (g.end > result.end_time) result.end_time = g.end;
    }
    std::sort(result.lost_uids.begin(), result.lost_uids.end());
  }

  std::vector<UnitSpan> const& Spans() const { return spans; }
  std::vector<UnitWait> const& Waits() const { return waits; }
  std::vector<GlobalLatency> const& Latency() const { return latency; }
  RunResult const& Result() const { return result; }
  uint64_t Dropped() const { return dropped; }

 private:
  std::vector<UnitSpan> spans;
  std::vector<UnitWait> waits;
  std::vector<GlobalMark> marks;
  std::vector<GlobalLatency> latency;
  RunResult result;
  uint64_t dropped = 0;
};

}
}

#endif
