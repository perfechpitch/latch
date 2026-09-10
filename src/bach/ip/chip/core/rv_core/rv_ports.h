#ifndef _LATCH_BACH_IP_CHIP_CORE_RV_CORE_RV_PORTS_
#define _LATCH_BACH_IP_CHIP_CORE_RV_CORE_RV_PORTS_

// RV core 看得见的地址空间。
//
// 三个实例各自可见：自己的 ITCM、DTCM、Share Mem、Core Mem（只有 DTE core 有）
// 与对应 DSA 的 IO reg。Matrix Mem 对三个 RV core 都不可见。
//
// 这一份与 compiler/kernel/bach.h 同源：kernel 里的 DSA 配置写的就是这些地址，
// 改一处要一起改。

#include <cstdint>
#include <memory>

#include "base/logic.h"

namespace latch {
namespace bach {

// ITCM 4 KB，存 firmware 与 kernel。DTE core 的 weights loader 是 kernel 里的
// 一段，不另占空间。
constexpr uint64_t kItcmBase = 0x00000000;
constexpr uint64_t kItcmSize = 4 * 1024;

// 三个 DSA 的 IO reg。跨度按各 DSA 文档里的最大偏移留：VU 的 Profile Block 到
// 0x40EC，所以它要 20 KB。
constexpr uint64_t kDteIoBase = 0x00008000;
constexpr uint64_t kMuIoBase = 0x00009000;
constexpr uint64_t kVuIoBase = 0x0000C000;
constexpr uint64_t kDsaIoSize = 4 * 1024;
constexpr uint64_t kVuIoSize = 20 * 1024;

// DTCM 8 KB，存 BSS、寄存器溢出与堆栈。排在 VU IO 之后。
constexpr uint64_t kDtcmBase = 0x00020000;
constexpr uint64_t kDtcmSize = 8 * 1024;

// task 控制区：自定义 CSR 的读口与 task_done 的写口。
//
// 硬件上这几样是 custom-0 自定义指令（task_done / dsar / dsaw 那一组），模型里
// 走 MMIO：rv32 的功能模型认的是标准 RV32IM，加一条自定义指令要动 gen/ 里
// codegen 出来的解码表。走 MMIO 的行为等价 —— 都是「往一个约定地址写一笔就
// 触发」，而且 kernel 那边本来就用 store 配 DSA 寄存器。
constexpr uint64_t kTaskCtrlBase = 0x00030000;
constexpr uint64_t kTaskCtrlSize = 4 * 1024;
// task 控制区内的偏移。
constexpr uint64_t kCsrStreamId = 0x00;
constexpr uint64_t kCsrTaskId = 0x04;
constexpr uint64_t kCsrUserId = 0x08;        // 可读写，B / R core 的软件写它
constexpr uint64_t kRegTaskDone = 0x10;      // 写 1 = 通知 TS，写 0 = 不通知
constexpr uint64_t kRegWaitTask = 0x14;      // 读它阻塞到下一个 task 到来

constexpr uint64_t kShareMemBase = 0x00040000;
constexpr uint64_t kShareMemSize = 32 * 1024;

// 只有 DTE core 有。Router I/O reg 复用这一段的高地址。
constexpr uint64_t kCoreMemBase = 0x00080000;
constexpr uint64_t kCoreMemSize = 1024 * 1024 + 32 * 1024;

// 访存延迟，记在时间轴上。
constexpr uint64_t kDtcmLatency = 3;
constexpr uint64_t kShareMemLatency = 8;    // 5～10 拍，取中间值
constexpr uint64_t kCoreMemLatency = 20;    // 15～25 拍，取中间值
constexpr uint64_t kDsaReadLatency = 4;

// task_queue 深 2：提前接一个，前一个做完立刻起下一个，用户之间无 bubble。
constexpr uint64_t kRvTaskQueueDepth = 2;
// dsa_rq 8 项，按顺序记录已下发的读指令。
constexpr uint64_t kDsaRqDepth = 8;
// sm_lsq 与 cm_lsq 各 16 项，顺序发射，每拍一个。
constexpr uint64_t kSmLsqDepth = 16;
constexpr uint64_t kCmLsqDepth = 16;
// DTCM 4 bank，同 bank 冲突阻塞第二条。
constexpr uint64_t kDtcmBanks = 4;

// 访存去哪。按地址范围分流。
enum class LsqTarget : uint32_t {
  kDtcm = 0,
  kShareMem = 1,
  kCoreMem = 2,
  kRouterIo = 3,   // 复用 cm_lsq，只有 DTE core 有
};

// ── task_queue → 指令执行器 ──
//
// 起一个 task：给出起始 PC 与这一笔的身份。身份进自定义 CSR 供软件读，同时经
// dsa_ids 直连到本核那个 DSA。
class TaskStartPort : public Logic {
 public:
  Logic64 valid, ready, task_pc, stream_id, task_id, user_id, path_id, dsa_en,
      seq;

  explicit TaskStartPort(ClockPtr c)
      : valid(c), ready(c), task_pc(c), stream_id(c), task_id(c), user_id(c),
        path_id(c), dsa_en(c), seq(c) {
    Fields(valid, ready, task_pc, stream_id, task_id, user_id, path_id, dsa_en,
           seq);
  }

  void Drive(uint64_t pc, uint64_t stream, uint64_t task, uint64_t user,
             uint64_t path, bool en, uint64_t n) {
    valid = 1;
    task_pc = pc;
    stream_id = stream;
    task_id = task;
    user_id = user;
    path_id = path;
    dsa_en = en ? 1 : 0;
    seq = n;
  }
  void Idle() {
    valid = 0;
    task_pc = 0;
    stream_id = 0;
    task_id = 0;
    user_id = 0;
    path_id = 0;
    dsa_en = 0;
    seq = seq.Get();
  }
  void DriveReady(bool ok) { ready = ok ? 1 : 0; }
  bool Valid() const { return valid.Get() != 0; }
  bool Ready() const { return ready.Get() != 0; }
  uint64_t Seq() const { return seq.Get(); }
};

// ── 指令执行器 → 指令执行器自己（task_done）──
//
// task_done 指令：交还自己，队列非空就跳到队头 task 的起始 PC。带 TS 标志时
// 另外通知 TS。
class TaskDonePort : public Logic {
 public:
  Logic64 valid, notify_ts, seq;

  explicit TaskDonePort(ClockPtr c) : valid(c), notify_ts(c), seq(c) {
    Fields(valid, notify_ts, seq);
  }

  void Drive(bool to_ts, uint64_t n) {
    valid = 1;
    notify_ts = to_ts ? 1 : 0;
    seq = n;
  }
  void Idle() {
    valid = 0;
    notify_ts = 0;
    seq = seq.Get();
  }
  bool Valid() const { return valid.Get() != 0; }
  bool NotifyTs() const { return notify_ts.Get() != 0; }
  uint64_t Seq() const { return seq.Get(); }
};

// ── 指令执行器 → dsa_iss ──
//
// 一条 DSA 寄存器读写。写会被下发通道反压，读不会。
class DsaReqPort : public Logic {
 public:
  Logic64 valid, ready, we, addr, wdata, rd_idx, seq;

  explicit DsaReqPort(ClockPtr c)
      : valid(c), ready(c), we(c), addr(c), wdata(c), rd_idx(c), seq(c) {
    Fields(valid, ready, we, addr, wdata, rd_idx, seq);
  }

  void Drive(bool is_write, uint64_t at, uint64_t data, uint64_t rd,
             uint64_t n) {
    valid = 1;
    we = is_write ? 1 : 0;
    addr = at;
    wdata = data;
    rd_idx = rd;
    seq = n;
  }
  void Idle() {
    valid = 0;
    we = 0;
    addr = 0;
    wdata = 0;
    rd_idx = 0;
    seq = seq.Get();
  }
  void DriveReady(bool ok) { ready = ok ? 1 : 0; }
  bool Valid() const { return valid.Get() != 0; }
  bool Ready() const { return ready.Get() != 0; }
  bool We() const { return we.Get() != 0; }
  uint64_t Seq() const { return seq.Get(); }
};

// ── 指令执行器 → lsq ──
//
// 一笔访存。target 由地址范围决定，rd_idx 是要写回的通用寄存器编号。
class LsqReqPort : public Logic {
 public:
  Logic64 valid, ready, we, addr, wdata, be, rd_idx, target, seq;

  explicit LsqReqPort(ClockPtr c)
      : valid(c), ready(c), we(c), addr(c), wdata(c), be(c), rd_idx(c),
        target(c), seq(c) {
    Fields(valid, ready, we, addr, wdata, be, rd_idx, target, seq);
  }

  void Drive(LsqTarget to, bool is_write, uint64_t at, uint64_t data,
             uint64_t mask, uint64_t rd, uint64_t n) {
    valid = 1;
    target = uint64_t(to);
    we = is_write ? 1 : 0;
    addr = at;
    wdata = data;
    be = mask;
    rd_idx = rd;
    seq = n;
  }
  void Idle() {
    valid = 0;
    target = 0;
    we = 0;
    addr = 0;
    wdata = 0;
    be = 0;
    rd_idx = 0;
    seq = seq.Get();
  }
  void DriveReady(bool ok) { ready = ok ? 1 : 0; }
  bool Valid() const { return valid.Get() != 0; }
  bool Ready() const { return ready.Get() != 0; }
  bool We() const { return we.Get() != 0; }
  LsqTarget Target() const { return LsqTarget(target.Get()); }
  uint64_t Seq() const { return seq.Get(); }
};

// ── 指令执行器 → DSA：四个身份信号 ──
//
// 《DTE寄存器配置参数》§硬件直连信号：streamID、taskID、userID、pathID 从
// RV core 的 CSR 直连到 DSA，DSA 在写 trigger 寄存器那一拍采样。所以这一束不
// 握手、每拍驱动，取的是本核当前那个 task 的身份。
class DsaIdsPort : public Logic {
 public:
  Logic64 stream_id, task_id, user_id, path_id;

  explicit DsaIdsPort(ClockPtr c)
      : stream_id(c), task_id(c), user_id(c), path_id(c) {
    Fields(stream_id, task_id, user_id, path_id);
  }

  void Drive(uint64_t stream, uint64_t task, uint64_t user, uint64_t path) {
    stream_id = stream;
    task_id = task;
    user_id = user;
    path_id = path;
  }
  uint64_t Stream() const { return stream_id.Get(); }
  uint64_t Task() const { return task_id.Get(); }
  uint64_t User() const { return user_id.Get(); }
  uint64_t Path() const { return path_id.Get(); }
};

// ── dsa_rq 与 lsq → 指令执行器 ──
//
// 写回一个通用寄存器并把它标成就绪。
//
// 访存那一路只管时序：数据在功能模型执行那一条指令时就已经落进 gpr 了，这一笔
// 决定的是就绪位什么时候立起来，也就是后面那条读它的指令什么时候能走。
//
// 读 DSA 寄存器那一路还要带回数据：DSA 的读不支持同步返回，功能模型执行 lw 那
// 一拍拿到的是个占位值，真正的内容隔几拍才从 dsa_rdata 回来。
class GprWbPort : public Logic {
 public:
  Logic64 valid, rd_idx, seq, has_data, data;

  explicit GprWbPort(ClockPtr c)
      : valid(c), rd_idx(c), seq(c), has_data(c), data(c) {
    Fields(valid, rd_idx, seq, has_data, data);
  }

  void Drive(uint64_t rd, uint64_t n) {
    valid = 1;
    rd_idx = rd;
    seq = n;
    has_data = 0;
    data = 0;
  }
  void DriveData(uint64_t rd, uint64_t v, uint64_t n) {
    valid = 1;
    rd_idx = rd;
    seq = n;
    has_data = 1;
    data = v;
  }
  void Idle() {
    valid = 0;
    rd_idx = 0;
    seq = seq.Get();
    has_data = 0;
    data = 0;
  }
  bool Valid() const { return valid.Get() != 0; }
  uint64_t RdIdx() const { return rd_idx.Get(); }
  uint64_t Seq() const { return seq.Get(); }
  bool HasData() const { return has_data.Get() != 0; }
  uint64_t Data() const { return data.Get(); }
};

}  // namespace bach
}  // namespace latch

#endif
