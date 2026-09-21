#ifndef _LATCH_BACH_IP_CHIP_CORE_MU_MU_
#define _LATCH_BACH_IP_CHIP_CORE_MU_MU_

// MU DSA 这一组七个模块的装配。
//
// 一笔任务走的路：RV core 写配置到 regfile，最后写 trigger 锁成一笔进 issue_q；
// agu 按「先循环 tile_K 再循环 tile_N」算三组地址；两条 ldq 分别从 Core Mem 读
// token、从 Matrix Mem 读 weight；matrix exe 按 scale block 分组累加；stq 把结果
// 拼成整拍写回 Core Mem，与 issue_q 的 finish 合成 dsa_done。
//
// task 间三段重叠：task0 load → {task0 计算 ‖ task1 load} → {task0 写回 ‖
// task1 计算}。所以 issue_q 里同时有几笔停在不同阶段，各阶段各自一笔。
//
// 七个模块由外层统一驱动：外层每拍调一次 RunStep()，它按末级先做的次序逐个
// 走一遍，整个 MU 只占外层那一个协程。

#include <memory>
#include <string>

#include "bach/ip/chip/core/mu/agu.h"
#include "bach/ip/chip/core/mu/gen_ep_info.h"
#include "bach/ip/chip/core/mu/issue_q.h"
#include "bach/ip/chip/core/mu/ldq.h"
#include "bach/ip/chip/core/mu/matrix_exe.h"
#include "bach/ip/chip/core/mu/mu_ports.h"
#include "bach/ip/chip/core/mu/regfile.h"
#include "bach/ip/chip/core/mu/stq.h"
#include "bach/ip/chip/core/ts/ts_ports.h"

namespace latch {
namespace bach {

struct MuCfg {
  uint64_t cmem_size = 1024 * 1024 + 32 * 1024;
  uint64_t mmem_size = 36ull * 1024 * 1024;
};

// Drain & Trap 的四步。acu 查出越界或不对齐时进第一步，逐步走完再回 kNone。
//
// 越界那一笔的数据要丢掉，已经进了脉动通路的合法数据照常算完写回，一并丢掉
// 的话执行通路上还挂着半笔，状态机再也回不到默认状态。
enum class MuDrain : uint32_t {
  kNone = 0,
  kBlock = 1,    // 阻塞任务下发
  kClean = 2,    // 已发出的访存请求的回复照常收，不再发起新的
  kFlush = 3,    // 排空计算流水线
  kRestore = 4,  // 恢复默认状态
};

class Mu {
 public:
  Mu(ClockPtr clock, const std::string& name, MuCfg const& setting,
     uint64_t parent = 0)
      : clk(clock), cfg(setting) {
    const uint64_t gid = TraceGroup(name, parent);
    // 都不自己挂时钟：下面那一小段控制直接调各模块的方法往它们的队列里放
    // 东西，那些方法改的是普通容器。各模块要是各占一个协程，就成了几个线程改
    // 同一个容器。外层每拍调一次 RunStep()，它按末级先做的次序走一遍。
    reg = std::make_unique<MuRegfile>(clock, "regfile", gid, false);
    iq = std::make_unique<MuIssueQ>(clock, "issue_q", gid, false);
    ep = std::make_unique<GenEpInfo>(clock, "gen_ep", gid, false);
    token_ldq = std::make_unique<MuLdq>(clock, "token_ldq",
                                        kTokenLdqDepth, gid, false);
    weight_ldq = std::make_unique<MuLdq>(clock, "weight_ldq",
                                         kWeightLdqDepth, gid, false);
    exe = std::make_unique<MatrixExe>(clock, "exe", gid, false);
    stq = std::make_unique<MuStq>(clock, "stq", gid, false);
    done = std::make_shared<DonePort>(clock);
    ctrl = std::make_unique<Ctrl>(clock, "ctrl", gid, *this, false);
  }

  // ── 对外 ──
  DsaCfgPort& Cfg() { return reg->CfgPort(); }
  std::shared_ptr<DsaCfgPort> CfgPtr() const { return reg->CfgPortPtr(); }
  void AttachCfg(std::shared_ptr<DsaCfgPort> p) { reg->AttachCfg(std::move(p)); }
  // 身份信号：写 TASK_TRIGGER 那一拍采样进任务快照。
  void AttachIds(std::shared_ptr<DsaIdsPort> p) { reg->AttachIds(std::move(p)); }
  DonePort& Done() { return *done; }
  void AttachDone(std::shared_ptr<DonePort> p) { done = std::move(p); }
  MemPort& TokenPort() { return token_ldq->Port(); }
  MemPort& WeightPort() { return weight_ldq->Port(); }
  MemPort& OutPort() { return stq->Port(); }
  void AttachCmemRd(std::shared_ptr<MemPort> p) {
    token_ldq->AttachPort(std::move(p));
  }
  void AttachMmemRd(std::shared_ptr<MemPort> p) {
    weight_ldq->AttachPort(std::move(p));
  }
  void AttachCmemWr(std::shared_ptr<MemPort> p) { stq->AttachPort(std::move(p)); }
  // DTE 搬运时经这条数据线把 topK 直接写进 topK_ep_table。
  void AttachMuTopk(std::shared_ptr<MuTopkPort> p) { topk_port = std::move(p); }

  // ── 配置面 ──
  GenEpInfo& EpInfo() { return *ep; }
  std::shared_ptr<DsaRdataPort> RdataPtr() const { return reg->RdataPtr(); }

  // topK 里第 i 个专家在本 EP Group 内的序号。没配 topK 的那一档按 topK 里的
  // 先后当序号用。
  uint64_t LocalEpOf(MuInflight const& f, uint64_t i) const {
    std::vector<TopkEntry> const& t = TopkOf(f);
    if (i >= t.size()) return i;
    uint64_t local = 0;
    if (!ep->ToLocal(t[i].expert_id, &local)) return i;
    return local;
  }
  // topK 里第 i 个专家的权重。合并成一份时乘它。
  float WeightOf(MuInflight const& f, uint64_t i) const {
    std::vector<TopkEntry> const& t = TopkOf(f);
    if (i >= t.size()) return 1.0f;
    return t[i].weight;
  }
  // 这一笔用哪一份 topK：按 stream_id 读 topK_ep_table。DTE 搬运时写进来，
  // MU 计算时与 token / weight 同时读，不再有前置串行读。
  std::vector<TopkEntry> const& TopkOf(MuInflight const& f) const {
    return ep->Topk(f.cfg.stream_id, f.cfg.Experts());
  }

  // ── 观测 ──
  MuRegfile& Regfile() { return *reg; }
  MuIssueQ& IssueQ() { return *iq; }
  MatrixExe& Exe() { return *exe; }
  MuStq& Stq() { return *stq; }
  MuDrain DrainState() const { return ctrl->State(); }
  uint64_t Drains() const { return ctrl->Drains(); }
  // 上一笔宣告完成时的笔数与身份，供 Core 层发波形。完成脉冲本身只带 stream 与
  // task（Drive 的第三参默认 0），user 取自写 trigger 那一拍从 CSR 直连采样的
  // 那一份。
  uint64_t DoneCnt() const { return ctrl->DoneCnt(); }
  uint64_t DoneTask() const { return ctrl->DoneTask(); }
  uint64_t DoneUser() const { return ctrl->DoneUser(); }

  void RunStep() {
    // DTE 搬运时写进来的 topK 这一拍先收进 topK_ep_table，下面才算得到。
    StepTopk();
    // 软件轮询 SYS_STATUS 等一笔任务做完，忙不忙由这里每拍写进去。空的判据与
    // dsa_done 一样要算上写回落地：离开 stq 的写请求还要经端口到存储。
    if (Quiescent()) {
      ++idle_cycles;
    } else {
      idle_cycles = 0;
    }
    reg->SetBusy(idle_cycles < 2);
    reg->RunStep();
    ep->RunStep();
    stq->RunStep();
    exe->RunStep();
    token_ldq->RunStep();
    weight_ldq->RunStep();
    iq->RunStep();
    ctrl->RunStep();
  }

  bool Quiescent() const {
    return reg->Quiescent() && iq->Quiescent() && exe->Quiescent() &&
           stq->Quiescent() && token_ldq->Quiescent() &&
           weight_ldq->Quiescent();
  }

 private:
  // DTE 那条 topK 数据线：每拍读一次，valid 且序号没见过的写进 topK_ep_table。
  // 同一拍数据会连着两拍出现在端口上，按序号认它。
  void StepTopk() {
    if (!topk_port || !topk_port->Valid()) return;
    if (topk_port->Seq() == last_topk_seq) return;
    last_topk_seq = topk_port->Seq();
    auto d = topk_port->Data();
    if (d) ep->WriteTopk(topk_port->StreamId(), *d);
  }
  // 把七个模块串起来的那一小段控制。放在装配里，因为它读的是各模块的接口，
  // 不属于任何一个模块自己。
  class Ctrl : public BachModule {
   public:
    Ctrl(ClockPtr c, const std::string& name, uint64_t parent, Mu& owner,
         bool tick)
        : BachModule(c, name, parent, tick), mu(owner) {}

    // 与别的模块一样由 Mu::RunStep() 驱动，不自己挂时钟。

    MuDrain State() const { return drain; }
    uint64_t Drains() const { return drain_cnt; }
    uint64_t DoneCnt() const { return done_cnt; }
    uint64_t DoneTask() const { return done_task; }
    uint64_t DoneUser() const { return done_user; }

   protected:
    void Step() override {
      // Drain 的推进排在最前：这一拍先把上一拍进的那一步走掉，外面才看得到
      // 每一步各占一拍。
      StepDrain();
      Retire();
      Store();
      Compute();
      Load();
      Accept();
    }

   private:
    // 四步走完回默认状态。每一步的条件就是那一步要清干净的东西。
    void StepDrain() {
      switch (drain) {
        case MuDrain::kBlock:
          // 任务下发这一拍已经停了，进下一步。
          drain = MuDrain::kClean;
          break;
        case MuDrain::kClean:
          // 已发出的访存请求的回复照常处理，收完了再往下。
          if (mu.token_ldq->Quiescent() && mu.weight_ldq->Quiescent()) {
            drain = MuDrain::kFlush;
          }
          break;
        case MuDrain::kFlush:
          // 已进入脉动通路的合法数据算完写回。
          if (mu.exe->Quiescent() && mu.stq->Quiescent()) {
            drain = MuDrain::kRestore;
          }
          break;
        case MuDrain::kRestore:
          drain = MuDrain::kNone;
          ++drain_cnt;
          break;
        default:
          break;
      }
    }

    void Accept() {
      // 第一步就是阻塞任务下发。
      if (drain != MuDrain::kNone) return;
      if (!mu.reg->HasPending() || !mu.iq->HasRoom()) return;
      mu.iq->Push(mu.reg->TakePending());
    }

    // 一笔任务按 tile 走 kblock × nblock 遍。三段各找各自那一笔，所以写回第 i
    // 个 tile 的同时能在算第 i+1 个、读第 i+2 个，相邻两笔任务之间同理。
    void Load() {
      // Drain 期间不再发起新的访存请求。
      if (drain != MuDrain::kNone) return;
      MuInflight* f = mu.iq->FirstToLoad();
      if (f == nullptr) return;
      if (!mu.token_ldq->HasRoom() || !mu.weight_ldq->HasRoom()) return;
      MuStep s = f->agu.Next();
      // acu 查越界与对齐。查出来走 Drain & Trap，本轮只留状态位：这一笔的余下
      // tile 全部作废，直接算做完。
      if (!f->agu.CheckStep(s, mu.cfg.cmem_size, mu.cfg.mmem_size)) {
        // 越界或不对齐：这一笔余下的 tile 全部作废，进 Drain 的第一步。已经
        // 发出去的读请求的回复照常收，但不进计算，那是越界任务的数据。
        f->dropped = true;
        f->issued = f->computed = f->total;
        f->stored = f->OutTotal();
        f->stage = MuStage::kFinished;
        drain = MuDrain::kBlock;
        return;
      }
      // token 与权重的请求都带上 scale：block scale 与数据一一映射，存储把它
      // 从 scale 旁带里取出来接在正文后面，不占独立的读通道。权重只有 MXFP8
      // 那一档带。
      bool has_scale = f->agu.ScaleBytes() != 0;
      mu.token_ldq->Push(
          {f->agu.TokenAddr(s), f->agu.TokenBytes(), f->seq, has_scale});
      mu.weight_ldq->Push({f->agu.WeightAddr(s, mu.LocalEpOf(*f, s.e_idx)),
                           f->agu.WeightBytes(), f->seq,
                           f->agu.WeightScaleBytes() != 0});
      f->out_addr.push_back(f->agu.OutAddr(s));
      ++f->issued;
      f->stage = MuStage::kLoading;
    }

    void Compute() {
      // Drain 里收干净已发出的那些回复：属于越界任务的丢掉，别的照常算。
      if (drain == MuDrain::kClean && mu.token_ldq->HasData() &&
          mu.weight_ldq->HasData()) {
        mu.token_ldq->TakeData();
        mu.weight_ldq->TakeData();
        return;
      }
      MuInflight* f = mu.iq->FirstToCompute();
      if (f == nullptr) return;
      if (!mu.token_ldq->HasData() || !mu.weight_ldq->HasData()) return;
      MuLdq::Rsp t = mu.token_ldq->TakeData();
      MuLdq::Rsp w = mu.weight_ldq->TakeData();
      if (f->dropped) {
        ++f->computed;
        return;
      }
      // scale 段跟在 token 响应正文之后。
      uint64_t body = f->agu.TokenBytes();
      std::vector<uint8_t> token(t.data.begin(),
                                 t.data.begin() + (t.data.size() < body
                                                       ? t.data.size()
                                                       : body));
      std::vector<uint8_t> scale;
      if (t.data.size() > body) {
        scale.assign(t.data.begin() + body, t.data.end());
      }
      // 权重那一块同样是正文后面接 scale。
      uint64_t wbody = f->agu.WeightBytes();
      std::vector<uint8_t> weight(w.data.begin(),
                                  w.data.begin() + (w.data.size() < wbody
                                                        ? w.data.size()
                                                        : wbody));
      std::vector<uint8_t> wscale;
      if (w.data.size() > wbody) {
        wscale.assign(w.data.begin() + wbody, w.data.end());
      }
      uint64_t at = f->computed < f->out_addr.size() ? f->out_addr[f->computed]
                                                     : 0;
      // tile 的循环顺序由内往外是 K、专家、N，所以同一列的几段与这一列的几个
      // 专家都连着算。算到最后才产出这一列的结果。
      uint64_t kb = f->Kblock();
      uint64_t ne = f->cfg.Experts();
      uint64_t ki = f->computed % kb;
      uint64_t ei = (f->computed / kb) % ne;
      mu.exe->Issue(f->cfg, token, weight, scale, wscale, at, ki == 0,
                    ki + 1 == kb, ei == 0, ei + 1 == ne, mu.WeightOf(*f, ei));
      ++f->computed;
      f->stage = MuStage::kComputing;
    }

    void Store() {
      if (!mu.exe->HasResult()) return;
      MuInflight* f = mu.iq->FirstToStore();
      if (f == nullptr || !mu.stq->HasRoom()) return;
      MatrixExe::Result r = mu.exe->TakeResult();
      mu.stq->Push(r.addr, r.out, r.cfg.out_bf16);
      ++f->stored;
      f->stage = MuStage::kStoring;
    }

    void Retire() {
      // 写回队列空了几拍。F41：结果写回 Core Mem 之后才与 issue_q 的 finish
      // 合成 dsa_done。离开 stq 的写请求还要经端口到存储，所以要空过两拍：
      // 报早了下游按完成往下走，读到的是这一段的旧值。
      if (mu.stq->Quiescent()) {
        ++stq_idle;
      } else {
        stq_idle = 0;
      }
      MuInflight* f = mu.iq->Head();
      if (f == nullptr || !f->Done() || f->stage == MuStage::kFinished) {
        mu.done->Idle();
        mu.iq->RetireFront();
        return;
      }
      if (stq_idle < 2) {
        mu.done->Idle();
        return;
      }
      // 全部 tile 都写回后与 issue_q 的 finish 合成 dsa_done。波形那一路每笔完成
      // 都记一笔（spans 靠 dsa_start / dsa_done 配对出「忙」段）；报 TS 的 dsa_done
      // 只有 task_last 那一笔，其余任务完成只推进 issue_q，不通知 TS。
      done_task = f->cfg.task_id;
      done_user = f->cfg.user_id;
      ++done_cnt;
      if (f->cfg.task_last) {
        mu.done->Drive(f->cfg.stream_id, f->cfg.task_id);
      } else {
        mu.done->Idle();
      }
      f->stage = MuStage::kFinished;
      mu.iq->RetireFront();
    }

    Mu& mu;
    MuDrain drain = MuDrain::kNone;
    uint64_t drain_cnt = 0;
    uint64_t stq_idle = 0;
    // 宣告完成的笔数与身份，供 Core 层发波形。
    uint64_t done_cnt = 0, done_task = 0, done_user = 0;
  };

  ClockPtr clk;
  MuCfg cfg;
  // 执行通路连着空了几拍，用来判 SYS_STATUS.BUSY 落下去。
  uint64_t idle_cycles = 0;
  // topK 数据线最近一笔写进来的序号，按它去重。
  uint64_t last_topk_seq = 0;
  std::unique_ptr<MuRegfile> reg;
  std::unique_ptr<MuIssueQ> iq;
  std::unique_ptr<GenEpInfo> ep;
  std::unique_ptr<MuLdq> token_ldq, weight_ldq;
  std::unique_ptr<MatrixExe> exe;
  std::unique_ptr<MuStq> stq;
  std::shared_ptr<MuTopkPort> topk_port;
  std::shared_ptr<DonePort> done;
  std::unique_ptr<Ctrl> ctrl;
};

}  // namespace bach
}  // namespace latch

#endif
