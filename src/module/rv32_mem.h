#ifndef _LATCH_MODULE_RV32_MEM_
#define _LATCH_MODULE_RV32_MEM_

// rv32 五级流水线用的存储器侧模块：本地存储器 Rv32Lim 与 MMIO 外设 Rv32Mmio
//
// 两者都是独立的 ClkModule，对外只有一组 Fifo 端口：请求进 req、响应出 resp。
// 按 base/fifo.h 的时序，请求在第 T 拍 Push、存储器在第 T+1 拍取到并回响应、
// 发起方在第 T+2 拍读到，也就是一次访问两拍往返，与同步 SRAM 加一级握手寄存器
// 的行为一致。端口是全流水的：每拍都能收一个新请求，响应按请求顺序回。

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/fifo.h"
#include "base/log.h"
#include "base/logic.h"
#include "base/module.h"
#include "module/rv32_isa.h"

namespace latch {

// ------------------------------------------------------------ 端口上的包

// 一次存储器访问请求
class Rv32MemReqPkt : public Logic {
 public:
  Logic64 addr, wdata, be, is_write;
  explicit Rv32MemReqPkt(ClockPtr c)
      : addr(c), wdata(c), be(c), is_write(c) {
    Fields(addr, wdata, be, is_write);
  }
};

// 一次存储器访问的响应，rdata 是地址所在那个 32 位字的原始内容。
// 端口按请求顺序回应，发起方按同样的顺序取走即可。
class Rv32MemRespPkt : public Logic {
 public:
  Logic64 rdata;
  explicit Rv32MemRespPkt(ClockPtr c) : rdata(c) { Fields(rdata); }
};

// 一组请求与响应 Fifo 就是一个存储器端口，模块之间按引用共享
struct Rv32MemPort {
  FifoPtr<Rv32MemReqPkt> req;
  FifoPtr<Rv32MemRespPkt> resp;

  Rv32MemPort() = default;
  Rv32MemPort(ClockPtr c, uint64_t depth)
      : req(std::make_shared<Fifo<Rv32MemReqPkt>>(depth, c)),
        resp(std::make_shared<Fifo<Rv32MemRespPkt>>(depth, c)) {}

  bool Valid() const { return req != nullptr; }
};

// ------------------------------------------------------------ 本地存储器

// 一块本地存储器（ILM 或 DLM）。内容按 32 位字组织，字内写由 byte enable 选中。
// 端口数可以多于一个：ILM 就是双端口的，取指口给 IF 级、数据口给 MEM 级，
// 因为只读数据（字符串常量、虚表、.data 的加载副本）都放在 ILM 上。
class Rv32Lim : public ClkModule {
 public:
  Rv32Lim(ClockPtr clock, const std::string& name, uint64_t parent,
          uint32_t base_addr, uint32_t byte_size,
          const std::vector<std::string>& port_names, uint64_t depth = 4)
      : ClkModule(clock),
        base(base_addr),
        size(byte_size),
        word(byte_size / 4, 0),
        served(clock),
        writes(clock) {
    LOGCHECK(byte_size % 4 == 0, "Rv32Lim: size must be a multiple of 4");
    RegisterId(name, parent);
    for (const auto& pn : port_names) {
      port.emplace_back(clock, depth);
      pname.push_back(pn);
      sig.push_back(PortSig(pn));
    }
  }

  // ---- 连接

  uint32_t Ports() const { return static_cast<uint32_t>(port.size()); }
  Rv32MemPort& Port(uint32_t i) { return port[i]; }

  // ---- 地址

  uint32_t Base() const { return base; }
  uint32_t Size() const { return size; }
  bool Contains(uint32_t addr) const {
    return addr >= base && (addr - base) < size;
  }

  // ---- 装载与直读，仅限复位前或仿真结束后由主线程调用

  uint32_t ReadWord(uint32_t addr) const {
    LOGCHECK(Contains(addr), "Rv32Lim: read out of range");
    return word[(addr - base) >> 2];
  }

  void WriteWord(uint32_t addr, uint32_t data, uint8_t be) {
    LOGCHECK(Contains(addr), "Rv32Lim: write out of range");
    uint32_t& dst = word[(addr - base) >> 2];
    uint32_t mask = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (be & (1u << i)) mask |= 0xFFu << (i * 8);
    }
    dst = (dst & ~mask) | (data & mask);
  }

  void LoadImage(uint32_t addr, const uint8_t* data, size_t len) {
    LOGCHECK(Contains(addr) && (addr - base) + len <= size,
             "Rv32Lim: image does not fit");
    for (size_t i = 0; i < len; ++i) {
      uint32_t a = addr + static_cast<uint32_t>(i);
      uint32_t& dst = word[(a - base) >> 2];
      uint32_t shift = (a & 3u) * 8u;
      dst = (dst & ~(0xFFu << shift)) | (static_cast<uint32_t>(data[i]) << shift);
    }
  }

  bool LoadBinFile(uint32_t addr, const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) return false;
    std::fseek(fp, 0, SEEK_END);
    long n = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> buf(n > 0 ? static_cast<size_t>(n) : 0);
    size_t got = buf.empty() ? 0 : std::fread(buf.data(), 1, buf.size(), fp);
    std::fclose(fp);
    if (got != buf.size()) return false;
    LoadImage(addr, buf.data(), buf.size());
    return true;
  }

  // ---- 统计，仿真结束后主线程读

  uint64_t ServedCount() const { return served_pending; }
  uint64_t WriteCount() const { return writes_pending; }

  // ---- 一拍

  void Cycle() override {
    DelayCycle(1);

    for (size_t p = 0; p < port.size(); ++p) {
      PortWire w;
      if (!port[p].req->IsEmpty() && !port[p].resp->IsFull()) {
        Rv32MemReqPkt& q = port[p].req->Front();
        w.en = 1;
        w.addr = static_cast<uint32_t>(uint64_t(q.addr));
        w.we = static_cast<uint32_t>(uint64_t(q.is_write));
        w.be = static_cast<uint32_t>(uint64_t(q.be));
        w.wdata = static_cast<uint32_t>(uint64_t(q.wdata));
        port[p].req->Pop();

        LOGCHECK(Contains(w.addr), "Rv32Lim: request address out of range");
        if (w.we) {
          WriteWord(w.addr, w.wdata, static_cast<uint8_t>(w.be));
          ++writes_pending;
        }
        w.rdata = ReadWord(w.addr);

        Rv32MemRespPkt r(clk);
        r.rdata = w.rdata;
        port[p].resp->Push(r);
        ++served_pending;
      }
      wire[p] = w;
    }

    served = served_pending;
    writes = writes_pending;
    EmitTraces();
  }

 private:
  struct PortWire {
    uint32_t en = 0, we = 0, addr = 0, be = 0, wdata = 0, rdata = 0;
  };
  struct PortSig {
    std::string en, we, addr, be, wdata, rdata, req_cnt, resp_cnt;
    explicit PortSig(const std::string& p)
        : en(p + "_en"),
          we(p + "_we"),
          addr(p + "_addr"),
          be(p + "_be"),
          wdata(p + "_wdata"),
          rdata(p + "_rdata"),
          req_cnt(p + "_req_valid"),
          resp_cnt(p + "_resp_valid") {}
  };

  void EmitTraces() {
    if (TraceDisabled()) return;
    for (size_t p = 0; p < port.size(); ++p) {
      const PortWire& w = wire[p];
      TracePerCycle(sig[p].en, w.en);
      TracePerCycle(sig[p].we, w.we);
      TracePerCycle(sig[p].addr, w.addr);
      TracePerCycle(sig[p].be, w.be);
      TracePerCycle(sig[p].wdata, w.wdata);
      TracePerCycle(sig[p].rdata, w.rdata);
      TracePerCycle(sig[p].req_cnt, port[p].req->ValidCount());
      TracePerCycle(sig[p].resp_cnt, port[p].resp->ValidCount());
    }
    TracePerCycle("served", served_pending);
    TracePerCycle("writes", writes_pending);
  }

  uint32_t base;
  uint32_t size;
  std::vector<uint32_t> word;      // Cycle 独占，仿真前后由主线程装载与查看
  std::vector<Rv32MemPort> port;
  std::vector<std::string> pname;
  std::vector<PortSig> sig;
  PortWire wire[4];

  Logic64 served, writes;          // 协程内可读的计数
  uint64_t served_pending = 0;     // Cycle 独占累加器
  uint64_t writes_pending = 0;
};

// ---------------------------------------------------------------- MMIO

// 仿真用的外设窗口：一个只发不收的 UART、一个退出寄存器、一个周期计数器。
//
// 整个核的时钟只在这里停。程序往退出寄存器里写什么，什么就是退出码；流水线
// 各级遇到陷入或访存出错时不自己停时钟，只把 halt_req 举起来交给这里。
// 收到停机请求之后还要再放行若干拍，让管子里的指令走完、写回寄存器堆，
// 然后才停。停机那一拍谁先谁后是不确定的，留出这段排空时间，
// 停下来时的机器状态就与线程调度顺序无关。
class Rv32Mmio : public ClkModule {
 public:
  enum Reg : uint32_t {
    kUartTx = 0x0000u,   // 写：发送一个字节
    kUartLsr = 0x0005u,  // 读：bit0 = 发送器可接收
    kSimExit = 0x1000u,  // 写：停机，写入值即退出码
    kCycleLow = 0x1008u, // 读：当前周期计数低 32 位
  };

  // 排空拍数要盖住最长的一条在途路径：进 MEM 三拍、到 WB 一拍、
  // 再到 ID 写回寄存器堆一拍，另外留一点余量
  static constexpr uint64_t kDrainCycles = 16;

  Rv32Mmio(ClockPtr clock, const std::string& name, uint64_t parent,
           uint32_t base_addr, uint32_t byte_size, uint64_t depth = 4,
           uint64_t cycle_limit = 0)
      : ClkModule(clock),
        base(base_addr),
        size(byte_size),
        limit(cycle_limit),
        port(clock, depth),
        halted(clock),
        uart_bytes(clock) {
    RegisterId(name, parent);
  }

  Rv32MemPort& Port() { return port; }

  // 流水线各级的停机请求接到这里，每拍看一眼
  void BindHalt(std::vector<const Logic64*> reqs) { halt_req = std::move(reqs); }

  bool Contains(uint32_t addr) const {
    return addr >= base && (addr - base) < size;
  }
  uint32_t Base() const { return base; }

  // ---- 仿真结束后主线程读

  const std::string& UartOut() const { return uart_out; }
  uint64_t UartBytes() const { return uart_pending; }
  bool ExitRequested() const { return exit_seen; }
  uint32_t ExitCode() const { return exit_code; }
  uint64_t Cycles() const { return cycle_count; }
  bool CycleLimitHit() const { return limit_hit; }

  void Cycle() override {
    DelayCycle(1);
    if (limit != 0 && cycle_count >= limit) {
      limit_hit = true;
      clk->Stop();
      return;
    }
    ++cycle_count;

    Wire w;
    if (!port.req->IsEmpty() && !port.resp->IsFull()) {
      Rv32MemReqPkt& q = port.req->Front();
      w.en = 1;
      w.addr = static_cast<uint32_t>(uint64_t(q.addr));
      w.we = static_cast<uint32_t>(uint64_t(q.is_write));
      w.be = static_cast<uint32_t>(uint64_t(q.be));
      w.wdata = static_cast<uint32_t>(uint64_t(q.wdata));
      port.req->Pop();

      const uint32_t off = (w.addr & ~3u) - base;
      if (w.we) {
        w.rdata = 0;
        if (off == (kUartTx & ~3u)) {
          // 字节使能选中哪个字节，发的就是哪个字节
          for (uint32_t i = 0; i < 4; ++i) {
            if (w.be & (1u << i)) {
              uart_out.push_back(static_cast<char>(w.wdata >> (i * 8)));
              ++uart_pending;
              break;
            }
          }
          w.uart = 1;
        } else if (off == kSimExit) {
          exit_seen = true;
          exit_code = w.wdata;
          w.exit_hit = 1;
        }
      } else {
        if (off == (kUartLsr & ~3u)) {
          // bit0 恒为 1：这个模型里 UART 不会背压
          w.rdata = 0x1u << ((kUartLsr & 3u) * 8u);
        } else if (off == kCycleLow) {
          w.rdata = static_cast<uint32_t>(cycle_count);
        } else {
          w.rdata = 0;
        }
      }

      Rv32MemRespPkt r(clk);
      r.rdata = w.rdata;
      port.resp->Push(r);
    }
    wire = w;

    halted = exit_seen ? 1u : 0u;
    uart_bytes = uart_pending;

    // 停机：自己看到退出寄存器被写，或者哪一级举了 halt_req，
    // 都先进入排空，放行几拍之后才真的把时钟停下来
    if (draining == 0) {
      bool req = exit_seen;
      for (const Logic64* r : halt_req) {
        if (uint64_t(*r) != 0) req = true;
      }
      if (req) draining = kDrainCycles;
    } else if (--draining == 0) {
      clk->Stop();
    }

    EmitTraces();
  }

 private:
  struct Wire {
    uint32_t en = 0, we = 0, addr = 0, be = 0, wdata = 0, rdata = 0;
    uint32_t uart = 0, exit_hit = 0;
  };

  void EmitTraces() {
    if (TraceDisabled()) return;
    TracePerCycle("en", wire.en);
    TracePerCycle("we", wire.we);
    TracePerCycle("addr", wire.addr);
    TracePerCycle("be", wire.be);
    TracePerCycle("wdata", wire.wdata);
    TracePerCycle("rdata", wire.rdata);
    TracePerCycle("uart_write", wire.uart);
    TracePerCycle("exit_write", wire.exit_hit);
    TracePerCycle("uart_bytes", uart_pending);
    TracePerCycle("exit_code", exit_code);
    TracePerCycle("halted", exit_seen ? 1u : 0u);
    TracePerCycle("cycle", cycle_count);
    TracePerCycle("draining", draining);
  }

  uint32_t base;
  uint32_t size;
  uint64_t limit;
  Rv32MemPort port;
  Wire wire;
  std::vector<const Logic64*> halt_req;

  Logic64 halted, uart_bytes;
  std::string uart_out;        // Cycle 独占
  uint64_t uart_pending = 0;
  uint64_t cycle_count = 0;
  uint64_t draining = 0;
  bool exit_seen = false;
  bool limit_hit = false;
  uint32_t exit_code = 0;
};

}  // namespace latch

#endif
