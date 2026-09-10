#ifndef _LATCH_BACH_IP_CHIP_CORE_RV_CORE_EXEC_
#define _LATCH_BACH_IP_CHIP_CORE_RV_CORE_EXEC_

// M2 · 取指与执行，以及 M6 · task_done。
//
// 指令逐条执行，不建流水线：pc_gen、loop_bp、decode、dispatch、双发射、gpr 端口、
// SEU 的乘除多拍、DTCM 的 bank 冲突都折算进这一拍。功能由 src/rv32 的模型算，
// 本级只管拍数与记账。
//
// gpr 就绪表是这一层的核心：发出访存或 DSA 读时把目的寄存器标成未就绪，指令
// 读到未就绪的源寄存器就等，等到写回才继续。延迟因此记在这张表上而不是记在
// 「停多少拍不取指」上 —— 后者会把「读了别的寄存器可以先走」那点重叠也吃掉。
//
// 数据与时序分开：功能模型的 load 是同步返回的，指令执行那一刻数据就落进了
// gpr；就绪表管的是「这个值从哪一拍起算数」。对顺序单发射的核，两者合起来就是
// 真实行为。
//
// 自定义指令走 MMIO：rv32 的功能模型认标准 RV32IM，加 custom-0 那一组要动 gen/
// 里 codegen 出来的解码表。改成往约定地址写一笔，行为等价 —— 都是「往一个约定
// 地址写一笔就触发」，kernel 那边本来也是用 store 配 DSA 寄存器的。

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/ip/chip/core/rv_core/rv_ports.h"
#include "bach/ip/chip/core/rv_core/task_queue.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/module_base.h"
#include "rv32/isa/engine.h"
#include "rv32/rv32.h"
#include "rv32/scalar.h"

namespace latch {
namespace bach {

// 三个实例。
enum class RvUnit : uint32_t {
  kDte = 0,
  kMu = 1,
  kVu = 2,
};

// 一条指令用到哪几个通用寄存器。RV32 的字段位置是定长的，按 opcode 判断哪几个
// 字段有效就够 —— 不必去动 gen/ 里 codegen 出来的解码表。
struct RvRegUse {
  bool has_rs1 = false, has_rs2 = false, has_rd = false;
  uint32_t rs1 = 0, rs2 = 0, rd = 0;
};

inline RvRegUse RvDecodeRegs(uint32_t inst) {
  RvRegUse u;
  u.rs1 = (inst >> 15) & 0x1Fu;
  u.rs2 = (inst >> 20) & 0x1Fu;
  u.rd = (inst >> 7) & 0x1Fu;
  switch (inst & 0x7Fu) {
    case 0x37: case 0x17: case 0x6F:        // LUI / AUIPC / JAL
      u.has_rd = true;
      break;
    case 0x67: case 0x03: case 0x13:        // JALR / LOAD / OP-IMM
      u.has_rd = true;
      u.has_rs1 = true;
      break;
    case 0x33:                              // OP
      u.has_rd = true;
      u.has_rs1 = true;
      u.has_rs2 = true;
      break;
    case 0x63: case 0x23:                   // BRANCH / STORE
      u.has_rs1 = true;
      u.has_rs2 = true;
      break;
    case 0x73:                              // SYSTEM（含 CSR）
      u.has_rd = true;
      u.has_rs1 = true;
      break;
    default:                                // FENCE 与未识别的，当作不碰寄存器
      break;
  }
  // x0 恒为零，不参与就绪判定。
  if (u.rs1 == 0) u.has_rs1 = false;
  if (u.rs2 == 0) u.has_rs2 = false;
  if (u.rd == 0) u.has_rd = false;
  return u;
}

// 挂在地址空间上的 MMIO 设备：写它就等于对外发一笔请求。
//
// 这几个设备的 Read / Write 在功能模型的执行流里同步返回，真正的对外请求推进
// 队列、由模块的 Step() 逐拍发出去。这样功能与时序分开：功能模型算的是数据，
// 拍数由外面这层记。
class MmioSink : public systeml::MemoryPort {
 public:
  struct Req {
    bool we = false;
    uint64_t addr = 0;
    uint64_t data = 0;
  };

  // 自定义 CSR 那一档是同步返回的：读它就该当场拿到当前这一笔 task 的身份。
  // 没有这个回调时读回的是上一次写进来的值，那对应 DSA 寄存器读的语义 ——
  // 发出去就走，数据由 dsa_rq 按记录的顺序写回。
  using ReadFn = std::function<uint64_t(uint64_t offset)>;

  MmioSink(uint64_t base_addr, uint64_t span) : base(base_addr), bytes(span) {}

  void SetReadFn(ReadFn fn) { read_fn = std::move(fn); }

  uint64_t Size() override { return bytes; }

  int Read(uint8_t* buf, uint64_t addr, uint64_t len) override {
    Req r;
    r.we = false;
    r.addr = addr;
    q.push_back(r);
    uint64_t v = read_fn ? read_fn(addr - base) : rdata;
    for (uint64_t i = 0; i < len; ++i) {
      buf[i] = i < 8 ? uint8_t((v >> (8 * i)) & 0xFFu) : 0;
    }
    return int(len);
  }

  int Write(uint8_t* buf, uint64_t addr, uint64_t len) override {
    Req r;
    r.we = true;
    r.addr = addr;
    r.data = 0;
    for (uint64_t i = 0; i < len && i < 8; ++i) {
      r.data |= uint64_t(buf[i]) << (8 * i);
    }
    q.push_back(r);
    return int(len);
  }

  bool Empty() const { return q.empty(); }
  Req const& Front() const { return q.front(); }
  void Pop() { q.pop_front(); }
  void SetRdata(uint64_t v) { rdata = v; }
  uint64_t Base() const { return base; }

 private:
  uint64_t base, bytes;
  uint64_t rdata = 0;
  ReadFn read_fn;
  std::deque<Req> q;
};

class RvExec : public BachModule {
 public:
  RvExec(ClockPtr clock, const std::string& name, RvUnit which,
         uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        unit(which),
        start(std::make_shared<TaskStartPort>(clock)),
        finish(std::make_shared<TaskDonePort>(clock)),
        done(std::make_shared<DonePort>(clock)),
        dsa_req(std::make_shared<DsaReqPort>(clock)),
        dsa_ids(std::make_shared<DsaIdsPort>(clock)),
        lsq_req(std::make_shared<LsqReqPort>(clock)),
        dsa_wb(std::make_shared<GprWbPort>(clock)),
        lsq_wb(std::make_shared<GprWbPort>(clock)),
        insts(clock),
        waits(clock),
        pc_sig(clock) {
    ready.fill(true);
    BuildFunctional();
  }

  // ── 对外 ──
  std::shared_ptr<TaskStartPort> StartPtr() const { return start; }
  void AttachStart(std::shared_ptr<TaskStartPort> p) { start = std::move(p); }
  std::shared_ptr<TaskDonePort> FinishPtr() const { return finish; }
  void AttachFinish(std::shared_ptr<TaskDonePort> p) { finish = std::move(p); }
  DonePort& Done() { return *done; }
  void AttachDone(std::shared_ptr<DonePort> p) { done = std::move(p); }
  std::shared_ptr<DsaReqPort> DsaReqPtr() const { return dsa_req; }
  void AttachDsaReq(std::shared_ptr<DsaReqPort> p) { dsa_req = std::move(p); }
  std::shared_ptr<DsaIdsPort> DsaIdsPtr() const { return dsa_ids; }
  void AttachDsaIds(std::shared_ptr<DsaIdsPort> p) { dsa_ids = std::move(p); }
  std::shared_ptr<LsqReqPort> LsqReqPtr() const { return lsq_req; }
  void AttachLsqReq(std::shared_ptr<LsqReqPort> p) { lsq_req = std::move(p); }
  void AttachDsaWb(std::shared_ptr<GprWbPort> p) { dsa_wb = std::move(p); }
  void AttachLsqWb(std::shared_ptr<GprWbPort> p) { lsq_wb = std::move(p); }

  // boot 期由 ctrl_noc 装载 ITCM 与 DTCM。
  void LoadImage(std::string const& path) { sys->LoadProgramFile(path); }
  void PokeItcm(uint64_t addr, std::vector<uint8_t> const& bytes) {
    local->Write(const_cast<uint8_t*>(bytes.data()), addr, bytes.size());
  }

  // 三个 RV core 都进 wait 后 core 把 ready 拉高，SCP 据此开放业务接收。
  bool AtWait() const { return !running; }
  uint64_t Insts() const { return inst_cnt; }
  uint64_t Waits() const { return wait_cnt; }
  uint64_t Pc() const { return sys->GetPC(0); }
  bool Quiescent() const override { return !running; }

 protected:
  void Step() override {
    dsa_used = false;
    lsq_used = false;

    // 末级先做：先收写回（就绪位立起来），再执行，最后接新 task。
    TakeWriteback();
    RunOne();
    TakeStart();

    if (!dsa_used) dsa_req->Idle();
    if (!lsq_used) lsq_req->Idle();
    // 四个身份信号直连本核那个 DSA，每拍驱动。软件改 user_id 或 task_id 之后，
    // 下一次写 trigger 采到的就是新值。
    dsa_ids->Drive(cur.stream_id, cur.task_id, cur.user_id, cur.path_id);

    insts = inst_cnt;
    waits = wait_cnt;
    pc_sig = sys->GetPC(0);
    TracePerCycle("pc", sys->GetPC(0));
    TracePerCycle("insts", inst_cnt);
  }

 private:
  uint64_t DsaIoBase() const {
    if (unit == RvUnit::kDte) return kDteIoBase;
    if (unit == RvUnit::kMu) return kMuIoBase;
    return kVuIoBase;
  }
  uint64_t DsaIoSize() const {
    return unit == RvUnit::kVu ? kVuIoSize : kDsaIoSize;
  }

  void BuildFunctional() {
    isa = std::make_shared<rv32::Rv32>();
    router = std::make_shared<systeml::MemoryRouter>();
    sys = std::make_shared<rv32::SystemRv32>(isa, kItcmBase, 1);
    auto mem = std::make_shared<rv32::MemorySysRouted>(router);
    sys->memorySystem = mem;

    dsa_io = std::make_shared<MmioSink>(DsaIoBase(), DsaIoSize());
    task_ctrl = std::make_shared<MmioSink>(kTaskCtrlBase, kTaskCtrlSize);
    task_ctrl->SetReadFn([this](uint64_t at) { return CsrOf(at); });
    share_mem = std::make_shared<MmioSink>(kShareMemBase, kShareMemSize);
    core_mem = std::make_shared<MmioSink>(kCoreMemBase, kCoreMemSize);
    local = std::make_shared<systeml::MemoryBase>();
    dtcm = std::make_shared<systeml::MemoryBase>();

    // ITCM 与 DTCM 各挂一段，中间那一大块留给 DSA IO 与 task 控制区。一段盖住
    // 另一段的地址，路由就按谁先注册挑，不留这种含糊：各段互不重叠。
    router->AddPort(local, kItcmBase, kItcmSize, 0, 0, "itcm");
    router->AddPort(dtcm, kDtcmBase, kDtcmSize, 0, 0, "dtcm");
    router->AddPort(dsa_io, DsaIoBase(), DsaIoSize(), 1, 0, "dsa");
    router->AddPort(task_ctrl, kTaskCtrlBase, kTaskCtrlSize, 1, 0, "taskctrl");
    router->AddPort(share_mem, kShareMemBase, kShareMemSize, 1, 0, "smem");
    // Core Mem 只有 DTE core 看得见。
    if (unit == RvUnit::kDte) {
      router->AddPort(core_mem, kCoreMemBase, kCoreMemSize, 1, 0, "cmem");
    }

    engine = std::make_unique<rv32::Engine>(/*maxCycles=*/~0ull);
    engine->Add(sys.get(), 0);
    sys->SetHaltState(true, 0);   // 复位后等 task，不自己跑
    ResetAbi();
  }

  // firmware 的 _start 头两条做的事：栈顶与全局指针。模型里 RV core 不跑
  // firmware —— TS 下发任务时直接跳到那一笔的 task_pc —— 所以这两个寄存器在
  // 这里给初值。不给的话 kernel 里但凡用一次栈，地址就落到 0 减去帧长那里。
  //
  // 栈顶取 DTCM 的顶，往下长；全局指针取链接脚本里 .data 起点加 0x800，与
  // compiler/kernel/link.ld 的 global_pointer 同一个地址。
  void ResetAbi() {
    auto& sr = sys->registerSystem->GetRegFile(rv32::REG_SR, 0);
    sr[2].Retype(uint32) = uint32_t(kDtcmBase + kDtcmSize);
    sr[3].Retype(uint32) = uint32_t(kDtcmBase + 0x800);
  }

  // 收 dsa_rq 与 lsq 的写回：把就绪位立起来。
  void TakeWriteback() {
    if (dsa_wb->Valid() && dsa_wb->Seq() != last_dsa_wb) {
      last_dsa_wb = dsa_wb->Seq();
      // 读 DSA 寄存器那一路带着数据回来：功能模型执行 lw 那一拍拿到的是占位
      // 值，真正的内容这时候才补进目的寄存器。
      if (dsa_wb->HasData() && dsa_wb->RdIdx() != 0) {
        sys->registerSystem->GetRegFile(rv32::REG_SR, 0)[dsa_wb->RdIdx()]
            .Retype(uint32) = uint32_t(dsa_wb->Data());
      }
      SetReady(dsa_wb->RdIdx());
    }
    if (lsq_wb->Valid() && lsq_wb->Seq() != last_lsq_wb) {
      last_lsq_wb = lsq_wb->Seq();
      // Share Mem 与 Core Mem 那两路同理：功能模型这边只有占位值，读回来的
      // 内容这时候才补进目的寄存器。DTCM 走功能模型自己的存储，不带数据。
      if (lsq_wb->HasData() && lsq_wb->RdIdx() != 0) {
        sys->registerSystem->GetRegFile(rv32::REG_SR, 0)[lsq_wb->RdIdx()]
            .Retype(uint32) = uint32_t(lsq_wb->Data());
      }
      SetReady(lsq_wb->RdIdx());
    }
  }

  void TakeStart() {
    start->DriveReady(!running);
    if (running || !start->Valid() || start->Seq() == last_start_seq) return;
    last_start_seq = start->Seq();
    cur.task_pc = start->task_pc.Get();
    cur.stream_id = start->stream_id.Get();
    cur.task_id = start->task_id.Get();
    cur.user_id = start->user_id.Get();
    cur.path_id = start->path_id.Get();
    cur.dsa_en = start->dsa_en.Get() != 0;
    sys->SetPC(cur.task_pc, 0);
    sys->SetHaltState(false, 0);
    running = true;
  }

  void RunOne() {
    finish->Idle();
    done->Idle();
    if (!running) return;

    // 读到未就绪的源寄存器就等。等的这一拍不取指，pc 保持。
    uint64_t pc = sys->GetPC(0);
    uint32_t inst = sys->LoadMem<uint32_t>(pc);
    RvRegUse u = RvDecodeRegs(inst);
    if ((u.has_rs1 && !ready[u.rs1]) || (u.has_rs2 && !ready[u.rs2])) {
      ++wait_cnt;
      return;
    }
    // DSA 那条通路还没收走上一笔就不取指。这一条要是也访问 DSA 寄存器，发出去
    // 会把上一笔盖掉 —— 配 DSA 是一串连着的写，盖掉一笔那一笔就没配上。
    if (!dsa_req->Ready()) {
      ++wait_cnt;
      return;
    }

    engine->Cycle();
    ++inst_cnt;

    // 这一条产生的对外请求收出来，按去处分发。
    DrainSinks(u);

    if (sys->GetHaltState(0)) running = false;
  }

  // 把功能模型这一条指令产生的 MMIO 请求转成端口动作。发出去的那一笔把目的
  // 寄存器标成未就绪，写回时再立起来。
  void DrainSinks(RvRegUse const& u) {
    while (!dsa_io->Empty()) {
      MmioSink::Req r = dsa_io->Front();
      dsa_io->Pop();
      uint64_t at = r.addr - dsa_io->Base();
      if (r.we) {
        Send(true, at, r.data, 0);
      } else {
        // DSA 读寄存器不支持同步返回：目的寄存器标成未就绪，等 dsa_rq 写回。
        uint64_t rd = u.has_rd ? u.rd : 0;
        Send(false, at, 0, rd);
        if (rd != 0) ClearReady(rd);
      }
    }
    while (!share_mem->Empty()) {
      MmioSink::Req r = share_mem->Front();
      share_mem->Pop();
      SendLsq(LsqTarget::kShareMem, r, u, r.addr - share_mem->Base());
    }
    while (!core_mem->Empty()) {
      MmioSink::Req r = core_mem->Front();
      core_mem->Pop();
      SendLsq(LsqTarget::kCoreMem, r, u, r.addr - core_mem->Base());
    }
    while (!task_ctrl->Empty()) {
      MmioSink::Req r = task_ctrl->Front();
      task_ctrl->Pop();
      HandleTaskCtrl(r);
    }
  }

  void Send(bool we, uint64_t addr, uint64_t data, uint64_t rd) {
    LOGCHECK(!dsa_used, "RvExec: 一拍里发了两笔 DSA 请求。");
    dsa_req->Drive(we, addr, data, rd, ++dsa_seq);
    dsa_used = true;
  }

  void SendLsq(LsqTarget to, MmioSink::Req const& r, RvRegUse const& u,
               uint64_t offset) {
    LOGCHECK(!lsq_used, "RvExec: 一拍里发了两笔访存。");
    uint64_t rd = (!r.we && u.has_rd) ? u.rd : 0;
    lsq_req->Drive(to, r.we, offset, r.data, 0xF, rd, ++lsq_seq);
    lsq_used = true;
    if (rd != 0) ClearReady(rd);
  }

  // 自定义 CSR 的值。读它当场返回，不排队。
  uint64_t CsrOf(uint64_t at) const {
    if (at == kCsrStreamId) return cur.stream_id;
    if (at == kCsrTaskId) return cur.task_id;
    if (at == kCsrUserId) return cur.user_id;
    return 0;
  }

  // task 控制区：自定义 CSR 的读口与 task_done 的写口。
  void HandleTaskCtrl(MmioSink::Req const& r) {
    uint64_t at = r.addr - task_ctrl->Base();
    if (!r.we) return;   // 读已经在 CsrOf 里当场返回了
    if (at == kCsrUserId) {
      // 自启动的 B / R core：软件在 flag_check 认出这一笔属于哪个用户之后写进
      // 来，随 task_done 回 TS，由 TS 的 completion 写口补进 stream_table。
      cur.user_id = r.data;
      return;
    }
    if (at == kCsrTaskId) {
      // 异步 datain 任务是例外：软件识别包头后写入，告诉 TS 是任务链里哪一步。
      cur.task_id = r.data;
      return;
    }
    if (at == kRegTaskDone) {
      // 交还自己。带 TS 标志时另外通知 TS。
      bool to_ts = r.data != 0;
      finish->Drive(to_ts, ++finish_seq);
      if (to_ts) done->Drive(cur.stream_id, cur.task_id, 0, cur.user_id);
      sys->SetHaltState(true, 0);
      running = false;
    }
  }

  void ClearReady(uint64_t idx) {
    if (idx < ready.size()) ready[idx] = false;
  }
  void SetReady(uint64_t idx) {
    if (idx < ready.size()) ready[idx] = true;
  }

  RvUnit unit;
  std::shared_ptr<TaskStartPort> start;
  std::shared_ptr<TaskDonePort> finish;
  std::shared_ptr<DonePort> done;
  std::shared_ptr<DsaReqPort> dsa_req;
  std::shared_ptr<DsaIdsPort> dsa_ids;
  std::shared_ptr<LsqReqPort> lsq_req;
  std::shared_ptr<GprWbPort> dsa_wb, lsq_wb;

  std::shared_ptr<rv32::Rv32> isa;
  std::shared_ptr<systeml::MemoryRouter> router;
  std::shared_ptr<rv32::SystemRv32> sys;
  std::unique_ptr<rv32::Engine> engine;
  std::shared_ptr<systeml::MemoryBase> local, dtcm;
  std::shared_ptr<MmioSink> dsa_io, task_ctrl, share_mem, core_mem;

  // gpr 就绪表：32 位，复位全 1。
  std::array<bool, 32> ready{};
  RvTask cur;
  bool running = false, dsa_used = false, lsq_used = false;
  uint64_t last_start_seq = 0, last_dsa_wb = 0, last_lsq_wb = 0;
  uint64_t dsa_seq = 0, lsq_seq = 0, finish_seq = 0;
  uint64_t inst_cnt = 0, wait_cnt = 0;

  Logic64 insts, waits, pc_sig;
};

}  // namespace bach
}  // namespace latch

#endif
