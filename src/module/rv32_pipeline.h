#ifndef _LATCH_MODULE_RV32_PIPELINE_
#define _LATCH_MODULE_RV32_PIPELINE_

// RV32IM 顺序流水线：五个执行级各自是一个独立的 ClkModule
//
//   Rv32Fetch → Rv32Decode → Rv32Execute → Rv32Mem → Rv32Writeback
//      │            ↑                         │
//      │            └──── 寄存器堆写口 ───────┼──── Rv32Writeback
//      ↓                                      ↓
//   Rv32Lim(ILM 取指口)              Rv32Lim(ILM 数据口) / Rv32Lim(DLM) / Rv32Mmio
//
// 级与级之间只有两种连接，都是 base/ 里的时序原语，没有任何裸成员穿透：
//
//   * 数据通路走 Fifo<Pkt>，Pkt 是 Logic 子类、字段都用 Fields 注册。
//     一次 Push 到对方读到正好隔一拍，这就是流水线寄存器。
//   * 反馈信号走 Logic64：EX 的分支重定向广播给 IF 与 ID，MEM 与 WB 的在途
//     结果广播给 EX 做前推。同样是上拍写、下拍读，对应 RTL 里的寄存器输出。
//
// 存储器也是独立模块（见 module/rv32_mem.h），端口同样是 Fifo，因此一次访问
// 是两拍往返。取指与访存都做成固定延迟的流水段，稳态吞吐仍然是一拍一条：
//
//   IF 发地址 ─2拍→ 收指令 → ID → EX → MEM 三拍(发地址/在途/收数据) → WB
//
// 由此得到的时序参数：
//
//   * 分支在 EX 判定，重定向经 Logic64 到 IF 要一拍，取指再要两拍，
//     所以一次跳转的代价是 4 拍气泡。
//   * load 的数据在它进 MEM 后的第三拍才回来，紧跟其后的三条指令若用到它
//     就得停顿；再往后的指令由前推网络覆盖。
//   * 前推源共五路，按新旧排优先级：EX 上拍结果、MEM 三个槽、WB 上拍结果。
//     更早的指令已经写进寄存器堆，直接读得到。
//
// 地址空间：
//
//   0x10000000 .. 0x10001FFF   MMIO：UART 与仿真控制寄存器
//   0x80000000 .. 0x8003FFFF   ILM  256KB，双端口（取指口 + 数据口）
//   0x80040000 .. 0x8007FFFF   DLM  256KB
//
// 波形：每个模块把自己的线网逐拍写进 trace，层次是 rv32_core.<级>.<信号>。

#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/clock.h"
#include "base/fifo.h"
#include "base/log.h"
#include "base/logic.h"
#include "base/module.h"
#include "module/rv32_isa.h"
#include "module/rv32_mem.h"

namespace latch {

// ---------------------------------------------------------------- 配置

// 级间 Fifo 与存储器端口 Fifo 的深度。取指在路上最多压两拍，加上级间排队，
// 八格足够宽裕，任何一级都不会因为下游没腾出位置而卡住。
constexpr uint64_t kRv32FifoDepth = 8;

struct Rv32Config {
  uint32_t ilm_base = 0x80000000u;
  uint32_t ilm_size = 256u * 1024u;
  uint32_t dlm_base = 0x80040000u;
  uint32_t dlm_size = 256u * 1024u;
  uint32_t mmio_base = 0x10000000u;
  uint32_t mmio_size = 0x2000u;
  uint32_t reset_pc = 0x80000000u;

  // 跑飞保护：到了这个拍数还没停机就强行收尾
  uint64_t max_cycles = 4ull * 1000ull * 1000ull;
};

// 停机原因
enum class Rv32Halt : uint32_t {
  kRunning = 0,
  kSimExit = 1,     // 程序写了仿真退出寄存器
  kEbreak = 2,      // 退休了一条 ebreak
  kEcall = 3,       // 退休了一条 ecall
  kIllegal = 4,     // 退休了一条非法指令
  kFetchFault = 5,  // 取指地址不在 ILM 里
  kLoadFault = 6,   // 访存地址没落在任何一块存储器上
  kMisaligned = 7,  // 访存地址没对齐
  kCycleLimit = 8,  // 到了跑飞保护的拍数上限
};

// ------------------------------------------------------ 级间的包（流水线寄存器）

class Rv32IfIdPkt : public Logic {
 public:
  Logic64 pc, inst, epoch;
  explicit Rv32IfIdPkt(ClockPtr c) : pc(c), inst(c), epoch(c) {
    Fields(pc, inst, epoch);
  }
};

class Rv32IdExPkt : public Logic {
 public:
  Logic64 pc, inst, epoch;
  Logic64 rs1, rs2, rd, rs1_data, rs2_data, imm, funct3;
  Logic64 alu_op, alu_src_a, alu_src_b, wb_sel;
  Logic64 reg_write, mem_read, mem_write, branch, jump, jalr;
  Logic64 uses_rs1, uses_rs2, illegal, ebreak, ecall;

  explicit Rv32IdExPkt(ClockPtr c)
      : pc(c), inst(c), epoch(c),
        rs1(c), rs2(c), rd(c), rs1_data(c), rs2_data(c), imm(c), funct3(c),
        alu_op(c), alu_src_a(c), alu_src_b(c), wb_sel(c),
        reg_write(c), mem_read(c), mem_write(c), branch(c), jump(c), jalr(c),
        uses_rs1(c), uses_rs2(c), illegal(c), ebreak(c), ecall(c) {
    Fields(pc, inst, epoch, rs1, rs2, rd, rs1_data, rs2_data, imm, funct3);
    Fields(alu_op, alu_src_a, alu_src_b, wb_sel);
    Fields(reg_write, mem_read, mem_write, branch, jump, jalr);
    Fields(uses_rs1, uses_rs2, illegal, ebreak, ecall);
  }
};

class Rv32ExMemPkt : public Logic {
 public:
  Logic64 pc, inst, rd, alu_result, store_data, pc_plus4, funct3;
  Logic64 wb_sel, reg_write, mem_read, mem_write, illegal, ebreak, ecall;

  explicit Rv32ExMemPkt(ClockPtr c)
      : pc(c), inst(c), rd(c), alu_result(c), store_data(c), pc_plus4(c),
        funct3(c), wb_sel(c), reg_write(c), mem_read(c), mem_write(c),
        illegal(c), ebreak(c), ecall(c) {
    Fields(pc, inst, rd, alu_result, store_data, pc_plus4, funct3);
    Fields(wb_sel, reg_write, mem_read, mem_write, illegal, ebreak, ecall);
  }
};

class Rv32MemWbPkt : public Logic {
 public:
  Logic64 pc, inst, rd, wb_data, alu_result, load_data;
  Logic64 wb_sel, reg_write, illegal, ebreak, ecall;

  explicit Rv32MemWbPkt(ClockPtr c)
      : pc(c), inst(c), rd(c), wb_data(c), alu_result(c), load_data(c),
        wb_sel(c), reg_write(c), illegal(c), ebreak(c), ecall(c) {
    Fields(pc, inst, rd, wb_data, alu_result, load_data);
    Fields(wb_sel, reg_write, illegal, ebreak, ecall);
  }
};

// WB 送回 ID 的寄存器堆写口
class Rv32RfWritePkt : public Logic {
 public:
  Logic64 rd, data, pc;
  explicit Rv32RfWritePkt(ClockPtr c) : rd(c), data(c), pc(c) {
    Fields(rd, data, pc);
  }
};

// ------------------------------------------------------------ 前推总线

// 一路前推源：valid 表示这一格里确实有条要写寄存器的指令，ready 表示它的
// 结果已经算出来了（load 在头两个 MEM 槽里还没拿到数据，就是 valid 而不 ready）。
class Rv32FwdBus {
 public:
  explicit Rv32FwdBus(ClockPtr c) : valid(c), rd(c), ready(c), data(c) {}

  void Drive(uint32_t v, uint32_t r, uint32_t rdy, uint32_t d) {
    valid = v;
    rd = r;
    ready = rdy;
    data = d;
  }
  void Clear() { Drive(0, 0, 0, 0); }

  Logic64 valid, rd, ready, data;
};

class Rv32Execute;
class Rv32Mem;
class Rv32Writeback;

// ================================================================= IF 级

// 取指：给 ILM 的取指口发地址，两拍后收指令，再送进 IF/ID。
// 发出去还没回来的地址记在 inflight 里，收到指令时按序配对。
class Rv32Fetch : public ClkModule {
 public:
  Rv32Fetch(ClockPtr clock, const std::string& name, uint64_t parent,
            const Rv32Config& config, Rv32MemPort iport,
            FifoPtr<Rv32IfIdPkt> out)
      : ClkModule(clock),
        cfg(config),
        ilm(iport),
        if_id(out),
        pc_sig(clock),
        epoch_sig(clock),
        halt_req(clock) {
    RegisterId(name, parent);
    pc = cfg.reset_pc;
  }

  // EX 通过这个接口把重定向广播过来，IF 在下一拍看到
  void BindRedirect(const Logic64* valid, const Logic64* target,
                    const Logic64* gen) {
    redir_valid = valid;
    redir_target = target;
    redir_epoch = gen;
  }

  // 停机请求：本级只把标志举起来，真正停时钟的是 MMIO 模块
  const Logic64& HaltReq() const { return halt_req; }

  uint32_t Pc() const { return pc; }
  uint32_t Epoch() const { return epoch; }
  bool FetchFault() const { return fetch_fault; }
  uint32_t FaultPc() const { return fault_pc; }

  void Cycle() override {
    DelayCycle(1);

    Wire w;

    // 1. 分支重定向：EX 上拍判定的跳转这一拍到达
    const uint32_t seen = static_cast<uint32_t>(uint64_t(*redir_epoch));
    if (seen != epoch) {
      epoch = seen;
      pc = static_cast<uint32_t>(uint64_t(*redir_target));
      w.redirect = 1;
    }

    // 2. 收指令：ILM 两拍前收下的那个地址，这一拍把指令交回来
    if (!ilm.resp->IsEmpty()) {
      Rv32MemRespPkt& r = ilm.resp->Front();
      const uint32_t inst = static_cast<uint32_t>(uint64_t(r.rdata));
      ilm.resp->Pop();
      LOGCHECK(!inflight.empty(), "Rv32Fetch: response without inflight addr");
      const InFlight f = inflight.front();
      inflight.pop_front();

      w.resp_valid = 1;
      w.resp_pc = f.pc;
      w.resp_inst = inst;
      if (f.epoch != epoch) {
        // 错误路径上取回来的指令，直接丢掉，不占 IF/ID
        w.resp_drop = 1;
      } else {
        LOGCHECK(!if_id->IsFull(), "Rv32Fetch: IF/ID overflow");
        Rv32IfIdPkt p(clk);
        p.pc = f.pc;
        p.inst = inst;
        p.epoch = f.epoch;
        if_id->Push(p);
        w.issue = 1;
      }
    }

    // 3. 发地址：已经排在 IF/ID 里的加上还在路上的，都得留得下位置。
    //    ValidCount 是上拍视图，只会偏大，所以这个判断是保守的。
    const uint64_t booked = if_id->ValidCount() + inflight.size();
    const bool room = booked < kRv32FifoDepth;
    if (room && !ilm.req->IsFull() && !fetch_fault) {
      if (pc < cfg.ilm_base || (pc - cfg.ilm_base) >= cfg.ilm_size ||
          (pc & 3u) != 0) {
        fetch_fault = true;
        fault_pc = pc;
        w.fault = 1;
      } else {
        Rv32MemReqPkt q(clk);
        q.addr = pc;
        q.is_write = 0u;
        q.be = 0u;
        q.wdata = 0u;
        ilm.req->Push(q);
        inflight.push_back({pc, epoch});
        w.req = 1;
        w.req_pc = pc;
        pc += 4u;
      }
    }

    w.pc = pc;
    w.epoch = epoch;
    w.inflight = static_cast<uint32_t>(inflight.size());
    wire = w;

    pc_sig = pc;
    epoch_sig = epoch;
    halt_req = fetch_fault ? 1u : 0u;
    EmitTraces();
  }

 private:
  struct InFlight {
    uint32_t pc;
    uint32_t epoch;
  };
  struct Wire {
    uint32_t pc = 0, epoch = 0, inflight = 0;
    uint32_t req = 0, req_pc = 0;
    uint32_t resp_valid = 0, resp_pc = 0, resp_inst = 0, resp_drop = 0;
    uint32_t issue = 0, redirect = 0, fault = 0;
  };

  void EmitTraces() {
    if (TraceDisabled()) return;
    TracePerCycle("pc", wire.pc);
    TracePerCycle("epoch", wire.epoch);
    TracePerCycle("req", wire.req);
    TracePerCycle("req_addr", wire.req_pc);
    TracePerCycle("inflight", wire.inflight);
    TracePerCycle("resp_valid", wire.resp_valid);
    TracePerCycle("resp_pc", wire.resp_pc);
    TracePerCycle("resp_inst", wire.resp_inst);
    TracePerCycle("resp_drop", wire.resp_drop);
    TracePerCycle("issue", wire.issue);
    TracePerCycle("redirect", wire.redirect);
    TracePerCycle("fetch_fault", wire.fault);
    TracePerCycle("if_id_valid", if_id->ValidCount());
  }

  Rv32Config cfg;
  Rv32MemPort ilm;
  FifoPtr<Rv32IfIdPkt> if_id;

  const Logic64* redir_valid = nullptr;
  const Logic64* redir_target = nullptr;
  const Logic64* redir_epoch = nullptr;

  // Cycle 独占
  uint32_t pc = 0;
  uint32_t epoch = 0;
  std::deque<InFlight> inflight;
  bool fetch_fault = false;
  uint32_t fault_pc = 0;
  Wire wire;

  Logic64 pc_sig, epoch_sig, halt_req;
};

// ================================================================= ID 级

// 译码：拿 IF/ID 的指令，译出控制线，读寄存器堆，做冒险检测，送进 ID/EX。
// 寄存器堆本体归这一级所有，WB 的写口经 Fifo 送回来，每拍先写后读。
class Rv32Decode : public ClkModule {
 public:
  Rv32Decode(ClockPtr clock, const std::string& name, uint64_t parent,
             const Rv32Config& config, FifoPtr<Rv32IfIdPkt> in,
             FifoPtr<Rv32IdExPkt> out, FifoPtr<Rv32RfWritePkt> wb)
      : ClkModule(clock),
        cfg(config),
        if_id(in),
        id_ex(out),
        wb_rf(wb),
        stalls(clock) {
    RegisterId(name, parent);
    for (uint32_t i = 0; i < 32; ++i) {
      rf[i] = 0;
      reg_sig[i] = "x" + std::to_string(i) + "_" + rv32::RegAbi(i);
    }
    // 发射历史：下标就是相对当前拍的距离，距离 1 是上一拍发射的那条
    history.assign(kHistory, HistEntry());
  }

  void BindRedirect(const Logic64* gen) { redir_epoch = gen; }

  // 仿真结束后主线程读
  uint32_t Reg(uint32_t i) const { return rf[i & 31u]; }
  uint64_t StallCount() const { return stall_pending; }

  void Cycle() override {
    DelayCycle(1);

    Wire w;

    // 1. 寄存器堆写口先于读口：WB 上拍送来的写这一拍就生效
    if (!wb_rf->IsEmpty()) {
      Rv32RfWritePkt& p = wb_rf->Front();
      const uint32_t rd = static_cast<uint32_t>(uint64_t(p.rd));
      const uint32_t data = static_cast<uint32_t>(uint64_t(p.data));
      wb_rf->Pop();
      if (rd != 0) rf[rd] = data;
      w.rf_we = (rd != 0) ? 1u : 0u;
      w.rf_waddr = rd;
      w.rf_wdata = data;
    }

    // 2. 分支重定向：换代之后，还在管子里的旧指令一律作废
    epoch = static_cast<uint32_t>(uint64_t(*redir_epoch));
    w.epoch = epoch;

    HistEntry hist;  // 这一拍发射了什么，压进历史给后面几拍做冒险检测

    if (!if_id->IsEmpty() && !id_ex->IsFull()) {
      Rv32IfIdPkt& p = if_id->Front();
      const uint32_t pkt_epoch = static_cast<uint32_t>(uint64_t(p.epoch));
      const uint32_t pc = static_cast<uint32_t>(uint64_t(p.pc));
      const uint32_t inst = static_cast<uint32_t>(uint64_t(p.inst));

      if (pkt_epoch != epoch) {
        if_id->Pop();
        w.flush = 1;
        w.pc = pc;
        w.inst = inst;
      } else {
        const rv32::Rv32Decoded d = rv32::Decode(inst);
        const bool hazard = LoadUseHazard(d);

        w.pc = pc;
        w.inst = inst;
        w.opcode = d.opcode;
        w.rd = d.rd;
        w.rs1 = d.rs1;
        w.rs2 = d.rs2;
        w.funct3 = d.funct3;
        w.funct7 = d.funct7;
        w.imm = d.imm;
        w.reg_write = d.reg_write;
        w.mem_read = d.mem_read;
        w.mem_write = d.mem_write;
        w.branch = d.branch;
        w.jump = d.jump;
        w.jalr = d.jalr;
        w.alu_op = d.alu_op;
        w.alu_src_a = d.alu_src_a;
        w.alu_src_b = d.alu_src_b;
        w.wb_sel = d.wb_sel;
        w.uses_rs1 = d.uses_rs1;
        w.uses_rs2 = d.uses_rs2;
        w.illegal = d.illegal;
        w.rs1_data = rf[d.rs1];
        w.rs2_data = rf[d.rs2];

        if (hazard) {
          ++stall_pending;
          w.stall = 1;
        } else {
          if_id->Pop();

          Rv32IdExPkt q(clk);
          q.pc = pc;
          q.inst = inst;
          q.epoch = pkt_epoch;
          q.rs1 = d.rs1;
          q.rs2 = d.rs2;
          q.rd = d.rd;
          q.rs1_data = w.rs1_data;
          q.rs2_data = w.rs2_data;
          q.imm = d.imm;
          q.funct3 = d.funct3;
          q.alu_op = d.alu_op;
          q.alu_src_a = d.alu_src_a;
          q.alu_src_b = d.alu_src_b;
          q.wb_sel = d.wb_sel;
          q.reg_write = d.reg_write;
          q.mem_read = d.mem_read;
          q.mem_write = d.mem_write;
          q.branch = d.branch;
          q.jump = d.jump;
          q.jalr = d.jalr;
          q.uses_rs1 = d.uses_rs1;
          q.uses_rs2 = d.uses_rs2;
          q.illegal = d.illegal;
          q.ebreak = d.ebreak;
          q.ecall = d.ecall;
          id_ex->Push(q);

          w.issue = 1;
          hist.valid = 1;
          hist.rd = d.rd;
          hist.reg_write = d.reg_write;
          hist.mem_read = d.mem_read;
        }
      }
    } else {
      w.idle = 1;
    }

    history.pop_back();
    history.push_front(hist);

    wire = w;
    stalls = stall_pending;
    EmitTraces();
  }

 private:
  // 发射历史里存的东西：这一格的指令写哪个寄存器、是不是 load
  struct HistEntry {
    uint32_t valid = 0;
    uint32_t rd = 0;
    uint32_t reg_write = 0;
    uint32_t mem_read = 0;
  };
  struct Wire {
    uint32_t pc = 0, inst = 0, epoch = 0;
    uint32_t opcode = 0, rd = 0, rs1 = 0, rs2 = 0, funct3 = 0, funct7 = 0;
    uint32_t imm = 0, rs1_data = 0, rs2_data = 0;
    uint32_t reg_write = 0, mem_read = 0, mem_write = 0;
    uint32_t branch = 0, jump = 0, jalr = 0;
    uint32_t alu_op = 0, alu_src_a = 0, alu_src_b = 0, wb_sel = 0;
    uint32_t uses_rs1 = 0, uses_rs2 = 0, illegal = 0;
    uint32_t issue = 0, stall = 0, flush = 0, idle = 0;
    uint32_t rf_we = 0, rf_waddr = 0, rf_wdata = 0;
    uint32_t hazard_rs1 = 0, hazard_rs2 = 0;
  };

  // load 的数据要等它进 MEM 的第三拍才回来，也就是发射之后第四拍才可前推。
  // 所以只有距离 1 到 3 之内的 load 会挡住当前这条指令。
  bool LoadUseHazard(const rv32::Rv32Decoded& d) {
    bool hit = false;
    for (uint32_t dist = 0; dist < kLoadShadow; ++dist) {
      const HistEntry& h = history[dist];
      if (!h.valid || !h.reg_write || !h.mem_read || h.rd == 0) continue;
      if (d.uses_rs1 && d.rs1 == h.rd) {
        hazard_rs1 = 1;
        hit = true;
      }
      if (d.uses_rs2 && d.rs2 == h.rd) {
        hazard_rs2 = 1;
        hit = true;
      }
    }
    return hit;
  }

  void EmitTraces() {
    if (TraceDisabled()) return;
    TracePerCycle("pc", wire.pc);
    TracePerCycle("inst", wire.inst);
    TracePerCycle("epoch", wire.epoch);
    TracePerCycle("opcode", wire.opcode);
    TracePerCycle("rd", wire.rd);
    TracePerCycle("rs1", wire.rs1);
    TracePerCycle("rs2", wire.rs2);
    TracePerCycle("funct3", wire.funct3);
    TracePerCycle("funct7", wire.funct7);
    TracePerCycle("imm", wire.imm);
    TracePerCycle("rs1_data", wire.rs1_data);
    TracePerCycle("rs2_data", wire.rs2_data);
    TracePerCycle("ctrl_reg_write", wire.reg_write);
    TracePerCycle("ctrl_mem_read", wire.mem_read);
    TracePerCycle("ctrl_mem_write", wire.mem_write);
    TracePerCycle("ctrl_branch", wire.branch);
    TracePerCycle("ctrl_jump", wire.jump);
    TracePerCycle("ctrl_jalr", wire.jalr);
    TracePerCycle("ctrl_alu_op", wire.alu_op);
    TracePerCycle("ctrl_alu_src_a", wire.alu_src_a);
    TracePerCycle("ctrl_alu_src_b", wire.alu_src_b);
    TracePerCycle("ctrl_wb_sel", wire.wb_sel);
    TracePerCycle("ctrl_uses_rs1", wire.uses_rs1);
    TracePerCycle("ctrl_uses_rs2", wire.uses_rs2);
    TracePerCycle("ctrl_illegal", wire.illegal);
    TracePerCycle("issue", wire.issue);
    TracePerCycle("stall", wire.stall);
    TracePerCycle("flush", wire.flush);
    TracePerCycle("idle", wire.idle);
    TracePerCycle("hazard_rs1", hazard_rs1);
    TracePerCycle("hazard_rs2", hazard_rs2);
    TracePerCycle("rf_we", wire.rf_we);
    TracePerCycle("rf_waddr", wire.rf_waddr);
    TracePerCycle("rf_wdata", wire.rf_wdata);
    TracePerCycle("stall_count", stall_pending);
    TracePerCycle("id_ex_valid", id_ex->ValidCount());
    for (uint32_t i = 0; i < 32; ++i) TracePerCycle(reg_sig[i], rf[i]);
    hazard_rs1 = 0;
    hazard_rs2 = 0;
  }

  static constexpr uint32_t kHistory = 8;
  static constexpr uint32_t kLoadShadow = 3;

  Rv32Config cfg;
  FifoPtr<Rv32IfIdPkt> if_id;
  FifoPtr<Rv32IdExPkt> id_ex;
  FifoPtr<Rv32RfWritePkt> wb_rf;
  const Logic64* redir_epoch = nullptr;

  // Cycle 独占
  uint32_t rf[32] = {};
  uint32_t epoch = 0;
  std::deque<HistEntry> history;
  uint64_t stall_pending = 0;
  uint32_t hazard_rs1 = 0, hazard_rs2 = 0;
  Wire wire;
  std::string reg_sig[32];

  Logic64 stalls;
};

// ================================================================= MEM 级

// 访存：三拍的固定流水段。第一拍按地址把请求发给 ILM 数据口、DLM 或 MMIO，
// 第二拍在途，第三拍收数据、拼出写回值、送进 MEM/WB。
// 不访存的指令也照样走满三拍，这样出口顺序与入口一致，前推的位置也是固定的。
class Rv32Mem : public ClkModule {
 public:
  Rv32Mem(ClockPtr clock, const std::string& name, uint64_t parent,
          const Rv32Config& config, FifoPtr<Rv32ExMemPkt> in,
          FifoPtr<Rv32MemWbPkt> out, Rv32MemPort ilm_port,
          Rv32MemPort dlm_port, Rv32MemPort mmio_port)
      : ClkModule(clock),
        cfg(config),
        ex_mem(in),
        mem_wb(out),
        ilm(ilm_port),
        dlm(dlm_port),
        mmio(mmio_port),
        fwd1(clock),
        fwd2(clock),
        fwd3(clock),
        halt_req(clock) {
    RegisterId(name, parent);
  }

  // 停机请求：本级只把标志举起来，真正停时钟的是 MMIO 模块
  const Logic64& HaltReq() const { return halt_req; }

  const Rv32FwdBus& Fwd1() const { return fwd1; }
  const Rv32FwdBus& Fwd2() const { return fwd2; }
  const Rv32FwdBus& Fwd3() const { return fwd3; }

  Rv32Halt Fault() const { return fault; }
  uint32_t FaultAddr() const { return fault_addr; }

  void Cycle() override {
    DelayCycle(1);

    Wire w;

    // 1. 两级槽整体右移，同时把 EX/MEM 队首接进第一槽并发出访存请求。
    //    移出来的这条走满了三拍：发地址、在途、收数据，正好赶上响应回来。
    Slot done = s2;
    s2 = s1;
    s1 = Slot();

    if (!ex_mem->IsEmpty() && fault == Rv32Halt::kRunning) {
      LOGCHECK(!mem_wb->IsFull(),
               "Rv32Mem: MEM/WB back-pressured — the fixed three-cycle "
               "memory stage must never stall");
      Rv32ExMemPkt& p = ex_mem->Front();
      s1.valid = 1;
      s1.pc = static_cast<uint32_t>(uint64_t(p.pc));
      s1.inst = static_cast<uint32_t>(uint64_t(p.inst));
      s1.rd = static_cast<uint32_t>(uint64_t(p.rd));
      s1.alu_result = static_cast<uint32_t>(uint64_t(p.alu_result));
      s1.store_data = static_cast<uint32_t>(uint64_t(p.store_data));
      s1.pc_plus4 = static_cast<uint32_t>(uint64_t(p.pc_plus4));
      s1.funct3 = static_cast<uint32_t>(uint64_t(p.funct3));
      s1.wb_sel = static_cast<uint32_t>(uint64_t(p.wb_sel));
      s1.reg_write = static_cast<uint32_t>(uint64_t(p.reg_write));
      s1.mem_read = static_cast<uint32_t>(uint64_t(p.mem_read));
      s1.mem_write = static_cast<uint32_t>(uint64_t(p.mem_write));
      s1.illegal = static_cast<uint32_t>(uint64_t(p.illegal));
      s1.ebreak = static_cast<uint32_t>(uint64_t(p.ebreak));
      s1.ecall = static_cast<uint32_t>(uint64_t(p.ecall));
      ex_mem->Pop();

      if (s1.mem_read || s1.mem_write) IssueAccess(s1, w);
    }

    // 2. 走完三拍的那条：把存储器的响应接回来，拼出写回值，交给 WB
    if (done.valid) {
      if (done.mem_read || done.mem_write) CollectResponse(done, w);
      switch (done.wb_sel) {
        case rv32::kWbMem: done.wb_data = done.load_data; break;
        case rv32::kWbPc4: done.wb_data = done.pc_plus4; break;
        default: done.wb_data = done.alu_result; break;
      }

      LOGCHECK(!mem_wb->IsFull(), "Rv32Mem: MEM/WB overflow");
      Rv32MemWbPkt q(clk);
      q.pc = done.pc;
      q.inst = done.inst;
      q.rd = done.rd;
      q.wb_data = done.wb_data;
      q.alu_result = done.alu_result;
      q.load_data = done.load_data;
      q.wb_sel = done.wb_sel;
      q.reg_write = done.reg_write;
      q.illegal = done.illegal;
      q.ebreak = done.ebreak;
      q.ecall = done.ecall;
      mem_wb->Push(q);

      w.retire = 1;
      w.retire_pc = done.pc;
      w.retire_data = done.wb_data;
    }

    // 3. 三拍上各自那条指令都送上前推总线，EX 下一拍就能用。
    //    前两拍的 load 只有地址还没有数据，DriveFwd 会把 ready 标成 0。
    DriveFwd(fwd1, s1, false);
    DriveFwd(fwd2, s2, false);
    DriveFwd(fwd3, done, true);

    w.s1_valid = s1.valid;
    w.s1_pc = s1.pc;
    w.s2_valid = s2.valid;
    w.s2_pc = s2.pc;
    w.s3_valid = done.valid;
    w.s3_pc = done.pc;
    wire = w;

    halt_req = (fault != Rv32Halt::kRunning) ? 1u : 0u;
    EmitTraces();
  }

 private:
  struct Slot {
    uint32_t valid = 0;
    uint32_t pc = 0, inst = 0, rd = 0;
    uint32_t alu_result = 0, store_data = 0, pc_plus4 = 0, funct3 = 0;
    uint32_t wb_sel = 0, reg_write = 0, mem_read = 0, mem_write = 0;
    uint32_t illegal = 0, ebreak = 0, ecall = 0;
    uint32_t load_data = 0, wb_data = 0;
    uint32_t target = 0;  // 0=无 1=ilm 2=dlm 3=mmio
  };
  struct Wire {
    uint32_t en = 0, we = 0, addr = 0, be = 0, wdata = 0, size = 0;
    uint32_t target = 0, misaligned = 0, fault = 0;
    uint32_t resp_valid = 0, rdata = 0, load_data = 0;
    uint32_t retire = 0, retire_pc = 0, retire_data = 0;
    uint32_t s1_valid = 0, s1_pc = 0;
    uint32_t s2_valid = 0, s2_pc = 0;
    uint32_t s3_valid = 0, s3_pc = 0;
  };

  void IssueAccess(Slot& s, Wire& w) {
    const uint32_t addr = s.alu_result;
    const uint32_t size = rv32::AccessSize(s.funct3);

    w.en = 1;
    w.we = s.mem_write;
    w.addr = addr;
    w.size = size;

    if ((addr & (size - 1u)) != 0) {
      fault = Rv32Halt::kMisaligned;
      fault_addr = addr;
      w.misaligned = 1;
      s.valid = 0;
      return;
    }

    Rv32MemPort* port = nullptr;
    if (addr >= cfg.dlm_base && (addr - cfg.dlm_base) < cfg.dlm_size) {
      port = &dlm;
      s.target = 2;
    } else if (addr >= cfg.ilm_base && (addr - cfg.ilm_base) < cfg.ilm_size) {
      port = &ilm;
      s.target = 1;
    } else if (addr >= cfg.mmio_base && (addr - cfg.mmio_base) < cfg.mmio_size) {
      port = &mmio;
      s.target = 3;
    } else {
      fault = Rv32Halt::kLoadFault;
      fault_addr = addr;
      w.fault = 1;
      s.valid = 0;
      return;
    }

    const uint32_t be = s.mem_write ? rv32::ByteEnable(s.funct3, addr) : 0xFu;
    const uint32_t wdata =
        s.mem_write ? rv32::AlignStore(s.store_data, addr) : 0u;

    LOGCHECK(!port->req->IsFull(), "Rv32Mem: memory request fifo overflow");
    Rv32MemReqPkt q(clk);
    q.addr = addr & ~3u;
    q.is_write = s.mem_write;
    q.be = be;
    q.wdata = wdata;
    port->req->Push(q);

    w.be = be;
    w.wdata = wdata;
    w.target = s.target;
  }

  void CollectResponse(Slot& s, Wire& w) {
    Rv32MemPort* port = nullptr;
    if (s.target == 1) port = &ilm;
    else if (s.target == 2) port = &dlm;
    else if (s.target == 3) port = &mmio;
    if (port == nullptr) return;

    LOGCHECK(!port->resp->IsEmpty(),
             "Rv32Mem: expected memory response is not back yet");
    Rv32MemRespPkt& r = port->resp->Front();
    const uint32_t rdata = static_cast<uint32_t>(uint64_t(r.rdata));
    port->resp->Pop();

    s.load_data =
        s.mem_read ? rv32::ExtractLoad(rdata, s.funct3, s.alu_result) : 0u;
    w.resp_valid = 1;
    w.rdata = rdata;
    w.load_data = s.load_data;
  }

  // ready 为假表示这一格里是条还没拿到数据的 load，冒险检测本该把用它的
  // 指令挡在 ID 级，所以 EX 一旦选中这样的源就是设计出了问题
  void DriveFwd(Rv32FwdBus& bus, const Slot& s, bool data_ready) {
    if (!s.valid || !s.reg_write || s.rd == 0) {
      bus.Clear();
      return;
    }
    const bool ready = data_ready || (s.wb_sel != rv32::kWbMem);
    uint32_t value = 0;
    if (ready) {
      switch (s.wb_sel) {
        case rv32::kWbMem: value = s.load_data; break;
        case rv32::kWbPc4: value = s.pc_plus4; break;
        default: value = s.alu_result; break;
      }
    }
    bus.Drive(1, s.rd, ready ? 1u : 0u, value);
  }

  void EmitTraces() {
    if (TraceDisabled()) return;
    TracePerCycle("en", wire.en);
    TracePerCycle("we", wire.we);
    TracePerCycle("addr", wire.addr);
    TracePerCycle("be", wire.be);
    TracePerCycle("size", wire.size);
    TracePerCycle("wdata", wire.wdata);
    TracePerCycle("target", wire.target);
    TracePerCycle("resp_valid", wire.resp_valid);
    TracePerCycle("rdata", wire.rdata);
    TracePerCycle("load_data", wire.load_data);
    TracePerCycle("misaligned", wire.misaligned);
    TracePerCycle("fault", wire.fault);
    TracePerCycle("retire", wire.retire);
    TracePerCycle("retire_pc", wire.retire_pc);
    TracePerCycle("retire_data", wire.retire_data);
    TracePerCycle("s1_valid", wire.s1_valid);
    TracePerCycle("s1_pc", wire.s1_pc);
    TracePerCycle("s2_valid", wire.s2_valid);
    TracePerCycle("s2_pc", wire.s2_pc);
    TracePerCycle("s3_valid", wire.s3_valid);
    TracePerCycle("s3_pc", wire.s3_pc);
    TracePerCycle("fwd1_valid", fwd1.valid.Get());
    TracePerCycle("fwd1_rd", fwd1.rd.Get());
    TracePerCycle("fwd1_ready", fwd1.ready.Get());
    TracePerCycle("fwd1_data", fwd1.data.Get());
    TracePerCycle("fwd2_valid", fwd2.valid.Get());
    TracePerCycle("fwd2_rd", fwd2.rd.Get());
    TracePerCycle("fwd2_ready", fwd2.ready.Get());
    TracePerCycle("fwd2_data", fwd2.data.Get());
    TracePerCycle("fwd3_valid", fwd3.valid.Get());
    TracePerCycle("fwd3_rd", fwd3.rd.Get());
    TracePerCycle("fwd3_ready", fwd3.ready.Get());
    TracePerCycle("fwd3_data", fwd3.data.Get());
    TracePerCycle("mem_wb_valid", mem_wb->ValidCount());
  }

  Rv32Config cfg;
  FifoPtr<Rv32ExMemPkt> ex_mem;
  FifoPtr<Rv32MemWbPkt> mem_wb;
  Rv32MemPort ilm, dlm, mmio;

  // Cycle 独占
  Slot s1, s2;
  Rv32Halt fault = Rv32Halt::kRunning;
  uint32_t fault_addr = 0;
  Wire wire;

  Rv32FwdBus fwd1, fwd2, fwd3;
  Logic64 halt_req;
};

// ================================================================= WB 级

// 写回：把 MEM/WB 里的结果送回寄存器堆写口，同时统计退休指令数，
// 遇到 ebreak / ecall / 非法指令就在这里停机（精确异常点）。
class Rv32Writeback : public ClkModule {
 public:
  Rv32Writeback(ClockPtr clock, const std::string& name, uint64_t parent,
                FifoPtr<Rv32MemWbPkt> in, FifoPtr<Rv32RfWritePkt> out)
      : ClkModule(clock),
        mem_wb(in),
        wb_rf(out),
        fwd(clock),
        halt_req(clock),
        retired(clock) {
    RegisterId(name, parent);
  }

  // 停机请求：本级只把标志举起来，真正停时钟的是 MMIO 模块
  const Logic64& HaltReq() const { return halt_req; }

  const Rv32FwdBus& Fwd() const { return fwd; }

  uint64_t Instret() const { return retire_pending; }
  Rv32Halt Trap() const { return trap; }
  uint32_t TrapPc() const { return trap_pc; }
  uint32_t LastPc() const { return last_pc; }

  void Cycle() override {
    DelayCycle(1);

    Wire w;
    Rv32FwdBus& bus = fwd;

    if (!mem_wb->IsEmpty() && !wb_rf->IsFull()) {
      Rv32MemWbPkt& p = mem_wb->Front();
      const uint32_t pc = static_cast<uint32_t>(uint64_t(p.pc));
      const uint32_t inst = static_cast<uint32_t>(uint64_t(p.inst));
      const uint32_t rd = static_cast<uint32_t>(uint64_t(p.rd));
      const uint32_t data = static_cast<uint32_t>(uint64_t(p.wb_data));
      const uint32_t reg_write = static_cast<uint32_t>(uint64_t(p.reg_write));
      const uint32_t illegal = static_cast<uint32_t>(uint64_t(p.illegal));
      const uint32_t ebreak = static_cast<uint32_t>(uint64_t(p.ebreak));
      const uint32_t ecall = static_cast<uint32_t>(uint64_t(p.ecall));
      mem_wb->Pop();

      // 一旦有指令在这里陷入，比它年轻的都作废：退休是按序的，所以陷入那条
      // 之前的指令都已经写回，停下来时的机器状态是精确的。作废的这些照样
      // 从 MEM/WB 里取走，免得把前面几级堵住。
      if (trap == Rv32Halt::kRunning) {
        Rv32RfWritePkt q(clk);
        q.rd = reg_write ? rd : 0u;
        q.data = data;
        q.pc = pc;
        wb_rf->Push(q);

        ++retire_pending;
        last_pc = pc;
        w.valid = 1;
        w.pc = pc;
        w.inst = inst;
        w.rd = rd;
        w.data = data;
        w.reg_write = reg_write;

        bus.Drive(reg_write && rd != 0 ? 1u : 0u, rd, 1u, data);

        if (illegal) trap = Rv32Halt::kIllegal;
        else if (ebreak) trap = Rv32Halt::kEbreak;
        else if (ecall) trap = Rv32Halt::kEcall;
        if (trap != Rv32Halt::kRunning) {
          trap_pc = pc;
          w.trap = 1;
        }
      } else {
        w.killed = 1;
        bus.Clear();
      }
    } else {
      bus.Clear();
    }

    wire = w;
    halt_req = (trap != Rv32Halt::kRunning) ? 1u : 0u;
    retired = retire_pending;
    EmitTraces();
  }

 private:
  struct Wire {
    uint32_t valid = 0, pc = 0, inst = 0, rd = 0, data = 0;
    uint32_t reg_write = 0, trap = 0, killed = 0;
  };

  void EmitTraces() {
    if (TraceDisabled()) return;
    TracePerCycle("valid", wire.valid);
    TracePerCycle("pc", wire.pc);
    TracePerCycle("inst", wire.inst);
    TracePerCycle("rd", wire.rd);
    TracePerCycle("data", wire.data);
    TracePerCycle("reg_write", wire.reg_write);
    TracePerCycle("trap", wire.trap);
    TracePerCycle("killed", wire.killed);
    TracePerCycle("instret", retire_pending);
    TracePerCycle("fwd_valid", fwd.valid.Get());
    TracePerCycle("fwd_rd", fwd.rd.Get());
    TracePerCycle("fwd_data", fwd.data.Get());
  }

  FifoPtr<Rv32MemWbPkt> mem_wb;
  FifoPtr<Rv32RfWritePkt> wb_rf;

  // Cycle 独占
  uint64_t retire_pending = 0;
  uint32_t last_pc = 0;
  Rv32Halt trap = Rv32Halt::kRunning;
  uint32_t trap_pc = 0;
  Wire wire;

  Rv32FwdBus fwd;
  Logic64 halt_req, retired;
};

// ================================================================= EX 级

// 执行：前推选源、ALU、分支判定。分支成立就换一代 epoch 并把目标广播出去，
// IF 下一拍改取指方向，ID 与 EX 自己把还在管子里的旧代指令丢掉。
class Rv32Execute : public ClkModule {
 public:
  Rv32Execute(ClockPtr clock, const std::string& name, uint64_t parent,
              FifoPtr<Rv32IdExPkt> in, FifoPtr<Rv32ExMemPkt> out,
              const Rv32Mem* mem_stage, const Rv32Writeback* wb_stage)
      : ClkModule(clock),
        id_ex(in),
        ex_mem(out),
        mem(mem_stage),
        wb(wb_stage),
        fwd(clock),
        redir_valid(clock),
        redir_target(clock),
        redir_epoch(clock),
        branches(clock),
        taken(clock) {
    RegisterId(name, parent);
  }

  const Rv32FwdBus& Fwd() const { return fwd; }
  const Logic64& RedirValid() const { return redir_valid; }
  const Logic64& RedirTarget() const { return redir_target; }
  const Logic64& RedirEpoch() const { return redir_epoch; }

  uint64_t BranchCount() const { return branch_pending; }
  uint64_t TakenCount() const { return taken_pending; }

  void Cycle() override {
    DelayCycle(1);

    Wire w;
    uint32_t redirect_now = 0;
    uint32_t target_now = 0;

    // 本级上一拍算出的那条结果也是一路前推源。它跟本级这一拍要写的是同一组
    // Logic64，所以先把上拍的值抄下来再往下走，免得读成本拍刚写进去的。
    const FwdSnap self = {
        static_cast<uint32_t>(uint64_t(fwd.valid)),
        static_cast<uint32_t>(uint64_t(fwd.rd)),
        static_cast<uint32_t>(uint64_t(fwd.ready)),
        static_cast<uint32_t>(uint64_t(fwd.data))};

    if (!id_ex->IsEmpty() && !ex_mem->IsFull()) {
      Rv32IdExPkt& p = id_ex->Front();
      const uint32_t pkt_epoch = static_cast<uint32_t>(uint64_t(p.epoch));

      if (pkt_epoch != epoch) {
        id_ex->Pop();
        w.flush = 1;
        fwd.Clear();
      } else {
        const uint32_t pc = static_cast<uint32_t>(uint64_t(p.pc));
        const uint32_t inst = static_cast<uint32_t>(uint64_t(p.inst));
        const uint32_t rs1 = static_cast<uint32_t>(uint64_t(p.rs1));
        const uint32_t rs2 = static_cast<uint32_t>(uint64_t(p.rs2));
        const uint32_t rd = static_cast<uint32_t>(uint64_t(p.rd));
        const uint32_t imm = static_cast<uint32_t>(uint64_t(p.imm));
        const uint32_t funct3 = static_cast<uint32_t>(uint64_t(p.funct3));
        const uint32_t alu_op = static_cast<uint32_t>(uint64_t(p.alu_op));
        const uint32_t src_a = static_cast<uint32_t>(uint64_t(p.alu_src_a));
        const uint32_t src_b = static_cast<uint32_t>(uint64_t(p.alu_src_b));
        const uint32_t wb_sel = static_cast<uint32_t>(uint64_t(p.wb_sel));
        const uint32_t reg_write = static_cast<uint32_t>(uint64_t(p.reg_write));
        const uint32_t mem_read = static_cast<uint32_t>(uint64_t(p.mem_read));
        const uint32_t mem_write = static_cast<uint32_t>(uint64_t(p.mem_write));
        const uint32_t branch = static_cast<uint32_t>(uint64_t(p.branch));
        const uint32_t jump = static_cast<uint32_t>(uint64_t(p.jump));
        const uint32_t jalr = static_cast<uint32_t>(uint64_t(p.jalr));
        const uint32_t uses_rs1 = static_cast<uint32_t>(uint64_t(p.uses_rs1));
        const uint32_t uses_rs2 = static_cast<uint32_t>(uint64_t(p.uses_rs2));
        id_ex->Pop();

        // 前推：五路源按新旧排优先级，命中最近的那一路
        uint32_t sel_a = 0, sel_b = 0;
        const uint32_t rs1_val =
            Forward(self, rs1, uses_rs1,
                    static_cast<uint32_t>(uint64_t(p.rs1_data)), sel_a);
        const uint32_t rs2_val =
            Forward(self, rs2, uses_rs2,
                    static_cast<uint32_t>(uint64_t(p.rs2_data)), sel_b);

        uint32_t alu_a = rs1_val;
        if (src_a == rv32::kSrcAPc) alu_a = pc;
        else if (src_a == rv32::kSrcAZero) alu_a = 0;
        const uint32_t alu_b = (src_b == rv32::kSrcBImm) ? imm : rs2_val;
        const uint32_t alu_result = rv32::AluCompute(alu_op, alu_a, alu_b);

        const uint32_t cmp =
            rv32::BranchTaken(funct3, rs1_val, rs2_val) ? 1u : 0u;
        const uint32_t take = ((branch && cmp) || jump) ? 1u : 0u;
        const uint32_t target =
            jalr ? ((rs1_val + imm) & ~1u) : (pc + imm);

        if (branch) {
          ++branch_pending;
          if (cmp) ++taken_pending;
        }
        if (take) {
          ++epoch;
          redirect_now = 1;
          target_now = target;
        }

        Rv32ExMemPkt q(clk);
        q.pc = pc;
        q.inst = inst;
        q.rd = rd;
        q.alu_result = alu_result;
        q.store_data = rs2_val;
        q.pc_plus4 = pc + 4u;
        q.funct3 = funct3;
        q.wb_sel = wb_sel;
        q.reg_write = reg_write;
        q.mem_read = mem_read;
        q.mem_write = mem_write;
        q.illegal = uint64_t(p.illegal);
        q.ebreak = uint64_t(p.ebreak);
        q.ecall = uint64_t(p.ecall);
        ex_mem->Push(q);

        // 本级算出来的结果下一拍就能前推给紧跟着的指令；
        // load 这时只有地址、还没有数据，所以 ready 为 0
        fwd.Drive(reg_write && rd != 0 ? 1u : 0u, rd,
                  (wb_sel == rv32::kWbMem) ? 0u : 1u,
                  (wb_sel == rv32::kWbPc4) ? (pc + 4u) : alu_result);

        w.valid = 1;
        w.pc = pc;
        w.inst = inst;
        w.rd = rd;
        w.rs1 = rs1;
        w.rs2 = rs2;
        w.imm = imm;
        w.alu_a = alu_a;
        w.alu_b = alu_b;
        w.alu_op = alu_op;
        w.alu_result = alu_result;
        w.rs1_fwd = rs1_val;
        w.rs2_fwd = rs2_val;
        w.fwd_a = sel_a;
        w.fwd_b = sel_b;
        w.branch = branch;
        w.jump = jump;
        w.cmp = cmp;
        w.take = take;
        w.target = target;
        w.store_data = rs2_val;
      }
    } else {
      fwd.Clear();
      w.idle = 1;
    }

    w.epoch = epoch;
    w.redirect = redirect_now;
    wire = w;

    redir_valid = redirect_now;
    if (redirect_now) redir_last_target = target_now;
    redir_target = redir_last_target;
    redir_epoch = epoch;
    branches = branch_pending;
    taken = taken_pending;
    EmitTraces();
  }

 private:
  struct Wire {
    uint32_t valid = 0, pc = 0, inst = 0, rd = 0, rs1 = 0, rs2 = 0, imm = 0;
    uint32_t alu_a = 0, alu_b = 0, alu_op = 0, alu_result = 0;
    uint32_t rs1_fwd = 0, rs2_fwd = 0, fwd_a = 0, fwd_b = 0;
    uint32_t branch = 0, jump = 0, cmp = 0, take = 0, target = 0;
    uint32_t store_data = 0, epoch = 0, redirect = 0, flush = 0, idle = 0;
  };
  // 前推总线取下来的一拍快照
  struct FwdSnap {
    uint32_t valid = 0, rd = 0, ready = 0, data = 0;
  };

  // 前推源编号：1=EX 上拍 2=MEM 第一拍 3=MEM 第二拍 4=MEM 第三拍 5=WB 上拍，
  // 0 表示没人命中、直接用 ID 从寄存器堆读出来的值。再早的指令已经写进
  // 寄存器堆，ID 读得到，所以五路就够。
  uint32_t Forward(const FwdSnap& self, uint32_t rs, uint32_t uses,
                   uint32_t rf_value, uint32_t& sel) {
    sel = 0;
    if (rs == 0 || !uses) return rf_value;

    const Rv32FwdBus* bus[4] = {&mem->Fwd1(), &mem->Fwd2(), &mem->Fwd3(),
                                &wb->Fwd()};
    FwdSnap src[5];
    src[0] = self;
    for (uint32_t i = 0; i < 4; ++i) {
      src[i + 1] = {static_cast<uint32_t>(uint64_t(bus[i]->valid)),
                    static_cast<uint32_t>(uint64_t(bus[i]->rd)),
                    static_cast<uint32_t>(uint64_t(bus[i]->ready)),
                    static_cast<uint32_t>(uint64_t(bus[i]->data))};
    }

    for (uint32_t i = 0; i < 5; ++i) {
      if (!src[i].valid || src[i].rd != rs) continue;
      LOGCHECK(src[i].ready != 0,
               "Rv32Execute: forwarding from a load whose data is not back — "
               "the ID-stage interlock let a load-use hazard through");
      sel = i + 1;
      return src[i].data;
    }
    return rf_value;
  }

  void EmitTraces() {
    if (TraceDisabled()) return;
    TracePerCycle("valid", wire.valid);
    TracePerCycle("pc", wire.pc);
    TracePerCycle("inst", wire.inst);
    TracePerCycle("rd", wire.rd);
    TracePerCycle("rs1", wire.rs1);
    TracePerCycle("rs2", wire.rs2);
    TracePerCycle("imm", wire.imm);
    TracePerCycle("alu_a", wire.alu_a);
    TracePerCycle("alu_b", wire.alu_b);
    TracePerCycle("alu_op", wire.alu_op);
    TracePerCycle("alu_result", wire.alu_result);
    TracePerCycle("rs1_fwd", wire.rs1_fwd);
    TracePerCycle("rs2_fwd", wire.rs2_fwd);
    TracePerCycle("fwd_a", wire.fwd_a);
    TracePerCycle("fwd_b", wire.fwd_b);
    TracePerCycle("store_data", wire.store_data);
    TracePerCycle("branch", wire.branch);
    TracePerCycle("jump", wire.jump);
    TracePerCycle("branch_cmp", wire.cmp);
    TracePerCycle("branch_taken", wire.take);
    TracePerCycle("branch_target", wire.target);
    TracePerCycle("epoch", wire.epoch);
    TracePerCycle("redirect", wire.redirect);
    TracePerCycle("flush", wire.flush);
    TracePerCycle("idle", wire.idle);
    TracePerCycle("branch_count", branch_pending);
    TracePerCycle("taken_count", taken_pending);
    TracePerCycle("fwd_valid", fwd.valid.Get());
    TracePerCycle("fwd_rd", fwd.rd.Get());
    TracePerCycle("fwd_ready", fwd.ready.Get());
    TracePerCycle("fwd_data", fwd.data.Get());
    TracePerCycle("ex_mem_valid", ex_mem->ValidCount());
  }

  FifoPtr<Rv32IdExPkt> id_ex;
  FifoPtr<Rv32ExMemPkt> ex_mem;
  const Rv32Mem* mem;
  const Rv32Writeback* wb;

  // Cycle 独占
  uint32_t epoch = 0;
  uint32_t redir_last_target = 0;
  uint64_t branch_pending = 0, taken_pending = 0;
  Wire wire;

  Rv32FwdBus fwd;
  Logic64 redir_valid, redir_target, redir_epoch;
  Logic64 branches, taken;
};

// ================================================================ 顶层组装

// 把八个模块接成一颗核：五个执行级加两块本地存储器加一个 MMIO 窗口。
// 自己不是 ClkModule，只负责建 Fifo、连线、汇总统计。
class Rv32Core : public Object {
 public:
  Rv32Core(ClockPtr clock, const std::string& name,
           const Rv32Config& config = Rv32Config())
      : clk(clock), cfg(config) {
    RegisterName(name);

    if_id = std::make_shared<Fifo<Rv32IfIdPkt>>(kRv32FifoDepth, clk);
    id_ex = std::make_shared<Fifo<Rv32IdExPkt>>(kRv32FifoDepth, clk);
    ex_mem = std::make_shared<Fifo<Rv32ExMemPkt>>(kRv32FifoDepth, clk);
    mem_wb = std::make_shared<Fifo<Rv32MemWbPkt>>(kRv32FifoDepth, clk);
    wb_rf = std::make_shared<Fifo<Rv32RfWritePkt>>(kRv32FifoDepth, clk);

    ilm = std::make_unique<Rv32Lim>(clk, "ilm", obj_id, cfg.ilm_base,
                                    cfg.ilm_size,
                                    std::vector<std::string>{"i", "d"},
                                    kRv32FifoDepth);
    dlm = std::make_unique<Rv32Lim>(clk, "dlm", obj_id, cfg.dlm_base,
                                    cfg.dlm_size,
                                    std::vector<std::string>{"d"},
                                    kRv32FifoDepth);
    mmio = std::make_unique<Rv32Mmio>(clk, "mmio", obj_id, cfg.mmio_base,
                                      cfg.mmio_size, kRv32FifoDepth,
                                      cfg.max_cycles);

    // 依赖顺序：EX 要读 MEM 与 WB 的前推总线，IF 与 ID 要读 EX 的重定向
    mem = std::make_unique<Rv32Mem>(clk, "mem", obj_id, cfg, ex_mem, mem_wb,
                                    ilm->Port(1), dlm->Port(0), mmio->Port());
    wb = std::make_unique<Rv32Writeback>(clk, "wb", obj_id, mem_wb, wb_rf);
    ex = std::make_unique<Rv32Execute>(clk, "ex", obj_id, id_ex, ex_mem,
                                       mem.get(), wb.get());
    id = std::make_unique<Rv32Decode>(clk, "id", obj_id, cfg, if_id, id_ex,
                                      wb_rf);
    fetch = std::make_unique<Rv32Fetch>(clk, "if", obj_id, cfg, ilm->Port(0),
                                        if_id);

    fetch->BindRedirect(&ex->RedirValid(), &ex->RedirTarget(),
                        &ex->RedirEpoch());
    id->BindRedirect(&ex->RedirEpoch());

    // 停机集中在 MMIO 上：各级只举 halt_req，由它排空之后停时钟
    mmio->BindHalt({&wb->HaltReq(), &mem->HaltReq(), &fetch->HaltReq()});
  }

  // ---- 子模块

  Rv32Lim& Ilm() { return *ilm; }
  Rv32Lim& Dlm() { return *dlm; }
  Rv32Mmio& Mmio() { return *mmio; }
  Rv32Fetch& If() { return *fetch; }
  Rv32Decode& Id() { return *id; }
  Rv32Execute& Ex() { return *ex; }
  Rv32Mem& Mem() { return *mem; }
  Rv32Writeback& Wb() { return *wb; }
  const Rv32Config& Cfg() const { return cfg; }

  // ---- 装载

  bool LoadIlmBin(const std::string& path) {
    return ilm->LoadBinFile(cfg.ilm_base, path);
  }
  void LoadIlmImage(const uint8_t* data, size_t len) {
    ilm->LoadImage(cfg.ilm_base, data, len);
  }

  // ---- 仿真结束后的查询

  uint32_t Reg(uint32_t i) const { return id->Reg(i); }

  Rv32Halt HaltReason() const {
    if (mmio->ExitRequested()) return Rv32Halt::kSimExit;
    if (wb->Trap() != Rv32Halt::kRunning) return wb->Trap();
    if (mem->Fault() != Rv32Halt::kRunning) return mem->Fault();
    if (fetch->FetchFault()) return Rv32Halt::kFetchFault;
    if (mmio->CycleLimitHit()) return Rv32Halt::kCycleLimit;
    return Rv32Halt::kRunning;
  }
  bool Halted() const { return HaltReason() != Rv32Halt::kRunning; }
  uint32_t ExitCode() const { return mmio->ExitCode(); }
  const std::string& UartOut() const { return mmio->UartOut(); }

  uint64_t Cycles() const { return mmio->Cycles(); }
  uint64_t Instret() const { return wb->Instret(); }
  uint64_t StallCycles() const { return id->StallCount(); }
  uint64_t Branches() const { return ex->BranchCount(); }
  uint64_t BranchesTaken() const { return ex->TakenCount(); }
  uint64_t CpiMilli() const {
    return Instret() ? (Cycles() * 1000ull) / Instret() : 0;
  }

  std::string StatsText() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "cycles=%llu instret=%llu cpi=%llu.%03llu stall=%llu "
                  "branches=%llu taken=%llu halt=%u exit=%u",
                  static_cast<unsigned long long>(Cycles()),
                  static_cast<unsigned long long>(Instret()),
                  static_cast<unsigned long long>(CpiMilli() / 1000),
                  static_cast<unsigned long long>(CpiMilli() % 1000),
                  static_cast<unsigned long long>(StallCycles()),
                  static_cast<unsigned long long>(Branches()),
                  static_cast<unsigned long long>(BranchesTaken()),
                  static_cast<unsigned>(HaltReason()),
                  static_cast<unsigned>(ExitCode()));
    return buf;
  }

 private:
  ClockPtr clk;
  Rv32Config cfg;

  FifoPtr<Rv32IfIdPkt> if_id;
  FifoPtr<Rv32IdExPkt> id_ex;
  FifoPtr<Rv32ExMemPkt> ex_mem;
  FifoPtr<Rv32MemWbPkt> mem_wb;
  FifoPtr<Rv32RfWritePkt> wb_rf;

  std::unique_ptr<Rv32Lim> ilm;
  std::unique_ptr<Rv32Lim> dlm;
  std::unique_ptr<Rv32Mmio> mmio;
  std::unique_ptr<Rv32Mem> mem;
  std::unique_ptr<Rv32Writeback> wb;
  std::unique_ptr<Rv32Execute> ex;
  std::unique_ptr<Rv32Decode> id;
  std::unique_ptr<Rv32Fetch> fetch;
};

}  // namespace latch

#endif
