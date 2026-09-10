#ifndef _LATCH_BACH_IP_CHIP_SCP_
#define _LATCH_BACH_IP_CHIP_SCP_

// M1 · SCP boot 序列。
//
// 自启动 → 完成 PCIe 链路训练 → 给本 chip 全部 core 的 Router 配 RouterTable、
// Skip Mask 与 Credit Bypass Route → 顺序解复位并配置各 core 的 TS、三个 RV core
// 与三个 DSA。
//
// Router 那一段排在最前，而且全 chip 一个 core 不落：漏掉任何一个 core 的 Router，
// 经过它的 path 就全断。不派角色的 core 只配 Router 那两样 —— TS、RV core 与 DSA
// 本来就没构造。
//
// 每个 core 的初始化五步按序做完：RV core firmware 写进 ITCM → 配置 Bach core
// 解复位 → TS 初始化（任务链）→ Router 初始化（路由表）→ kernel 初始化。装载
// 拍数按镜像字节数除以 4 B 计，与业务段用同一把尺。
//
// 三个 RV core 的 ready 全高之后才开放业务接收权限，Router 才开始接收业务。

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "bach/ip/chip/chip_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// boot 的六个阶段。scp_fsm 记的就是走到哪一步。
enum class ScpState : uint32_t {
  kSelfBoot = 0,      // 自启动
  kPcieTrain = 1,     // PCIe 链路训练
  kRouterCfg = 2,     // 全 chip 每个 core 的 Router
  kCoreCfg = 3,       // 逐个 core 的五步初始化
  kWaitReady = 4,     // 等三个 RV core 都进 wait
  kBusiness = 5,      // 开放业务接收
};

// 一笔配置事务。编译侧算好一串，boot 期逐笔发出去。
struct ScpTxn {
  uint64_t core = 0;
  uint64_t addr = 0;
  bool we = true;
  uint64_t data = 0;
  bool bcast = false;
};

class ScpStub : public BachModule {
 public:
  ScpStub(ClockPtr clock, const std::string& name, uint64_t core_num,
          uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        cores(core_num),
        ctrl(std::make_shared<ScpCtrlPort>(clock)),
        state_sig(clock),
        issued(clock) {}

  ScpCtrlPort& Ctrl() { return *ctrl; }
  std::shared_ptr<ScpCtrlPort> CtrlPtr() const { return ctrl; }

  // 编译侧读入的那一串。分两段排：Router 那一段排在 TS 与 RV core 之前。
  void PushRouterTxn(ScpTxn const& t) { router_img.push_back(t); }
  void PushCoreTxn(ScpTxn const& t) { core_img.push_back(t); }
  // 各 core 报上来的 ready，装配层每拍喂进来。
  void SetCoreReady(uint64_t i, bool ok) {
    if (i < ready.size()) ready[i] = ok;
    if (ready.size() < cores) ready.resize(cores, false);
  }

  ScpState State() const { return state; }
  uint64_t Issued() const { return issue_cnt; }
  // 三个 RV core 的 ready 全高之后才开放业务接收权限。
  bool BusinessOpen() const { return state == ScpState::kBusiness; }
  bool Quiescent() const override {
    return router_img.empty() && core_img.empty();
  }

 protected:
  void Step() override {
    ready.resize(cores, false);
    Advance();
    state_sig = uint64_t(state);
    issued = issue_cnt;
    TracePerCycle("state", uint64_t(state));
  }

 private:
  void Advance() {
    switch (state) {
      case ScpState::kSelfBoot:
        state = ScpState::kPcieTrain;
        ctrl->Idle();
        return;
      case ScpState::kPcieTrain:
        // 链路训练：本轮不建模具体握手，占一拍。
        state = ScpState::kRouterCfg;
        ctrl->Idle();
        return;
      case ScpState::kRouterCfg:
        if (Issue(router_img)) return;
        state = ScpState::kCoreCfg;
        return;
      case ScpState::kCoreCfg:
        if (Issue(core_img)) return;
        state = ScpState::kWaitReady;
        return;
      case ScpState::kWaitReady: {
        ctrl->Idle();
        for (uint64_t i = 0; i < cores; ++i) {
          if (!ready[i]) return;
        }
        state = ScpState::kBusiness;
        return;
      }
      default:
        ctrl->Idle();
        return;
    }
  }

  // 每笔事务一拍。发完这一段返回 false。
  bool Issue(std::deque<ScpTxn>& img) {
    if (img.empty()) {
      ctrl->Idle();
      return false;
    }
    ScpTxn t = img.front();
    img.pop_front();
    ctrl->Drive(t.core, t.addr, t.we, t.data, t.bcast, ++seq);
    ++issue_cnt;
    return true;
  }

  uint64_t cores;
  std::shared_ptr<ScpCtrlPort> ctrl;
  std::deque<ScpTxn> router_img, core_img;
  std::vector<bool> ready;
  ScpState state = ScpState::kSelfBoot;
  uint64_t seq = 0, issue_cnt = 0;

  Logic64 state_sig, issued;
};

}  // namespace bach
}  // namespace latch

#endif
