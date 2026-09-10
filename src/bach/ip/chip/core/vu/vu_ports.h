#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_VU_PORTS_
#define _LATCH_BACH_IP_CHIP_CORE_VU_VU_PORTS_

// VU 内部的端口束。
//
// 宏指令与微指令都用 LogicPtr 整包搬，不把字段摊平：一条宏指令带 12 个动态
// 参数与一整组静态配置，摊平成几十根 Logic64 既不好读，也不像硬件 —— 硬件上
// 这些位是随微指令一起走级间 latch 的。
//
// 每个端口每拍必须 Drive 或 Idle 二选一：Logic64 一拍只能写一次，一拍不写会
// 回落成上一拍的值，下游会把同一笔认成两笔。

#include <memory>
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

// 一条宏指令在通路上的全部中间结果。
//
// 各执行单元的输出都挂在这一份上：SMUX 按静态配置从这里取 bypass 源，DMUX 从
// 这里取写回值。硬件上这是级间 latch 上的一组线，不是一块可寻址的存储。
struct VuFlow {
  VuUops uops;
  // 各执行单元的输出。SEXE 三次迭代各占一格 —— DMUX 的六个 SRF 写口是逐次
  // 迭代各写一个的，挤在一格里会把前一次的结果覆盖掉。
  VuOperand lu, su_in;
  std::array<VuOperand, 3> valu;
  VuOperand vsfu;
  VuOperand mexe;
  std::array<VuOperand, 3> sexe;
  // SMUX 这一级从 RF 读出来的：VRF 两个读口、MRF 两个读口、SRF 八个读口。
  // 端口号就是 src_sel 编码的低位，各单元按自己的 SRC*_SEL 直接取。
  std::array<VuOperand, 2> vrf_rd;
  std::array<VuOperand, 2> mrf_rd;
  std::array<VuOperand, kVuSrfRdPorts> srf_rd;
  uint64_t error = 0;
};

using VuFlowPtr = std::shared_ptr<VuFlow>;

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
