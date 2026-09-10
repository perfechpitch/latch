#ifndef _LATCH_BACH_IP_CHIP_CTRL_NOC_
#define _LATCH_BACH_IP_CHIP_CTRL_NOC_

// M2 · ctrl_noc 端点分发。
//
// 每个 core 一个端点。收 SCP 的配置事务，`cfg_core` 命中本 core 或带广播标记时
// 锁存，按 addr_map 查出目的模块，下一拍写到那个模块的 cfg 口。
//
// 地址空间视野检查：地址不在本 core 视野内时记地址错，不下发。三个 RV core 各自
// 看到 ITCM、DTCM、Share Mem、Core Mem 与对应 DSA 的 IO reg；SCP 看到 core 内
// 全部地址空间，所以端点这一层放行的范围比任何一个 RV core 都宽。
//
// core_id 是只读寄存器，SCP 经 ctrl_noc 读 MMIO 取得，软件不可修改 —— weights
// 落到哪个 core 全靠它。
//
// 广播开关默认关：关时 SCP 依次配每个 core，开时只发一次给 core0 由它依次广播。
// 建模里各 core 的端点都挂在同一根 scp_ctrl 上，带广播标记时每个端点各自认领，
// 效果与 core0 逐个转发相同，省掉一条只在 boot 期用一次的菊花链。

#include <memory>
#include <string>

#include "bach/ip/chip/chip_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 端点交给本 core 的一笔配置事务。装配层据此调各模块的写口。
struct CfgEvent {
  bool valid = false;
  CfgRoute route;
  bool we = false;
  uint64_t wdata = 0;
};

class CtrlNocEndpoint : public BachModule {
 public:
  CtrlNocEndpoint(ClockPtr clock, const std::string& name, uint64_t core_id,
                  uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        id(core_id),
        ctrl(std::make_shared<ScpCtrlPort>(clock)),
        taken(clock),
        addr_errs(clock) {}

  ScpCtrlPort& Ctrl() { return *ctrl; }
  void AttachCtrl(std::shared_ptr<ScpCtrlPort> p) { ctrl = std::move(p); }

  // 本拍要交给哪个模块。装配层每拍读一次，读到的是上一拍锁存下来的那一笔。
  CfgEvent const& Event() const { return out; }
  // 目的模块读出来的值，端点下一拍回给 SCP。
  void SetRdata(uint64_t v) { rdata = v; }

  uint64_t CoreId() const { return id; }
  uint64_t Taken() const { return take_cnt; }
  uint64_t AddrErrors() const { return err_cnt; }
  bool Quiescent() const override { return true; }

 protected:
  void Step() override {
    // 上一拍锁存的那一笔在本拍交给目的模块，同时把它的读返回给 SCP。
    ctrl->DriveRdata(rdata);
    out = staged;
    staged = CfgEvent();

    if (!ctrl->Valid() || ctrl->Seq() == last_seq) {
      taken = take_cnt;
      addr_errs = err_cnt;
      return;
    }
    last_seq = ctrl->Seq();
    // 命中本 core 或带广播标记才收。
    if (ctrl->Core() != id && !ctrl->Bcast()) {
      taken = take_cnt;
      addr_errs = err_cnt;
      return;
    }
    CfgRoute r = LookupCfg(ctrl->Addr());
    if (r.target == kCfgNone) {
      // 地址不在本 core 视野内：记地址错，不下发。
      ++err_cnt;
      taken = take_cnt;
      addr_errs = err_cnt;
      return;
    }
    staged.valid = true;
    staged.route = r;
    staged.we = ctrl->We();
    staged.wdata = ctrl->Wdata();
    ++take_cnt;

    taken = take_cnt;
    addr_errs = err_cnt;
    TracePerCycle("taken", take_cnt);
  }

 private:
  uint64_t id;
  std::shared_ptr<ScpCtrlPort> ctrl;
  CfgEvent staged, out;
  uint64_t rdata = 0, last_seq = 0, take_cnt = 0, err_cnt = 0;

  Logic64 taken, addr_errs;
};

}  // namespace bach
}  // namespace latch

#endif
