#ifndef _LATCH_BACH_IP_CHIP_CHIP_PORTS_
#define _LATCH_BACH_IP_CHIP_CHIP_PORTS_

// chip 层的端口束与配置地址映射。
//
// SCP 桩经 scp_ctrl 把配置事务发给各 core 的 ctrl_noc 端点，端点按 addr_map 查
// 出目的模块，下一拍写到那个模块的 cfg 口。

#include <cstdint>
#include <memory>

#include "base/logic.h"

namespace latch {
namespace bach {

// ── core 内的配置地址映射 ──
//
// ctrl_noc 看到的是 core 内全部地址空间。原文只列了目的模块的名字，没给逐段的
// 基址，下面这一套按模块排，标为待定：等 core 的 addr_map 冻结再核。
//
// 与 RV core 自己看到的那套地址（rv_ports.h）不是一回事：那一套是 kernel 跑起来
// 之后指令用的，这一套是 boot 期 SCP 往里灌配置用的，两者各自成体系。
enum CfgTarget : uint32_t {
  kCfgRouter = 0,     // RouterTable 与 CSR
  kCfgTs = 1,         // TS 的 CFG_REG
  kCfgDte = 2,        // 三个 DSA 的寄存器
  kCfgMu = 3,
  kCfgVu = 4,
  kCfgItcm = 5,       // 三个 RV core 的 ITCM，低位带 core 号
  kCfgDtcm = 6,
  kCfgSmem = 7,       // Share Mem
  kCfgCmem = 8,       // Core Mem 后门
  kCfgMmem = 9,       // Matrix Mem 后门
  kCfgNone = 10,      // 不在本 core 视野内
};

constexpr uint64_t kCfgRouterBase = 0x000000;
constexpr uint64_t kCfgTsBase = 0x100000;
constexpr uint64_t kCfgDteBase = 0x200000;
constexpr uint64_t kCfgMuBase = 0x210000;
constexpr uint64_t kCfgVuBase = 0x220000;
constexpr uint64_t kCfgItcmBase = 0x300000;   // 每个 RV core 占 0x10000
constexpr uint64_t kCfgDtcmBase = 0x340000;
constexpr uint64_t kCfgSmemBase = 0x400000;
constexpr uint64_t kCfgCmemBase = 0x500000;
constexpr uint64_t kCfgMmemBase = 0x600000;
constexpr uint64_t kCfgSegSize = 0x100000;
constexpr uint64_t kCfgDsaSize = 0x10000;
constexpr uint64_t kCfgRvStride = 0x10000;

// 一笔配置事务落在哪个模块上，以及是三个 RV core 里的哪一个。
struct CfgRoute {
  CfgTarget target = kCfgNone;
  uint64_t index = 0;      // ITCM / DTCM 时是 RV core 号
  uint64_t offset = 0;     // 目的模块内的偏移
};

inline CfgRoute LookupCfg(uint64_t addr) {
  CfgRoute r;
  auto in = [&](uint64_t base, uint64_t size) {
    return addr >= base && addr < base + size;
  };
  if (in(kCfgRouterBase, kCfgSegSize)) {
    r.target = kCfgRouter;
    r.offset = addr - kCfgRouterBase;
  } else if (in(kCfgTsBase, kCfgSegSize)) {
    r.target = kCfgTs;
    r.offset = addr - kCfgTsBase;
  } else if (in(kCfgDteBase, kCfgDsaSize)) {
    r.target = kCfgDte;
    r.offset = addr - kCfgDteBase;
  } else if (in(kCfgMuBase, kCfgDsaSize)) {
    r.target = kCfgMu;
    r.offset = addr - kCfgMuBase;
  } else if (in(kCfgVuBase, kCfgDsaSize * 2)) {
    r.target = kCfgVu;
    r.offset = addr - kCfgVuBase;
  } else if (in(kCfgItcmBase, kCfgRvStride * 3)) {
    r.target = kCfgItcm;
    r.index = (addr - kCfgItcmBase) / kCfgRvStride;
    r.offset = (addr - kCfgItcmBase) % kCfgRvStride;
  } else if (in(kCfgDtcmBase, kCfgRvStride * 3)) {
    r.target = kCfgDtcm;
    r.index = (addr - kCfgDtcmBase) / kCfgRvStride;
    r.offset = (addr - kCfgDtcmBase) % kCfgRvStride;
  } else if (in(kCfgSmemBase, kCfgSegSize)) {
    r.target = kCfgSmem;
    r.offset = addr - kCfgSmemBase;
  } else if (in(kCfgCmemBase, kCfgSegSize)) {
    r.target = kCfgCmem;
    r.offset = addr - kCfgCmemBase;
  } else if (in(kCfgMmemBase, kCfgSegSize)) {
    r.target = kCfgMmem;
    r.offset = addr - kCfgMmemBase;
  }
  return r;
}

// ── SCP 桩 → ctrl_noc 端点 ──
//
// 32 bit/T，每笔事务一拍。cfg_core 命中本 core 或带广播标记时端点才锁存。
class ScpCtrlPort : public Logic {
 public:
  Logic64 cfg_valid, cfg_core, cfg_addr, cfg_we, cfg_wdata, cfg_bcast,
      cfg_rdata, cfg_seq;

  explicit ScpCtrlPort(ClockPtr c)
      : cfg_valid(c), cfg_core(c), cfg_addr(c), cfg_we(c), cfg_wdata(c),
        cfg_bcast(c), cfg_rdata(c), cfg_seq(c) {
    Fields(cfg_valid, cfg_core, cfg_addr, cfg_we, cfg_wdata, cfg_bcast,
           cfg_rdata, cfg_seq);
  }

  void Drive(uint64_t core, uint64_t addr, bool we, uint64_t data, bool bcast,
             uint64_t seq) {
    cfg_valid = 1;
    cfg_core = core;
    cfg_addr = addr;
    cfg_we = we ? 1 : 0;
    cfg_wdata = data;
    cfg_bcast = bcast ? 1 : 0;
    cfg_seq = seq;
  }
  void Idle() {
    cfg_valid = 0;
    cfg_core = 0;
    cfg_addr = 0;
    cfg_we = 0;
    cfg_wdata = 0;
    cfg_bcast = 0;
    cfg_seq = cfg_seq.Get();
  }
  // rdata 下一拍回。写者是端点那一侧。
  void DriveRdata(uint64_t v) { cfg_rdata = v; }

  bool Valid() const { return cfg_valid.Get() != 0; }
  uint64_t Core() const { return cfg_core.Get(); }
  uint64_t Addr() const { return cfg_addr.Get(); }
  bool We() const { return cfg_we.Get() != 0; }
  uint64_t Wdata() const { return cfg_wdata.Get(); }
  bool Bcast() const { return cfg_bcast.Get() != 0; }
  uint64_t Seq() const { return cfg_seq.Get(); }
  uint64_t Rdata() const { return cfg_rdata.Get(); }
};

// ── 中断上报 ──
//
// core_status → SCP。本轮只留接口名与状态位，不实现行为。
class AsyncIntPort : public Logic {
 public:
  Logic64 int_valid, int_code, int_core;

  explicit AsyncIntPort(ClockPtr c)
      : int_valid(c), int_code(c), int_core(c) {
    Fields(int_valid, int_code, int_core);
  }

  void Drive(uint64_t code, uint64_t core) {
    int_valid = 1;
    int_code = code;
    int_core = core;
  }
  void Idle() {
    int_valid = 0;
    int_code = 0;
    int_core = 0;
  }
  bool Valid() const { return int_valid.Get() != 0; }
};

}  // namespace bach
}  // namespace latch

#endif
