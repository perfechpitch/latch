#ifndef _LATCH_BACH_IP_CHIP_CORE_MEMORY_BANKED_MEM_
#define _LATCH_BACH_IP_CHIP_CORE_MEMORY_BANKED_MEM_

// 三块存储共用的四级骨架。
//
//   M1 请求锁存    每个 master 一个深度 1 的槽，槽空才拉 ready；未获授权的请求
//                  原地保持，本模块不丢请求
//   M2 bank 仲裁   每 bank 每拍选一个：先比优先级，同优先级比请求拍，再比插入序
//   M3 SRAM 读写   落到 bank 的稀疏行上
//   M4 延迟线      按该 master 的端到端拍数排响应
//
// 三块存储的差别只在 bank 数、地址粒度、各 master 的优先级与延迟，以及同 bank
// 撞车时的处理：Core Mem 排队等下一拍，Matrix Mem 是硬约束，撞了直接断言失败。
//
// 存储内容按行稀疏存：一行 granule 字节，没写过的行读出来是全 0。真机上是
// 未初始化，模型里给确定值，免得比对结果依赖没写过的那一段。
//
// scale 寄存器：Core Mem 每 bank 另有一块寄存器存 scale，与 SRAM 地址一一映射，
// 128 B 数据配 4 B scale。scale 使能拉高时同时读写对应地址的那一份 —— MU 与 VU
// 访问 CM 按 132 B 读写，读回来的块尾就是这 4 B，写进去的也照这个排。不拉高时
// 只走 SRAM 那一段，有效带宽 128 B。
//
// ECC 按 128 bit 一组，编解码在 SRAM 接口处。byte_mask 非全 1 时那一行留个记录，
// 读它时不做 ECC 检测。1 bit 错用计数器记、2 bit 错报错，这一轮只留计数器与
// 接口名，不注错。

#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 一个 master 在这块存储上的口径。
struct MemMaster {
  std::string name;
  uint64_t priority = 0;   // 数字小的先得
  uint64_t read_latency = 1;
  uint64_t write_latency = 1;
};

struct BankedMemCfg {
  uint64_t bank_num = 8;
  uint64_t granule = 128;      // 地址粒度，一行的字节数
  uint64_t capacity = 1 << 20;
  // Matrix Mem 的硬约束：同一 bank 不许两个 master 同时访问。撞了不重试、不排队，
  // 直接断言失败 —— 这是软件排算子时就该保证的事，用重试掩盖会让配置错误一直
  // 查不出来。Core Mem 不设这条，撞了排队。
  bool exclusive_bank = false;
  // exclusive_bank 撞车时是停下还是只记一笔。默认停下：真硬件上被让路的那一笔
  // 直接丢弃，DTE 没有重传通路，丢一笔结果就错，所以模型不能让它悄悄过去。
  // 只在测试里关掉，用来验证冲突确实被检出。
  bool halt_on_conflict = true;
  // Share Mem 的四个 master 按轮询（原文未给，建模计划的默认值）。开着时忽略
  // priority，从上一拍赢家的下一个开始扫。
  bool round_robin = false;
  // 每 granule 配几字节 scale 寄存器。0 表示这块存储没有这一段。
  uint64_t scale_bytes = 0;
};

class BankedMem : public BachModule {
 public:
  BankedMem(ClockPtr clock, const std::string& name, BankedMemCfg const& shape,
            std::vector<MemMaster> master_list, uint64_t parent = 0,
            bool tick = true)
      : BachModule(clock, name, parent, tick),
        cfg(shape),
        masters(std::move(master_list)),
        granted(clock),
        conflicts(clock),
        pending_cnt(clock) {
    for (uint64_t i = 0; i < masters.size(); ++i) {
      ports.push_back(std::make_shared<MemPort>(clock));
      slots.emplace_back();
    }
    ecc_single.assign(masters.size(), 0);
    ecc_double.assign(masters.size(), 0);
  }

  MemPort& Port(uint64_t m) { return *ports.at(m); }
  // 装配层把这一根线交给对应的 master，两端指向同一个对象。
  std::shared_ptr<MemPort> PortPtr(uint64_t m) const { return ports.at(m); }

  // 只给测试用：关掉之后撞车只记一笔、不停下，用来验证冲突确实被检出。
  void RelaxConflictHalt() { cfg.halt_on_conflict = false; }
  uint64_t MasterNum() const { return masters.size(); }

  uint64_t Granted() const { return granted.Get(); }
  uint64_t Conflicts() const { return conflicts.Get(); }
  uint64_t PendingCount() const { return pending_cnt.Get(); }

  // ── ECC ──
  //
  // 1 bit 错的计数器每个 master 一个，可经 NOC 读取；2 bit 错报错。这一轮不注
  // 错，所以计数恒为 0，留的是接口。byte_mask 非全 1 的那些行读时不做检测。
  uint64_t EccSingleBit(uint64_t master) const {
    return ecc_single.at(master);
  }
  uint64_t EccDoubleBit(uint64_t master) const {
    return ecc_double.at(master);
  }
  // 这一行是不是被非全 1 的 byte_mask 写过。写过就不参与 ECC 检测。
  bool MaskedRow(uint64_t addr) const {
    return masked.count(addr / cfg.granule) != 0;
  }

  // 构造期或测试用：直接铺一份 scale。
  void PokeScale(uint64_t addr, ByteBlock const& bytes) {
    LOGCHECK(cfg.scale_bytes != 0, "BankedMem: 这块存储没有 scale 寄存器。");
    scale_store[addr / cfg.granule] = bytes;
  }
  ByteBlock PeekScale(uint64_t addr) const {
    auto it = scale_store.find(addr / cfg.granule);
    if (it == scale_store.end()) return ByteBlock(cfg.scale_bytes, 0);
    return it->second;
  }

  // 构造期或测试用：直接铺一行，不走端口。
  void Poke(uint64_t addr, ByteBlock const& bytes) {
    LOGCHECK(addr + bytes.size() <= cfg.capacity, "BankedMem: 写越界。");
    WriteBytes(addr, bytes, 0, bytes.size());
  }
  ByteBlock Peek(uint64_t addr, uint64_t bytes) const {
    return ReadBytes(addr, bytes);
  }

  bool Quiescent() const override {
    if (!pipe.empty()) return false;
    for (auto const& s : slots) {
      if (s.busy) return false;
    }
    return true;
  }

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做。
    Respond(now);
    Arbitrate(now);
    Accept(now);

    granted = granted_pending;
    conflicts = conflict_pending;
    uint64_t busy = 0;
    for (auto const& s : slots) {
      if (s.busy) ++busy;
    }
    pending_cnt = busy;
    TracePerCycle("granted", granted_pending);
    TracePerCycle("pending", busy);
  }

 private:
  struct Slot {
    bool busy = false;
    MemReqView req;
    uint64_t arrive_cycle = 0;  // 请求进槽的那一拍，同优先级按它排先到先得
    uint64_t seq = 0;
  };
  struct PipeItem {
    uint64_t master = 0;
    uint64_t done_cycle = 0;
    ByteBlockPtr data;
  };

  uint64_t BankOf(uint64_t addr) const {
    return (addr / cfg.granule) % cfg.bank_num;
  }

  // M1：槽空才收，收下的原地待命。ready 是本拍算出来的，master 下一拍才看到。
  void Accept(uint64_t now) {
    for (uint64_t m = 0; m < masters.size(); ++m) {
      MemReqView r = ReadMemReq(*ports[m]);
      if (r.valid && !slots[m].busy) {
        slots[m].busy = true;
        slots[m].req = r;
        slots[m].arrive_cycle = now;
        slots[m].seq = next_seq++;
      }
    }
    // ready 在本拍末按槽的状态给：空 = 下一拍能收。
    for (uint64_t m = 0; m < masters.size(); ++m) {
      auto it = rsp_this_cycle.find(m);
      if (it != rsp_this_cycle.end()) {
        ports[m]->DriveSlave(!slots[m].busy, true, it->second);
      } else {
        ports[m]->DriveSlave(!slots[m].busy, false, ByteBlockPtr());
      }
    }
    rsp_this_cycle.clear();
  }

  // M2 与 M3：每 bank 每拍选一个，选中的做完读写进延迟线。
  void Arbitrate(uint64_t now) {
    std::map<uint64_t, std::vector<uint64_t>> by_bank;
    for (uint64_t m = 0; m < masters.size(); ++m) {
      if (!slots[m].busy) continue;
      by_bank[BankOf(slots[m].req.addr)].push_back(m);
    }

    for (auto const& kv : by_bank) {
      std::vector<uint64_t> const& cands = kv.second;
      if (cfg.exclusive_bank && cands.size() > 1) {
        ++conflict_pending;
        // 同一 bank 两个 master 同时访问是硬约束被违反，不是正常工作点。
        // 真硬件上只执行 MU、被让路的那一笔直接丢弃，DTE 没有重传通路，丢一笔
        // 结果就错。所以这里直接停，不做等价的重试掩盖。
        LOGCHECK(!cfg.halt_on_conflict,
                 "BankedMem: 同一 bank 上有两个 master 同时访问，硬约束被违反。");
      }
      uint64_t win = cands.front();
      if (cfg.round_robin) {
        win = RoundRobinPick(cands);
      } else {
        for (uint64_t m : cands) {
          if (Better(m, win)) win = m;
        }
      }
      Serve(win, now);
    }
  }

  // 从上一拍赢家的下一个开始扫，第一个在候选里的就是本拍的赢家。
  uint64_t RoundRobinPick(std::vector<uint64_t> const& cands) {
    for (uint64_t k = 1; k <= masters.size(); ++k) {
      uint64_t m = (rr_last + k) % masters.size();
      for (uint64_t c : cands) {
        if (c == m) {
          rr_last = m;
          return m;
        }
      }
    }
    return cands.front();
  }

  // 先比优先级，同优先级比请求拍（先到先得），再比插入序。
  bool Better(uint64_t a, uint64_t b) const {
    if (masters[a].priority != masters[b].priority) {
      return masters[a].priority < masters[b].priority;
    }
    if (slots[a].arrive_cycle != slots[b].arrive_cycle) {
      return slots[a].arrive_cycle < slots[b].arrive_cycle;
    }
    return slots[a].seq < slots[b].seq;
  }

  void Serve(uint64_t m, uint64_t now) {
    MemReqView const& r = slots[m].req;
    LOGCHECK(r.addr + r.woff + r.bytes <= cfg.capacity, "BankedMem: 访存越界。");
    ByteBlockPtr data;
    uint64_t lat;
    bool with_scale = r.scale_en && cfg.scale_bytes != 0;
    if (r.we) {
      // 只改 wdata 里 [woff, woff + bytes) 那一段：一次请求搬的是一整块，而
      // 一条向量的两端可能落在块中间，两头的字节归相邻的数据。
      if (r.wdata) WriteBytes(r.addr + r.woff, *r.wdata, r.woff, r.bytes);
      // 按 Byte mask 写过的行读时不做 ECC 检测，这里记一笔。
      if (r.bytes != 0 && r.bytes < cfg.granule) {
        masked.insert((r.addr + r.woff) / cfg.granule);
      }
      // scale 使能：块尾那几字节写进对应地址的 scale 寄存器。
      if (with_scale && r.wdata) WriteScale(r.addr, *r.wdata, r.bytes);
      lat = masters[m].write_latency;
    } else {
      ByteBlock got = ReadBytes(r.addr, r.bytes);
      if (with_scale) AppendScale(r.addr, r.bytes, &got);
      data = std::make_shared<ByteBlock>(std::move(got));
      lat = masters[m].read_latency;
    }
    // 四级之和就是这个 master 的端到端拍数。M1、M2 各占一拍已经走掉，
    // 剩下的记在延迟线上。
    uint64_t rest = lat > 2 ? lat - 2 : 1;
    pipe.push_back({m, now + rest, data});
    slots[m].busy = false;
    ++granted_pending;
  }

  // M4：到点的送出去。每个 master 每拍最多一笔。
  void Respond(uint64_t now) {
    for (auto it = pipe.begin(); it != pipe.end();) {
      if (it->done_cycle <= now && rsp_this_cycle.count(it->master) == 0) {
        rsp_this_cycle[it->master] = it->data;
        it = pipe.erase(it);
      } else {
        ++it;
      }
    }
  }

  // 读回来的块尾接上这一段地址对应的 scale：每 granule 一份。
  void AppendScale(uint64_t addr, uint64_t bytes, ByteBlock* out) const {
    uint64_t rows = bytes == 0 ? 1 : (bytes + cfg.granule - 1) / cfg.granule;
    for (uint64_t k = 0; k < rows; ++k) {
      ByteBlock one = PeekScale(addr + k * cfg.granule);
      out->insert(out->end(), one.begin(), one.end());
    }
  }

  // 写进来的块尾那几字节是 scale，按 granule 拆开存。
  void WriteScale(uint64_t addr, ByteBlock const& data, uint64_t bytes) {
    uint64_t n = bytes == 0 ? (data.size() > cfg.scale_bytes
                                   ? data.size() - cfg.scale_bytes
                                   : 0)
                            : bytes;
    uint64_t rows = n == 0 ? 1 : (n + cfg.granule - 1) / cfg.granule;
    for (uint64_t k = 0; k < rows; ++k) {
      uint64_t at = n + k * cfg.scale_bytes;
      ByteBlock one(cfg.scale_bytes, 0);
      for (uint64_t i = 0; i < cfg.scale_bytes && at + i < data.size(); ++i) {
        one[i] = data[at + i];
      }
      scale_store[(addr + k * cfg.granule) / cfg.granule] = one;
    }
  }

  void WriteBytes(uint64_t addr, ByteBlock const& bytes, uint64_t from,
                  uint64_t len) {
    uint64_t end = from + len;
    if (end > bytes.size()) end = bytes.size();
    uint64_t off = 0;
    while (from + off < end) {
      uint64_t a = addr + off;
      uint64_t row = a / cfg.granule;
      uint64_t in_row = a % cfg.granule;
      uint64_t n = cfg.granule - in_row;
      if (n > end - from - off) n = end - from - off;
      ByteBlock& line = store[row];
      if (line.empty()) line.assign(cfg.granule, 0);
      for (uint64_t i = 0; i < n; ++i) line[in_row + i] = bytes[from + off + i];
      off += n;
    }
  }

  ByteBlock ReadBytes(uint64_t addr, uint64_t bytes) const {
    ByteBlock out(bytes, 0);
    uint64_t off = 0;
    while (off < bytes) {
      uint64_t a = addr + off;
      uint64_t row = a / cfg.granule;
      uint64_t in_row = a % cfg.granule;
      uint64_t n = cfg.granule - in_row;
      if (n > bytes - off) n = bytes - off;
      auto it = store.find(row);
      if (it != store.end()) {
        for (uint64_t i = 0; i < n; ++i) out[off + i] = it->second[in_row + i];
      }
      off += n;
    }
    return out;
  }

  BankedMemCfg cfg;
  std::vector<MemMaster> masters;
  std::vector<std::shared_ptr<MemPort>> ports;

  // Step 独占。
  std::vector<Slot> slots;
  std::deque<PipeItem> pipe;
  std::map<uint64_t, ByteBlockPtr> rsp_this_cycle;
  std::map<uint64_t, ByteBlock> store;        // 按行稀疏
  std::map<uint64_t, ByteBlock> scale_store;  // 每行一份 scale
  std::set<uint64_t> masked;                  // 被非全 1 的 byte_mask 写过的行
  std::vector<uint64_t> ecc_single, ecc_double;
  uint64_t next_seq = 0;
  uint64_t rr_last = 0;
  uint64_t granted_pending = 0, conflict_pending = 0;

  Logic64 granted, conflicts, pending_cnt;
};

}  // namespace bach
}  // namespace latch

#endif
