#ifndef _LATCH_BACH_IP_CHIP_CORE_ROUTER_ROUTER_TABLE_
#define _LATCH_BACH_IP_CHIP_CORE_ROUTER_ROUTER_TABLE_

// RouterTable 与 CSR。
//
// 64 条表项按 path_id 索引，只描述静态路由与资源需求，不保存包的动态执行状态。
// 同一个 path_id 在不同 core 上表项不同，所以每 core 一份，不能从拓扑反推。
//
// 内部多副本：所有需要并行查询的位置各持一份，由这里统一接收写事务。更新状态机
// 把同一笔写依次写进全部副本，全部写完才向软件报完成，中途不暴露部分新部分旧的
// 状态。DTE 与 ReduceModule 那两份是外部副本，软件自己写，硬件不同步。
//
// 另有一组与 RouterTable 分开配的 Skip Mask：per-core 一位，标记该 core 是否被
// 跳过。复位释放后全部条目是 bypass / no-op，配置写入前不投递任何包。

#include <array>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/common/flit.h"
#include "bach/common/message.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 表项数，按 path_id 索引。
constexpr uint64_t kPathNum = 64;

// VC 数。VC3 专给逐级 reduce，VC0/1/2 由软件配给其余操作类型。
constexpr uint64_t kReduceVc = 3;

// 输入侧的方向：三个 R2R 加 core。vc_buf、vc_credit、stream_tab 都按它开。
enum Dir : uint64_t {
  kDirMid = 0,
  kDirLeft = 1,
  kDirRight = 2,
  kDirCore = 3,
  kDirNum = 4,
};

// ReduceModule 回注那一路在 Xbar 上是第五个输入。它也走一个 RouterStation，
// 好让结果「重新参与仲裁」这件事与其他方向走同一套代码。
constexpr uint64_t kDirReduceInject = 4;

// R2R 三个方向，credit 与 stream 表只对它们记账。
constexpr uint64_t kR2RNum = 3;

// 一个方向的 VC Buffer 分两级：每个 VC 一块只归自己的 private，另有一块四个 VC
// 先到先得的 shared pool。private 那一档取够覆盖 credit 往返延迟，防死锁下限是
// 2；shared pool 是冗余，同样按 credit 往返延迟取。一个方向合计 4 × 20 + 20 =
// 100 flit，正好等于下游该方向的 VC Buffer 容量，任何时刻都不会超发，上游因此
// 不需要第二道反压信号，链路上也没有 ready。
//
// 深度由软件在跑之前经 ctrl_noc 写 RTR_VC_DEPTH[d][v] 配，下面是复位默认值。
// 硬件的寄存器每个方向每个 VC 一项，模型简化成各方向共用一套。
using VcDepth = std::array<uint64_t, kVcNum>;
constexpr uint64_t kVcPrivateDepth = 20;
constexpr uint64_t kVcSharedDepth = 20;
constexpr VcDepth kVcPrivateDepthDefault = {kVcPrivateDepth, kVcPrivateDepth,
                                            kVcPrivateDepth, kVcPrivateDepth};

// flow_dir 的 5 位：bit0 上下、bit1 左、bit2 右、bit3 reduce1、bit4 reduce2。
// 进本 core 不占这里的位，末端核这一项全不置位。
enum FlowBit : uint64_t {
  kFlowMid = 1u << 0,
  kFlowLeft = 1u << 1,
  kFlowRight = 1u << 2,
  kFlowReduce1 = 1u << 3,
  kFlowReduce2 = 1u << 4,
};

// Xbar 的 7 个出口。
enum XbarOut : uint64_t {
  kOutMid = 0,
  kOutLeft = 1,
  kOutRight = 2,
  kOutCore = 3,
  kOutReduce0 = 4,
  kOutReduce1 = 5,
  kOutReduce2 = 6,
  kXbarOutNum = 7,
};

// Xbar 的 5 个入口。
enum XbarIn : uint64_t {
  kInMid = 0,
  kInLeft = 1,
  kInRight = 2,
  kInLocal = 3,
  kInReduce = 4,
  kXbarInNum = 5,
};

// 一条 path 在本 core 上的表项。字段照 F53，含义与填法照「编译侧怎么填这三张表」。
// Release 静态路由的出方向掩码：bit0 mid、bit1 left、bit2 right 与 flow_dir 同位，
// 这一位交给本级。
constexpr uint64_t kReleaseSelf = 1u << 3;

struct RouteEntry {
  bool valid = false;

  OpType op_type = OpType::kTransfer;
  uint64_t flow_dir = 0;              // 5 bit 出方向掩码
  uint64_t cur_vc = 0;                // 给上一级无法指定 VC 的入口用
  std::array<uint64_t, 5> nxt_vc{};   // 五个出方向各 2 bit

  // 进不进本 core 由前两者二选一决定。enable=0 时按 bypass 定（0 进核、1 bypass），
  // =1 时取 MSG 里 path_core_mask 的第 idx 位。位到 core 的对应不是固定编码，
  // 每个 core 在自己的表项里指定看哪一位。
  bool path_core_mask_enable = false;
  uint64_t path_core_mask_idx = 0;
  bool path_core_bypass = true;

  bool need_buffer = false;           // 允许进 core 缓存，即溢流使能
  bool stream_table_enable = false;   // 出核前查不查对应输出端的 stream credit 表

  uint64_t cur_credit_type = 0;       // 0 广播、1 P2P
  uint64_t cur_credit_require = 0;    // 上游已拨给本核的量；不进核填 0
  std::array<uint64_t, kR2RNum> nxt_credit_type{};
  std::array<uint64_t, kR2RNum> nxt_credit_require{};

  uint64_t reduce_data_type = 0;      // 输入精度
  uint64_t reduce_outdata_type = 0;   // 输出精度
  uint64_t reduce_in_mask = 0;        // 本级要等哪几个相邻方向的分量
  bool reduce_need = false;           // reduceNeedMask：结果往下游发之前查不查下游那一级这个用户空不空
  Operation operation = Operation::kForward;
  bool stall_way = false;             // false 留在 VC 等，true 转 Core Mem 重发

  // 出核的包发到片外那一段时的目的标识：0～47 是 chip、48 是出口桩。阵列内部
  // 靠 path_id 走，chip 与 chip 之间也不经 PCIe Switch，所以只有走边缘口出去
  // 的那几条 path 填它。
  uint64_t ext_dst = 0;

  // 进本 core 的判定。mask 是包头里的 path_core_mask。
  bool EntersCore(uint64_t path_core_mask) const {
    if (!path_core_mask_enable) return !path_core_bypass;
    return ((path_core_mask >> path_core_mask_idx) & 1u) != 0;
  }
};

// 复位值：全条目 bypass / no-op，配置写入前不投递任何包。
inline RouteEntry NoOpEntry() {
  RouteEntry e;
  e.valid = false;
  e.path_core_bypass = true;
  e.flow_dir = 0;
  return e;
}

class RouterTable : public BachModule {
 public:
  // 副本数按「所有需要并行查询的位置各持一份」数出来：四个 RouterStation、
  // ReduceModule（查 reduce_in_mask 与 flow_dir）、CreditMonitor（按 PathID 查
  // 需要的方向），共 6 处。每副本写入拍数是建模计划的默认值（原文未给）。
  static constexpr uint64_t kCopyNum = 6;
  static constexpr uint64_t kWriteCyclesPerCopy = 1;

  RouterTable(ClockPtr clock, const std::string& name, uint64_t parent = 0,
              bool tick = true)
      : BachModule(clock, name, parent, tick),
        commit_done(clock),
        committed(clock) {
    for (uint64_t c = 0; c < kCopyNum; ++c) {
      copies.emplace_back();
      copies.back().assign(kPathNum, NoOpEntry());
    }
  }

  // ── 数据面：并行查询的各位置各读自己那份副本 ──
  RouteEntry const& Lookup(uint64_t copy, uint64_t path_id) const {
    LOGCHECK(path_id < kPathNum, "RouterTable: path_id 越界。");
    return copies.at(copy).at(path_id);
  }

  // ── 配置面 ──
  // 一笔写事务：锁存表项，随后由更新状态机逐份写进全部副本。
  void Write(uint64_t path_id, RouteEntry const& e) {
    LOGCHECK(path_id < kPathNum, "RouterTable: path_id 越界。");
    LOGCHECK(!busy, "RouterTable: 上一笔提交没完成，不接受覆盖。");
    pend_path = path_id;
    pend_entry = e;
    pend_entry.valid = true;
    busy = true;
    cursor = 0;
  }

  bool Busy() const { return busy; }
  uint64_t CommitDone() const { return commit_done.Get(); }
  uint64_t Committed() const { return committed.Get(); }

  // Skip Mask 与 Credit Bypass Route 都是与 RouterTable 分开配的一组。
  void SetSkipMask(uint64_t mask) { skip_mask = mask; }
  uint64_t SkipMask() const { return skip_mask; }
  bool CoreSkipped(uint64_t core_in_chip) const {
    return ((skip_mask >> core_in_chip) & 1u) != 0;
  }

  // 每个业务 credit 输入端口一个静态输出方向 Mask（RTR_RELEASE_ROUTE）。Stream
  // 与 Reduce 两类 release 只按它转发，不查 RouterTable、不进 Xbar 仲裁；带
  // kReleaseSelf 的交给本级。
  void SetCreditBypass(uint64_t in_port, uint64_t out_mask) {
    LOGCHECK(in_port < kDirNum, "RouterTable: credit 输入端口越界。");
    credit_bypass[in_port] = out_mask;
  }
  uint64_t CreditBypass(uint64_t in_port) const {
    return credit_bypass.at(in_port);
  }

  // 构造期一次性铺满，跳过提交状态机。boot 期的逐笔写走 Write。
  void Preload(uint64_t path_id, RouteEntry e) {
    LOGCHECK(path_id < kPathNum, "RouterTable: path_id 越界。");
    e.valid = true;
    for (auto& c : copies) c[path_id] = e;
    ++committed_pending;
  }

  bool Quiescent() const override { return !busy; }

 protected:
  void Step() override {
    if (busy) {
      // 更新状态机：一拍写一份副本，全部写完才报完成。
      copies[cursor][pend_path] = pend_entry;
      ++cursor;
      if (cursor >= kCopyNum) {
        busy = false;
        done_pulse = true;
        ++committed_pending;
      }
    }
    commit_done = done_pulse ? 1 : 0;
    committed = committed_pending;
    done_pulse = false;
    TracePerCycle("busy", busy ? 1 : 0);
    TracePerCycle("committed", committed_pending);
  }

 private:
  std::vector<std::vector<RouteEntry>> copies;
  std::array<uint64_t, kDirNum> credit_bypass{};
  uint64_t skip_mask = 0;

  bool busy = false;
  bool done_pulse = false;
  uint64_t cursor = 0;
  uint64_t pend_path = 0;
  RouteEntry pend_entry;
  uint64_t committed_pending = 0;

  Logic64 commit_done, committed;
};

}  // namespace bach
}  // namespace latch

#endif
