#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_VU_PORTS_
#define _LATCH_BACH_IP_CHIP_CORE_VU_VU_PORTS_

// VU 内部的端口束。
//
// 宏指令与微指令都用 LogicPtr 整包搬，不把字段摊平：一条宏指令带 12 个动态
// 参数与一整组静态配置，摊平成几十根 Logic64 既不好读，也不像硬件。硬件上
// 这些位是随微指令一起走级间 latch 的。
//
// 每个端口每拍必须 Drive 或 Idle 二选一：Logic64 一拍只能写一次，一拍不写会
// 回落成上一拍的值，下游会把同一笔认成两笔。

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "base/logic.h"
#include "bach/ip/chip/core/vu/vu_types.h"

namespace latch {
namespace bach {

// config_register → ISQ：写 trigger 锁存出来的一条宏指令。
//
// 带序号是因为一次 valid/ready 最少两拍：请求方在等 ready 那一拍不改端口，
// 端口上的 valid 会回落成上一拍的 1，接收方就会把同一条认两遍。
class VuInstPort : public Logic {
 public:
  Logic64 valid, ready, seq;
  LogicPtr<VuMacroInst> inst;

  explicit VuInstPort(ClockPtr c) : valid(c), ready(c), seq(c), inst(c) {
    Fields(valid, ready, seq, inst);
  }

  void Drive(std::shared_ptr<VuMacroInst> pkt, uint64_t n) {
    valid = 1;
    inst = std::move(pkt);
    seq = n;
  }
  void Idle() {
    valid = 0;
    inst = std::shared_ptr<VuMacroInst>();
    seq = seq.Get();
  }
  void DriveReady(bool ok) { ready = ok ? 1 : 0; }

  bool Valid() const { return valid.Get() != 0; }
  bool Ready() const { return ready.Get() != 0; }
  uint64_t Seq() const { return seq.Get(); }
  std::shared_ptr<VuMacroInst> Inst() const { return inst.Get(); }
};

// pipe_ctrl → 执行通路：一条宏指令展开出来的一组微指令。
class VuUopsPort : public Logic {
 public:
  Logic64 valid, ready, seq;
  LogicPtr<VuUops> uops;

  explicit VuUopsPort(ClockPtr c) : valid(c), ready(c), seq(c), uops(c) {
    Fields(valid, ready, seq, uops);
  }

  void Drive(std::shared_ptr<VuUops> pkt, uint64_t n) {
    valid = 1;
    uops = std::move(pkt);
    seq = n;
  }
  void Idle() {
    valid = 0;
    uops = std::shared_ptr<VuUops>();
    seq = seq.Get();
  }
  void DriveReady(bool ok) { ready = ok ? 1 : 0; }

  bool Valid() const { return valid.Get() != 0; }
  bool Ready() const { return ready.Get() != 0; }
  uint64_t Seq() const { return seq.Get(); }
  std::shared_ptr<VuUops> Uops() const { return uops.Get(); }
};

// 一条宏指令在通路上的一段现场。
//
// 各执行单元的输出都挂在这一份上：SMUX 按静态配置从这里取 bypass 源，DMUX 从
// 这里取写回值。硬件上这是级间 latch 上的一组线，不是一块可寻址的存储。
//
// 一条宏指令在向量通路上是逐段流过的：一段是一个 RF entry，FP32 32 个元素、
// BF16 64 个。LU 每凑齐一段就往下交一份，下游各级各算各的那一段，SU 收到一段
// 就写一段，这样 Load、计算与 Store 在同一条宏指令内重叠。跨元素的那几种运算
// （Top-K、mask 的归约与按第一个 1 生成、标量迭代）看的是整条，它们所在的宏
// 指令不分段，整条走一份。
// DMUX → SU 的一级 skid buffer。DMUX 把 su_in 放进来就归还上游 VEXE
// 分组的 credit；SU 从这里取。只有一格：满了 DMUX 停收，SU 的反压沿
// TailRoute 传回 VEXE，不会因为提前还 credit 而丢掉反压。
class VuSuSkid {
 public:
  bool HasRoom() const { return !slot_.has_value(); }
  bool Empty() const { return !slot_.has_value(); }
  void Push(VuOperand const& v) { slot_ = v; }
  VuOperand Pop() {
    if (!slot_.has_value()) return VuOperand{};
    VuOperand v = std::move(*slot_);
    slot_.reset();
    return v;
  }

 private:
  std::optional<VuOperand> slot_;
};

struct VuFlow {
  VuUops uops;
  // 本段是第几段、本段多少个元素、本段第一个元素在整条里的下标、是不是最后
  // 一段。不分段时只有一段，长度就是 VL。
  uint64_t seg = 0;
  uint64_t seg_len = 0;
  uint64_t seg_base = 0;
  bool seg_last = true;
  // 各执行单元的输出。SEXE 三次迭代各占一格：DMUX 的六个 SRF 写口是逐次
  // 迭代各写一个的，挤在一格里会把前一次的结果覆盖掉。VSFU 两格对应两个功能
  // 相同的单元，BF16 下拼接成一个逻辑单元、只有第 0 格有结果。
  VuOperand lu, su_in;
  std::array<VuOperand, 3> valu;
  std::array<VuOperand, 2> vsfu;
  VuOperand mexe;
  std::array<VuOperand, 3> sexe;
  // SMUX 这一级从 RF 读出来的：VRF 两个读口、MRF 两个读口、SRF 八个读口。
  // 端口号就是 src_sel 编码的低位，各单元按自己的 SRC*_SEL 直接取。
  std::array<VuOperand, 2> vrf_rd;
  std::array<VuOperand, 2> mrf_rd;
  std::array<VuOperand, kVuSrfRdPorts> srf_rd;
  // VEXE 分组的结果还要被 VEXE 之外的器件取几次：DMUX 的 RF 写口、SU、
  // MEXE / SEXE。VEXENet 在段里统计它，消费者取走时减一；减到 0 才释放
  // 该分组的流水槽。下标是 VEXE 分组号（VALU0/1/2、VSFU0/1）。
  std::array<uint8_t, 5> ext_need{};

  // 某个源被 VEXE 之外的消费者取走一次。非 VEXE 源的 sel 直接忽略。
  void Consume(uint64_t sel) {
    int g = -1;
    switch (sel) {
      case kSrcValu0: g = 0; break;
      case kSrcValu1: g = 1; break;
      case kSrcValu2: g = 2; break;
      case kSrcVsfu0: g = 3; break;
      case kSrcVsfu1: g = 4; break;
      default: return;
    }
    if (g >= 0 && ext_need[g] > 0) --ext_need[g];
  }

  bool ExtPending() const {
    for (uint8_t n : ext_need) {
      if (n != 0) return true;
    }
    return false;
  }
  // 这一分组的数据目的地（RF / SU）已经改走按分组结果通道；段级写回不再重复。
  std::array<bool, 5> result_handled{};
  bool su_result_done = false;
  // 通路上攒下的异常位，退休那一拍记进 error_code；err_unit 是首个上报异常的
  // 单元，进 error_info.ERR_UNIT。
  uint64_t error = 0;
  uint64_t err_unit = kVuErrUnitNone;
  // 替换模式下本段被换掉的 NaN / Inf element 数，退休时累进 Profile 计数器。
  uint64_t nan_replaced = 0;
  uint64_t inf_replaced = 0;
  // VEXE 这一截的结果已经按取源次序一次算好：后面的 VEXE 单元只计拍数。
  bool vexe_done = false;

  // 本段多少个元素。各级按它算，不再直接取 VL：分段之后 VL 是整条的长度。
  uint64_t SegLen() const { return seg_len != 0 ? seg_len : uops.inst.Vl(); }
};

using VuFlowPtr = std::shared_ptr<VuFlow>;

// 按分组结果通道的目的地编号：VRF p0/p1、MRF、SRF p0–p5、SU。
enum : uint8_t {
  kVuDestVrf0 = 0,
  kVuDestVrf1 = 1,
  kVuDestMrf = 2,
  kVuDestSrf0 = 3,
  kVuDestSu = 9,
  kVuDestCount = 10,
};

// 一笔按分组结果。只带一个目的地；同一分组要写多个端口就生成多项，
// 各端口队列独立推进。
struct VuResult {
  VuFlowPtr f;
  int group = -1;
  uint8_t dest = 0;
  uint64_t port = 0;
  VuOperand data;
};

// DMUX 的按目的地队列 + 宏级未完成计数。每个目的地一队，队列之间互不
// 队首阻塞；pending 按宏统计，宏末退休流等它归零。
class VuResultBus {
 public:
  static constexpr size_t kDepth = 16;
  bool HasRoom(uint8_t dest) const { return q_[dest].size() < kDepth; }
  bool Empty(uint8_t dest) const { return q_[dest].empty(); }
  void Push(VuResult r) {
    ++pending_[r.f->uops.inst.seq];
    q_[r.dest].push_back(std::move(r));
  }
  VuResult Pop(uint8_t dest) {
    VuResult r = std::move(q_[dest].front());
    q_[dest].pop_front();
    return r;
  }
  void Done(uint64_t seq) {
    auto it = pending_.find(seq);
    if (it != pending_.end() && it->second > 0) --it->second;
  }
  uint64_t Pending(uint64_t seq) const {
    auto it = pending_.find(seq);
    return it == pending_.end() ? 0 : it->second;
  }

 private:
  std::array<std::deque<VuResult>, kVuDestCount> q_;
  std::unordered_map<uint64_t, uint64_t> pending_;
};

// SU 数据通道：DMUX 只把它当另一个输出端口，SU 独立于退休流处理。
class VuSuDataQ {
 public:
  static constexpr size_t kDepth = 16;
  bool HasRoom() const { return q_.size() < kDepth; }
  bool Empty() const { return q_.empty(); }
  void Push(VuResult r) { q_.push_back(std::move(r)); }
  VuResult Pop() {
    VuResult r = std::move(q_.front());
    q_.pop_front();
    return r;
  }

 private:
  std::deque<VuResult> q_;
};

// 执行通路各级之间：搬一份在飞的宏指令现场。
class VuFlowPort : public Logic {
 public:
  Logic64 valid, ready, seq;
  LogicPtr<VuFlow> flow;

  explicit VuFlowPort(ClockPtr c) : valid(c), ready(c), seq(c), flow(c) {
    Fields(valid, ready, seq, flow);
  }

  void Drive(VuFlowPtr pkt, uint64_t n) {
    valid = 1;
    flow = std::move(pkt);
    seq = n;
  }
  void Idle() {
    valid = 0;
    flow = VuFlowPtr();
    seq = seq.Get();
  }
  void DriveReady(bool ok) { ready = ok ? 1 : 0; }

  bool Valid() const { return valid.Get() != 0; }
  bool Ready() const { return ready.Get() != 0; }
  uint64_t Seq() const { return seq.Get(); }
  VuFlowPtr Flow() const { return flow.Get(); }
};

}  // namespace bach
}  // namespace latch

#endif
