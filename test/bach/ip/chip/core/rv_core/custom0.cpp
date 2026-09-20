// custom-0 自定义指令的译码与执行。
//
// 《RV Core自定义指令详细设计》的 6 条：dsaw / dsawi / dsar / dsari / loop /
// task_done。软件侧的编码宏在 src/bach/compiler/kernel/self_inst.h，模型侧的译码
// 与执行在 src/bach/ip/chip/core/rv_core/custom0.h。
//
// 这里的做法是按字段表把编码重新拼一遍，灌进 ITCM 跑，再看露到端口上的东西：
// 拼法与 custom0.h 的拆法是两份独立的实现（尤其是 16 bit 立即数怎么占 rd / rs2
// 两个字段、loop 的 B 型偏移），两边对不上这几条用例就红。
//
// 观察点全部取执行之后对外端口上的结果——DSA 配置口上的地址与数据、读回来经
// dsa_rq 异步写回的路径、task_done 的完成口、loop 的退出次数——不去戳内部状态。
//
// kernel 目前仍走 MMIO 约定地址那一组，所以这条通路只有这里跑得到。

#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "bach/ip/chip/core/rv_core/custom0.h"
#include "bach/ip/chip/core/rv_core/rv_core.h"
#include "base/clock.h"
#include "base/runtime.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

// 手拼的那几段程序灌在 ITCM 的这一段里：不装 kernel，只跑这些指令。
constexpr uint64_t kProg = 0x100;

// 用到的通用寄存器编号。
constexpr uint32_t kX0 = 0, kA0 = 10, kA1 = 11, kA2 = 12, kA3 = 13;

// TS 下发的身份，完成时应该原样带回来。
constexpr uint64_t kStream = 3, kTask = 5, kUser = 77, kPath = 9;

// ── 按字段表拼编码 ──
//
// R 型的字段位置：funct7(31:25) | rs2(24:20) | rs1(19:15) | funct3(14:12) |
//                 rd(11:7) | opcode(6:0)，opcode 取 custom-0 即 0x0B。
uint32_t EncR(uint32_t funct3, uint32_t funct7, uint32_t rd, uint32_t rs1,
              uint32_t rs2) {
  return (funct7 << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) |
         (rd << 7) | 0x0Bu;
}

// 立即数寻址那两条：imm[4:0] 占 rd 字段，imm[9:5] 占 rs2 字段，
// imm[15:10] 与「这是立即数寻址」的标志位一起进 funct7。
uint32_t ImmFunct7(uint32_t imm16) { return 0x40u | ((imm16 >> 10) & 0x3Fu); }
uint32_t ImmRd(uint32_t imm16) { return imm16 & 0x1Fu; }
uint32_t ImmRs2(uint32_t imm16) { return (imm16 >> 5) & 0x1Fu; }

// dsaw rs, addr：数据在 rs1，DSA 字节地址在 rs2，bit31 = 0。
uint32_t EncDsaw(uint32_t rs, uint32_t addr_reg) {
  return EncR(1, 0, 0, rs, addr_reg);
}
// dsawi rs, addr：地址是 16 bit 立即数，bit31 = 1。
uint32_t EncDsawi(uint32_t rs, uint32_t imm16) {
  return EncR(1, ImmFunct7(imm16), ImmRd(imm16), rs, ImmRs2(imm16));
}
// dsar d, addr：地址在 rs1，结果写 rd，bit31 = 0。
uint32_t EncDsar(uint32_t d, uint32_t addr_reg) {
  return EncR(0, 0, d, addr_reg, 0);
}
// dsari d, addr：地址是 16 bit 立即数，结果写 rd，bit31 = 1。地址的
// imm[4:0] 与 imm[9:5] 分别占 rs1 与 rs2 两个字段。
uint32_t EncDsari(uint32_t d, uint32_t imm16) {
  return EncR(0, ImmFunct7(imm16), d, ImmRd(imm16), ImmRs2(imm16));
}
// task_done ts：funct3=010，其余字段全 0，TS 标志占 bit31。
uint32_t EncTaskDone(uint32_t ts) {
  return EncR(2, 0, 0, 0, 0) | ((ts & 1u) << 31);
}
// loop max, cur, offset：按 B 型排——imm[12] 在 bit31、imm[11] 在 bit7、
// imm[10:5] 在 bits[30:25]、imm[4:1] 在 bits[11:8]；rs1 是最大次数，
// rs2 是当前次数，funct3=110。offset 是有符号字节偏移，低 1 bit 隐含为 0。
uint32_t EncLoop(uint32_t max_reg, uint32_t cur_reg, int32_t offset) {
  uint32_t off = uint32_t(offset);
  uint32_t funct7 = (((off >> 12) & 1u) << 6) | (((off >> 5) & 0x3Fu));
  uint32_t rd = (((off >> 1) & 0xFu) << 1) | ((off >> 11) & 1u);
  return EncR(6, funct7, rd, max_reg, cur_reg);
}
// 拼程序时也要放几条普通指令：addi rd, rs1, imm。
uint32_t EncAddi(uint32_t rd, uint32_t rs1, int32_t imm) {
  return ((uint32_t(imm) & 0xFFFu) << 20) | (rs1 << 15) | (rd << 7) | 0x13u;
}

void Poke(RvCore& rv, uint64_t at, std::vector<uint32_t> const& words) {
  std::vector<uint8_t> bytes;
  bytes.reserve(words.size() * 4);
  for (uint32_t w : words) {
    for (int k = 0; k < 4; ++k) bytes.push_back(uint8_t((w >> (8 * k)) & 0xFFu));
  }
  rv.PokeItcm(at, bytes);
}

// 扮演 RV core 外面的世界：按拍下发一笔 task、在 DSA 那一侧收配置写并回读数据、
// 收完成。三件事放在一个模块里，免得两个模块同时驱动 DsaCfg 的 ready。
class Env : public BachModule {
 public:
  // reads 按读请求发出的顺序逐个回；写测试传空的。
  Env(ClockPtr c, RvCore& target, uint64_t entry, std::vector<uint64_t> reads)
      : BachModule(c, "env"), rv(target), pc(entry), ret(std::move(reads)) {}

  struct Wr {
    uint64_t at = 0, data = 0;
  };

  std::vector<Wr> writes;            // DSA 配置口上收到的写
  std::vector<uint64_t> read_addrs;  // DSA 配置口上收到的读
  // 完成口打了几拍、带回的身份。valid 没有序号，按上升沿记一笔。
  uint64_t dones = 0;
  uint64_t done_stream = 0, done_task = 0, done_user = 0, done_path = 0;
  // dsa_rq 真正写回了几笔，用来确认读回来的值走完了那条异步通路。
  uint64_t returned = 0;
  uint64_t read_latency = 3;

 protected:
  void Step() override {
    uint64_t now = CycleNow();

    if (!sent && now >= fire_at) {
      rv.Cmd().Drive(pc, kStream, kTask, kUser, kPath, /*dsa_en=*/true,
                     /*issue_seq=*/1);
      if (rv.Cmd().Ready()) sent = true;
    } else {
      rv.Cmd().Idle();
    }

    bool done_now = rv.Done().Valid();
    if (done_now && !done_prev) {
      ++dones;
      done_stream = rv.Done().stream_id.Get();
      done_task = rv.Done().task_id.Get();
      done_user = rv.Done().user_id.Get();
      done_path = rv.Done().pid.Get();
    }
    done_prev = done_now;

    // 收 DSA 配置口：一个请求可能停几拍，按序号去重。
    if (rv.DsaCfg().Valid() && rv.DsaCfg().Seq() != last_cfg_seq) {
      last_cfg_seq = rv.DsaCfg().Seq();
      if (rv.DsaCfg().req_we.Get() != 0) {
        writes.push_back({rv.DsaCfg().req_addr.Get(),
                          rv.DsaCfg().req_wdata.Get()});
      } else {
        read_addrs.push_back(rv.DsaCfg().req_addr.Get());
        pending.push_back(now + read_latency);
      }
    }
    rv.DsaCfg().DriveReady(true);

    // 回读数据：一笔一笔回，等 dsa_rq 收走（Returned 加一）再回下一笔。同一笔连着
    // 几拍驱动同一个序号是安全的——dsa_rq 按序号去重。
    returned = rv.Rq().Returned();
    if (sent_rsp && returned > sent_at) {
      sent_rsp = false;
      pending.pop_front();
    }
    if (!sent_rsp && !pending.empty() && pending.front() <= now &&
        next_ret < ret.size()) {
      cur_data = ret[next_ret++];
      cur_seq = ++rdata_seq;
      sent_at = returned;
      sent_rsp = true;
    }
    if (sent_rsp) {
      rv.DsaRdata().Drive(cur_data, cur_seq);
    } else {
      rv.DsaRdata().Idle();
    }
  }

 private:
  RvCore& rv;
  uint64_t pc;
  std::vector<uint64_t> ret;
  uint64_t fire_at = 2;
  bool sent = false;

  uint64_t last_cfg_seq = 0;
  std::deque<uint64_t> pending;   // 每条读请求该在哪一拍回
  bool done_prev = false;

  uint64_t next_ret = 0, rdata_seq = 0, cur_data = 0, cur_seq = 0, sent_at = 0;
  bool sent_rsp = false;
};

// 起一个 RV core、灌一段程序、跑到停，再把观察结果交出来。省得每条用例都写一遍
// 建时钟 / 建模块 / JoinAll 的账。程序只跑任务那一段，不装 kernel，所以 ITCM 里
// 只有这几十条指令。
struct Case {
  uint64_t dones = 0, done_stream = 0, done_task = 0, done_user = 0, done_path = 0;
  uint64_t finishes = 0, returned = 0, insts = 0;
  bool at_wait = false;
  std::vector<Env::Wr> writes;
  std::vector<uint64_t> read_addrs;
};

Case RunProgram(std::vector<uint32_t> const& words,
                std::vector<uint64_t> const& reads, uint64_t cycles = 400) {
  Case out;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RvCore rv(clk, "rv_dte", RvUnit::kDte);
    class Driver : public BachModule {
     public:
      Driver(ClockPtr c, RvCore& t) : BachModule(c, "driver"), rv(t) {}

     protected:
      void Step() override { rv.RunStep(); }

     private:
      RvCore& rv;
    };
    Driver drv(clk, rv);
    Env env(clk, rv, kProg, reads);
    Poke(rv, kProg, words);

    clk->Continue(cycles * kPeriod);
    RT::JoinAll();

    out.dones = env.dones;
    out.done_stream = env.done_stream;
    out.done_task = env.done_task;
    out.done_user = env.done_user;
    out.done_path = env.done_path;
    out.finishes = rv.TaskQueue().Finishes();
    out.returned = env.returned;
    out.insts = rv.Exec().Insts();
    out.at_wait = rv.Exec().AtWait();
    out.writes = env.writes;
    out.read_addrs = env.read_addrs;
  }
  RT::Reset();
  return out;
}

}  // namespace

// dsaw / dsawi：写 DSA 单寄存器。两条都该把同一个数据写到各自的地址上，区别只
// 在地址来自寄存器还是来自指令里的 16 bit 立即数。
TEST(BachCustom0, WritesDsaRegisterBothAddressModes) {
  std::vector<uint32_t> words = {
      EncAddi(kA0, kX0, 0x123),      // 数据
      EncAddi(kA1, kX0, 0x40),       // 寄存器寻址的地址
      EncDsaw(kA0, kA1),             // DSA[0x40] = 0x123
      EncDsawi(kA0, 0x234),          // DSA[0x234] = 0x123
      EncTaskDone(1),
  };
  Case c = RunProgram(words, {});
  ASSERT_EQ(c.writes.size(), 2u);
  EXPECT_EQ(c.writes[0].at, 0x40u);
  EXPECT_EQ(c.writes[0].data, 0x123u);
  // 立即数占 rd / rs2 / funct7 三段，0x234 在低两段上都不是零（rs2 段 0x11、
  // rd 段 0x14），任何一段接错这个地址都对不上。
  EXPECT_EQ(c.writes[1].at, 0x234u);
  EXPECT_EQ(c.writes[1].data, 0x123u);
  EXPECT_EQ(c.dones, 1u);
}

// 立即数地址的每一段都咬得住：挑几个分别落在三段里的地址，逐个过。
//
// 走到配置口的地址只能落在本核那个 DSA 的 IO 窗口里——DTE / MU 是 4 KB
// （0x000～0xFFF），VU 是 20 KB。16 bit 立即数的高 4 位（imm[15:12]）超出窗口，
// 端到端跑不到，只能在译码那一层核（见 DecodeMapsEachEncoding 的 0xBEEF）。
// 也就是说：立即数编码给了 0～64K，真正用得上的只有窗口这么大一块。
TEST(BachCustom0, ImmediateAddressLandsInWindow) {
  for (uint32_t addr : {0x0001u, 0x0020u, 0x0400u, 0x0800u, 0x0F00u}) {
    std::vector<uint32_t> words = {
        EncAddi(kA0, kX0, 0x5A),
        EncDsawi(kA0, addr),
        EncTaskDone(1),
    };
    Case c = RunProgram(words, {});
    ASSERT_EQ(c.writes.size(), 1u) << "立即数地址 " << addr;
    EXPECT_EQ(c.writes[0].at, addr) << "立即数地址 " << addr;
    EXPECT_EQ(c.writes[0].data, 0x5Au);
  }
}

// dsar / dsari：读 DSA 寄存器。读不是同步返回的——这一拍拿到的是占位值，真值由
// dsa_rq 异步补进目的寄存器。所以这里让读回来的两个值再原样写回 DSA：能对得上，
// 就说明两条读指令各自把值送进了对的寄存器，而且写那一拍确实等到了写回。
TEST(BachCustom0, ReadsDsaRegisterAndValueReachesConsumer) {
  std::vector<uint32_t> words = {
      EncAddi(kA1, kX0, 0x40),
      EncDsar(kA0, kA1),        // a0 <- DSA[0x40]，地址在寄存器
      EncDsari(kA2, 0x44),      // a2 <- DSA[0x44]，地址是立即数
      EncAddi(kA3, kX0, 0x90),
      EncDsaw(kA0, kA3),        // 把第一条读回来的值写出去
      EncDsaw(kA2, kA3),        // 把第二条读回来的值写出去
      EncTaskDone(1),
  };
  Case c = RunProgram(words, {0xAA, 0xBB});
  EXPECT_EQ(c.read_addrs, (std::vector<uint64_t>{0x40, 0x44}));
  EXPECT_EQ(c.returned, 2u);
  ASSERT_EQ(c.writes.size(), 2u);
  // 按发出的顺序回来，第一个落到 a0、第二个落到 a2，没有串位。
  EXPECT_EQ(c.writes[0].at, 0x90u);
  EXPECT_EQ(c.writes[0].data, 0xAAu);
  EXPECT_EQ(c.writes[1].at, 0x90u);
  EXPECT_EQ(c.writes[1].data, 0xBBu);
  EXPECT_EQ(c.dones, 1u);
}

// task_done 的 TS 位：置位时通知 TS，完成口带着下发时的身份；不置位时只交还
// 自己，完成口不响，核停在等下一个 task 上。两条路的「交还」都要发生。
TEST(BachCustom0, TaskDoneNotifyFlagPicksThePath) {
  std::vector<uint32_t> notified = {EncTaskDone(1)};
  Case c = RunProgram(notified, {});
  EXPECT_EQ(c.finishes, 1u);   // 两条路都交还自己
  EXPECT_EQ(c.dones, 1u);      // 置位这一条另外通知 TS
  EXPECT_EQ(c.done_stream, kStream);
  EXPECT_EQ(c.done_task, kTask);
  EXPECT_EQ(c.done_user, kUser);
  EXPECT_EQ(c.done_path, kPath);
  EXPECT_TRUE(c.at_wait);

  std::vector<uint32_t> yielded = {EncTaskDone(0)};
  Case y = RunProgram(yielded, {});
  EXPECT_EQ(y.finishes, 1u);
  EXPECT_EQ(y.dones, 0u);      // 不置位就不通知 TS
  EXPECT_TRUE(y.at_wait);      // 但一样停下来等下一个 task
}

// loop：rs2 < rs1 回跳，否则顺序执行。循环体累计加一，退出时的次数经 dsaw 写到
// DSA 配置口上，靠这个数把退出条件钉死——多转一圈少转一圈都会露出来。
TEST(BachCustom0, LoopBranchesUntilCurrentReachesMax) {
  const uint32_t kMax = 4;
  std::vector<uint32_t> words = {
      EncAddi(kA0, kX0, kMax),        // 0: 最大次数
      EncAddi(kA2, kX0, 0),           // 1: 当前次数
      EncAddi(kA3, kX0, 0),           // 2: 累计
      EncAddi(kA3, kA3, 1),           // 3: <- 循环体起点
      EncAddi(kA2, kA2, 1),
      EncLoop(kA0, kA2, -8),          // 5: cur < max ? 回 3
      EncAddi(kA1, kX0, 0x40),
      EncDsaw(kA3, kA1),              // 7: DSA[0x40] = 循环次数
      EncTaskDone(1),
  };
  Case c = RunProgram(words, {});
  ASSERT_EQ(c.writes.size(), 1u);
  EXPECT_EQ(c.writes[0].data, kMax);   // 正好转 kMax 圈
  EXPECT_EQ(c.dones, 1u);
}

// cur 一开始就够大时 loop 不跳，循环体一次都不进——顺带把「顺序执行」那一支
// 也走上：偏移字段全 0 时不能当成回跳。
TEST(BachCustom0, LoopFallsThroughWhenAlreadyDone) {
  std::vector<uint32_t> words = {
      EncAddi(kA0, kX0, 0),           // 最大次数 0
      EncAddi(kA2, kX0, 0),           // 当前次数 0，不小于最大值
      EncAddi(kA3, kX0, 7),
      EncLoop(kA0, kA2, -8),          // 3: 不跳，落到下一条
      EncAddi(kA1, kX0, 0x40),
      EncDsaw(kA3, kA1),
      EncTaskDone(1),
  };
  Case c = RunProgram(words, {});
  ASSERT_EQ(c.writes.size(), 1u);
  EXPECT_EQ(c.writes[0].data, 7u);   // 跳了的话这里会是别的值
  EXPECT_EQ(c.dones, 1u);
}

// 译码本身：6 条各落成哪一类，以及那个「同 funct3 靠 bits[29:25] 分开」的
// task_done。flag_check（bits[29:25] = 00001）不在这一版的 6 条里，不接。
TEST(BachCustom0, DecodeMapsEachEncoding) {
  auto isa = std::make_shared<rv32::Rv32>();
  SystemRv32Bach sys(isa, /*pc=*/0, /*thread=*/1, /*dsa_base=*/kDteIoBase);

  auto decode = [&](uint32_t w) {
    return sys.Decode(std::make_shared<InstBinary>(w));
  };

  EXPECT_NE(dynamic_cast<Custom0Dsaw*>(decode(EncDsaw(kA0, kA1)).get()), nullptr);
  EXPECT_NE(dynamic_cast<Custom0Dsawi*>(decode(EncDsawi(kA0, 0x1234)).get()),
            nullptr);
  EXPECT_NE(dynamic_cast<Custom0Dsar*>(decode(EncDsar(kA0, kA1)).get()), nullptr);
  EXPECT_NE(dynamic_cast<Custom0Dsari*>(decode(EncDsari(kA0, 0x1234)).get()),
            nullptr);
  EXPECT_NE(dynamic_cast<Custom0TaskDone*>(decode(EncTaskDone(1)).get()),
            nullptr);
  EXPECT_NE(dynamic_cast<Custom0Loop*>(decode(EncLoop(kA0, kA2, -8)).get()),
            nullptr);

  // 立即数那两条要真的把地址拆对：从指令字里拆回来的 16 bit，得等于拼编码时
  // 传进去的那个数。
  EXPECT_EQ(Custom0Fields::ImmW(EncDsawi(kA0, 0xBEEF)), 0xBEEFu);
  EXPECT_EQ(Custom0Fields::ImmR(EncDsari(kA0, 0xBEEF)), 0xBEEFu);

  // loop 的 B 型偏移正负都要回来。
  EXPECT_EQ(Custom0Fields::ImmB(EncLoop(kA0, kA2, -8)), -8);
  EXPECT_EQ(Custom0Fields::ImmB(EncLoop(kA0, kA2, 4094)), 4094);

  // 不是 custom-0 的照旧交给基类；flag_check 那一档（bits[29:25] = 00001）不在
  // 这一版的 6 条里，也交回基类。
  uint32_t flag_check = EncR(2, 0, 0, kA0, kA2) | (1u << 25);
  EXPECT_EQ(dynamic_cast<Custom0TaskDone*>(decode(flag_check).get()), nullptr);

  // 固件进 wait 那条 WFT（funct3=011）同样不在 6 条里，尤其不能落成 dsaw：
  // 它的旧编码 `.insn i 0x0B, 1, x0, x0, 0` 正是 dsaw，译出来是“往 DSA 偏移 0
  // 写 0”——VU 的偏移 0 是 macro trigger_inst，真机上会误触发一条宏指令。
  uint32_t wft = EncR(3, 0, 0, 0, 0);
  EXPECT_EQ(wft, 0x0000300Bu) << "start.s 里 WFT 的编码，改了要一起改这里";
  EXPECT_EQ(dynamic_cast<Custom0Dsaw*>(decode(wft).get()), nullptr);
  EXPECT_EQ(dynamic_cast<Custom0Dsawi*>(decode(wft).get()), nullptr);
}
