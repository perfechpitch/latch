#ifndef _LATCH_BACH_IP_CHIP_CORE_ROUTER_REDUCE_MODULE_
#define _LATCH_BACH_IP_CHIP_CORE_ROUTER_REDUCE_MODULE_

// ReduceModule：Router 里做逐级归约的那一块，对应 Router MAS 的 ReduceMemory。
//
// 三级：
//   M10 三路输入仲裁  仲裁 SRAM、Bank 与计算资源；进入后锁定当前包直至尾 flit
//   M11 RMW 累加      同一 User 同一包的第一份输入写进这个用户的分区，后续方向
//                     的输入读出当前值、累加、写回，原位 Read-Modify-Write
//   M12 结果流水      某个结果 flit 要的操作数都累加完了就能发，不等整包；一个
//                     包发出首 flit 后锁定到尾 flit，后面的结果没好就占着输出
//                     等。发送前查目标 VC credit，一笔任务头一个 flit 还要查下游
//                     Reduce credit，作为 Xbar 的第五路输入参与仲裁
//
// 下游 Reduce credit 按 UserID 与目标方向记，粒度是一笔 Reduce 任务（F-032、
// F-033）：表项的 reduce_need 置位时，一笔任务头一个 flit 往某个方向发之前，要那
// 个方向这个用户空闲，取得之后这笔任务后面的 flit 不再查。模型里一笔任务就是一个
// reduce_seq 的包。下游把这笔任务的结果整包交出去之后，给每一路上游来源各还一个
// release，写在往那个方向去的出线上；本 core 那一份不还。
//
// 从某个方向进来的 release 按那个口的 Release 静态路由（RouterTable 的
// CreditBypass，对应 RTR_RELEASE_ROUTE）走：带本级位的交给本级，放开那个方向这
// 个用户；带方向位的原样转出去，纯透传的 core 走这一档。
//
// 「全部方向」取自 RouterTable 的 reduce_in_mask：按包头的 path_id 查表，得到
// 这条 path 在本级会有哪几个相邻方向送来分量。首份输入建上下文时把这个集合一并
// 记进用户表，本包收齐前不再重查。
//
// 必须执行 Reduce：SRAM、Bank 或计算单元暂不可用时对输入反压，不允许绕过 Reduce
// 降级为直接存储或转发。
//
// 上下文保护：当前任务的全部输入处理完、结果全部发出之前，同一 User 的下一笔
// 任务不得进来覆盖（F-030）。
//
// 收不下的 flit 留在本路的输入缓冲里，按缓冲余量对 Xbar 反压。收不下有三种：
// 上下文正被同一 User 的另一笔任务占着；这一 User 正锁在另一路的包上；新用户
// 进来时 16 个分区都占着。
//
// 分区：16 个用户各一个 kReduceCtxBytes 的分区，按 FP32 驻留数据量算（F-044）。
// 用户第一笔任务真正进来时分配，后面的任务接着用，收到这个用户的 User Retire 才
// 释放（F-029、F-037）。下游 Reduce 资源映射与本地分区分开记、分开清：Retire 之后
// 分区照放，还有在途任务的方向等 release 回来才清。一个包累加出来超过分区容量就
// 报错：软件要按这个容量把一笔 reduce 拆成链上几项逐级 reduce 任务，一项一包。
//
// 链上没有同步点：上游分量到达时不必等本 core 算完，先存进上下文，本地出核的
// 分量出来时再加。
//
// 数值：输入按 RouterTable 的 reduce_data_type 解，BF16 先扩成 FP32，FP32 直接
// 进；中间累加固定 FP32；输出按 reduce_outdata_type 转成 FP32 或 BF16。
//
// 包最前面 16 B 是软件辅助信息，加法跳过这一段，原样留在输出里。

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/common/flit.h"
#include "bach/common/numeric/accum.h"
#include "bach/common/numeric/formats.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// reduce_data_type / reduce_outdata_type 的取值。原文只说「BF16 或 FP32」，
// 编码按这两档定。
enum ReduceDtype : uint64_t {
  kReduceBf16 = 0,
  kReduceFp32 = 1,
};

// 用户分区数（F-044）。
constexpr uint64_t kReduceCtxNum = 16;
// 每个用户分区的容量，按 FP32 驻留数据量算：Router MAS F-044 每用户 32 KiB，
// 最多 8192 个 FP32。
constexpr uint64_t kReduceCtxBytes = 32 * 1024;
// 每路输入缓冲的 flit 数。取 DATA_NOC HAS 面积预算的每口 32 flit；同一份 HAS 的
// ASM-07 写的是 128 flit，两处没对齐。
constexpr uint64_t kReduceInDepth = 32;

// ReduceModule → TS：一笔 reduce 任务的结果全部交付（F-034）。带 user_id 与这笔
// 任务的 PID，TS 按 user_id 找到那个 stream、完成它的当前任务。reduce_seq 是包头
// 里的任务边界，TS 不用它。
class ReduceDonePort : public Logic {
 public:
  Logic64 valid, user_id, reduce_seq, path_id;

  explicit ReduceDonePort(ClockPtr c)
      : valid(c), user_id(c), reduce_seq(c), path_id(c) {
    Fields(valid, user_id, reduce_seq, path_id);
  }

  void Drive(uint64_t user, uint64_t seq, uint64_t pid = 0) {
    valid = 1;
    user_id = user;
    reduce_seq = seq;
    path_id = pid;
  }
  void Idle() {
    valid = 0;
    user_id = 0;
    reduce_seq = 0;
    path_id = 0;
  }
  bool Valid() const { return valid.Get() != 0; }
};

class ReduceModule : public BachModule {
 public:
  ReduceModule(ClockPtr clock, const std::string& name, RouterTable& table,
               uint64_t copy_idx, uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        rtab(table),
        copy(copy_idx),
        done(std::make_shared<ReduceDonePort>(clock)),
        req(std::make_shared<XbarReqPort>(clock)),
        accepted(clock),
        emitted(clock),
        stalled(clock) {
    for (uint64_t r = 0; r < 3; ++r) {
      in_wire.push_back(std::make_shared<LinkEnd>(clock));
      in_level.push_back(std::make_shared<ReadyLevelPort>(clock));
    }
  }

  // 三路输入，接 Xbar 的 reduce_0 / 1 / 2 出口。
  LinkEnd& In(uint64_t r) { return *in_wire.at(r); }
  void AttachIn(uint64_t r, LinkEndPtr wire) { in_wire.at(r) = std::move(wire); }
  // 结果回注 Xbar 的第五路输入。方向已经由 flow_dir 算好，直接交请求，不再走
  // 一遍 RouterStation 重查表，那样会拿同一个 path_id 判成「又要归约」。
  std::shared_ptr<XbarReqPort> ReqPtr() const { return req; }
  ReduceDonePort& Done() { return *done; }
  std::shared_ptr<ReduceDonePort> DonePtr() const { return done; }
  // 准入电平给 Xbar 读，每路一个：SRAM、Bank 或计算单元暂不可用时对输入反压，
  // 不允许绕过 Reduce 降级为直接存储或转发。判据是本路输入缓冲的余量。
  std::shared_ptr<ReadyLevelPort> LevelPtr(uint64_t r) const {
    return in_level.at(r);
  }
  // 往方向 d 去的出线：还方向 d 那个上游的 Reduce release 写在它的 release 上。
  void AttachUpRelease(uint64_t d, LinkEndPtr wire) { up_rel.at(d) = std::move(wire); }
  // 从方向 d 来的进线：方向 d 那个下游还回来的 Reduce release 从它上面读。
  void AttachDownRelease(uint64_t d, LinkEndPtr wire) {
    down_rel.at(d) = std::move(wire);
  }

  // Retire 从这个口广播退休的 user。表由本模块的协程改，Retire 只送号。
  void AttachRetire(std::shared_ptr<RetireBroadcastPort> p) {
    retire_in = std::move(p);
  }

  // User Retire 只放本地分区（F-037）：这个用户没有在做的任务就当场放，有就等
  // 结果交付完再放。下游 Reduce 资源映射不在这里清，等在途任务的 release 回来。
  void RetireUser(uint64_t user) {
    auto it = ctx.find(user);
    if (it == ctx.end()) return;
    if (it->second.busy) {
      it->second.retired = true;
      return;
    }
    ctx.erase(it);
  }
  // 这个用户占着一个本地分区。
  bool HoldsUser(uint64_t user) const { return ctx.count(user) != 0; }
  uint64_t ContextUsed() const { return ctx.size(); }
  // 方向 dir 的下游那一级正做着本级发过去的这个用户的一笔任务。
  bool DownBusy(uint64_t user, uint64_t dir) const {
    auto it = down.find(user);
    if (it == down.end()) return false;
    return it->second[dir];
  }
  // 方向 dir 的下游做完了这个用户的一笔任务，还回来一个 release。各方向都空了，
  // 这个用户的映射就清掉。
  void ReturnCredit(uint64_t user, uint64_t dir) {
    auto it = down.find(user);
    LOGCHECK(it != down.end() && it->second[dir],
             "ReduceModule: 这个方向没有在途的任务，却收到了 release。");
    it->second[dir] = false;
    if (!it->second[0] && !it->second[1] && !it->second[2]) down.erase(it);
  }

  // 正在做的任务有几笔：从首份输入进来到结果尾 flit 交付。
  uint64_t OutQueued() const { return out_q.size(); }
  uint64_t Accepted() const { return accepted.Get(); }
  uint64_t Emitted() const { return emitted.Get(); }
  uint64_t Stalled() const { return stalled.Get(); }

  bool Quiescent() const override {
    for (auto const& b : in_buf) {
      if (!b.empty()) return false;
    }
    for (auto const& q : rel_q) {
      if (!q.empty()) return false;
    }
    return out_q.empty();
  }

 protected:
  void Step() override {
    TakeRetire();
    TakeDownReleases();
    // 末级先做。
    rel = RelOut{};
    EmitOne();
    PublishReleases();
    for (uint64_t r = 0; r < 3; ++r) {
      TakeIn(r);
      AcceptFrom(r);
    }

    // Xbar 读上一拍的电平：拉高之后路上最多还有两个 flit，一个上一拍已经发出，
    // 一个照着这一拍的电平发，留得出两格才拉高。
    for (uint64_t r = 0; r < 3; ++r) {
      in_level[r]->Drive(in_buf[r].size() + 2 <= kReduceInDepth);
    }
    accepted = accepted_pending;
    emitted = emitted_pending;
    stalled = stalled_pending;
    TracePerCycle("out_q", out_q.size());
    TracePerCycle("ctx", ctx.size());
  }

 private:
  // 收 Retire 的广播。同一笔会连着两拍出现在端口上，按序号认它。
  void TakeRetire() {
    if (!retire_in || !retire_in->Valid()) return;
    if (retire_in->Seq() == last_retire_seq) return;
    last_retire_seq = retire_in->Seq();
    RetireUser(retire_in->User());
  }

  std::shared_ptr<RetireBroadcastPort> retire_in;
  uint64_t last_retire_seq = 0;

  // 一个用户的本地分区，以及它正在做的那一笔任务。
  struct Ctx {
    bool busy = false;               // 有一笔任务在做：首份输入进来到结果尾 flit 交付
    uint64_t path_id = 0;
    uint64_t reduce_seq = 0;
    uint64_t expect_mask = 0;        // 这笔任务开始时记下的 reduce_in_mask
    uint64_t in_done_mask = 0;
    std::array<uint64_t, 3> got{};   // 各路那一份已经收进来的字节
    std::vector<float> acc;          // FP32 中间累加结果
    std::vector<bool> seen;          // 各元素是不是已经写进过第一份
    std::vector<uint8_t> sw_head;    // 首份输入的软件辅助信息，输出时照抄
    uint64_t in_dtype = kReduceBf16, out_dtype = kReduceBf16;
    bool retired = false;            // 收到 Retire，这笔任务交付完就放分区
    uint64_t locked_from = 3;        // 锁定到尾 flit 的那一路，3 = 没锁
    MessagePtr first_msg;            // 首份输入的包头，输出时照抄
  };
  // 一笔在做的任务在输出这一侧的进度。
  struct OutItem {
    uint64_t user_id = 0, reduce_seq = 0;
    MessagePtr msg;             // 结果包，发首 flit 之前按累加结果造
    uint64_t sent = 0;          // 已经发出去的字节
    uint64_t contrib = 0;       // 这笔任务是哪几路来源凑齐的
    uint64_t local_lane = 3;    // 其中本 core 那一份占的那一路
  };
  // 这一拍要还出去的 release：EmitOne 填，PublishReleases 一拍各口只写一次。
  struct RelOut {
    std::array<bool, 3> up{};
    uint64_t user = 0;
  };

  // 这一拍线上到的 flit 先进本路的输入缓冲。
  void TakeIn(uint64_t r) {
    FlitView f = ReadFlit(in_wire[r]->flit);
    if (!f.valid || !f.msg) return;
    LOGCHECK(in_buf[r].size() < kReduceInDepth,
             "ReduceModule: 输入缓冲满了，Xbar 没按准入电平停。");
    in_buf[r].push_back(f);
  }

  // M10 与 M11。收不下的 flit 留在本路输入缓冲的队头，下一拍再试。
  void AcceptFrom(uint64_t r) {
    if (in_buf[r].empty()) return;
    FlitView f = in_buf[r].front();
    uint64_t user = f.msg->user_id;

    RouteEntry const& e = rtab.Lookup(copy, f.msg->path_id);
    // reduce_in_mask 为 0 表示本级不做累加：不派角色的 core 与纯透传的中继核
    // 都是这一档，包按 flow_dir 直接转发，不该进到这里。
    LOGCHECK(e.reduce_in_mask != 0,
             "ReduceModule: 这条 path 在本级 reduce_in_mask 是 0，不该进来。");

    auto it = ctx.find(user);
    if (it == ctx.end()) {
      // 用户第一笔任务真正进来时才分配分区（F-029）。16 个都占着就反压，等有
      // 用户 Retire 放出一个。
      LOGCHECK(f.head, "ReduceModule: 没有分区的用户来的不是首 flit。");
      if (ctx.size() >= kReduceCtxNum) {
        ++stalled_pending;
        return;
      }
      it = ctx.emplace(user, Ctx{}).first;
    }
    Ctx& c = it->second;

    // 进入后锁定当前包直至尾 flit：三路输入不能在包中间换。
    if (c.locked_from != 3 && c.locked_from != r) {
      ++stalled_pending;
      return;
    }

    // 上下文保护：当前任务的结果全部发出之前，同一 User 的下一笔任务不得进来。
    if (c.busy && c.reduce_seq != f.msg->reduce_seq) {
      ++stalled_pending;
      return;
    }
    in_buf[r].pop_front();

    // 按 flit 累加：整包的字节都挂在同一个 Message 上，发送方边读边填，这一 flit
    // 到的时候只有它覆盖的那一段是真的。所以每个 flit 只动它带来的那一段元素，
    // 一个元素只算一遍。
    if (f.head) {
      if (!c.busy) {
        // 首份输入开始这笔任务，把 expect_mask 一并记下，本任务做完前不再重查。
        c.busy = true;
        c.path_id = f.msg->path_id;
        c.reduce_seq = f.msg->reduce_seq;
        c.expect_mask = e.reduce_in_mask;
        c.in_done_mask = 0;
        c.got = {};
        c.in_dtype = e.reduce_data_type;
        c.out_dtype = e.reduce_outdata_type;
        uint64_t n = ElemsOf(f.msg->size, c.in_dtype);
        c.acc.assign(n, 0.0f);
        c.seen.assign(n, false);
        CheckCtxFits(c);
        c.sw_head.assign(f.msg->payload.begin(),
                         f.msg->payload.begin() +
                             std::min<size_t>(kReduceSwHeaderBytes,
                                              f.msg->payload.size()));
        c.first_msg = f.msg;
        out_q.push_back({user, c.reduce_seq, nullptr, 0, c.expect_mask,
                         LocalLaneOf(e)});
      }
      // 这一路一个新包开始，收了多少从包头起算。
      c.got[r] = 0;
    }
    uint64_t lo = c.got[r];
    uint64_t hi = f.tail ? f.msg->size
                         : std::min<uint64_t>(lo + kFlitBytes, f.msg->size);
    AccumRange(c, f.msg->payload, lo, hi);
    c.got[r] = hi;
    c.locked_from = f.tail ? 3 : r;
    ++accepted_pending;
    if (f.tail) c.in_done_mask |= 1ull << r;
  }

  // 结果包里前多少字节已经算好：一个结果元素要等各路对应的那个元素都进来，收齐
  // 的那一路不再限制。有一路一个元素都还没进来就是 0。
  uint64_t ReadyBytes(Ctx const& c, uint64_t out_size) const {
    if (c.in_done_mask == c.expect_mask) return out_size;
    uint64_t in_n = ElemBytes(c.in_dtype), out_n = ElemBytes(c.out_dtype);
    uint64_t elems = ~0ull;
    for (uint64_t r = 0; r < 3; ++r) {
      if (((c.expect_mask >> r) & 1u) == 0) continue;
      if ((c.in_done_mask >> r) & 1u) continue;
      if (c.got[r] < kReduceSwHeaderBytes + in_n) return 0;
      elems = std::min(elems, (c.got[r] - kReduceSwHeaderBytes) / in_n);
    }
    return std::min(out_size, kReduceSwHeaderBytes + elems * out_n);
  }

  // 结果包的长度：软件辅助信息加上按输出精度排的累加结果。
  static uint64_t OutSizeOf(Ctx const& c) {
    return kReduceSwHeaderBytes + c.acc.size() * ElemBytes(c.out_dtype);
  }

  // 一个包的 FP32 累加结果要装得进这个用户的分区。
  static void CheckCtxFits(Ctx const& c) {
    LOGCHECK(c.acc.size() * 4 <= kReduceCtxBytes,
             "ReduceModule: 一个包的 FP32 累加结果超过一个用户的分区 32 KiB。"
             "软件要按上下文容量把一笔 reduce 拆成链上几项逐级 reduce 任务。");
  }

  // 输入的一个元素占几个字节。
  static uint64_t ElemBytes(uint64_t dtype) {
    return dtype == kReduceFp32 ? 4 : 2;
  }

  // 把输入的一个元素解成 FP32：BF16 扩展，FP32 直接进。
  static float ElemOf(std::vector<uint8_t> const& b, uint64_t at,
                      uint64_t dtype) {
    if (dtype == kReduceFp32) {
      uint32_t v = 0;
      for (int k = 0; k < 4; ++k) v |= uint32_t(b[at + k]) << (8 * k);
      return numeric::FloatOf(v);
    }
    uint16_t v = uint16_t(uint16_t(b[at]) | (uint16_t(b[at + 1]) << 8));
    return numeric::FromBf16(v);
  }

  // 一份分量有几个元素：软件辅助信息之后的数据段按输入精度算。
  static uint64_t ElemsOf(uint64_t bytes, uint64_t dtype) {
    if (bytes <= kReduceSwHeaderBytes) return 0;
    return (bytes - kReduceSwHeaderBytes) / ElemBytes(dtype);
  }

  // 一个 flit 带来的字节段 [lo, hi) 里的元素累加进分区：一个元素归结束在这一段
  // 里的那个 flit。第一份写入，后面的读出当前值、加、写回，中间累加固定 FP32。
  // 软件辅助信息那 16 B 跳过。
  static void AccumRange(Ctx& c, std::vector<uint8_t> const& in, uint64_t lo,
                         uint64_t hi) {
    if (hi <= kReduceSwHeaderBytes) return;
    uint64_t n = ElemBytes(c.in_dtype);
    uint64_t from = lo <= kReduceSwHeaderBytes ? 0 : (lo - kReduceSwHeaderBytes) / n;
    uint64_t to = (hi - kReduceSwHeaderBytes) / n;
    for (uint64_t k = from; k < to; ++k) {
      uint64_t at = kReduceSwHeaderBytes + k * n;
      if (at + n > in.size()) break;
      if (k >= c.acc.size()) {
        c.acc.resize(k + 1, 0.0f);
        c.seen.resize(k + 1, false);
        CheckCtxFits(c);
      }
      float v = ElemOf(in, at, c.in_dtype);
      c.acc[k] = c.seen[k] ? numeric::ClampNanInf(c.acc[k] + v) : v;
      c.seen[k] = true;
    }
  }

  // 结果包的字节段 [lo, hi) 按 reduce_outdata_type 从分区里转出来写进去，软件
  // 辅助信息照抄首份输入的那 16 B。发哪个 flit 之前写哪一段。
  static void PackRange(Ctx const& c, Message& m, uint64_t lo, uint64_t hi) {
    for (uint64_t i = lo; i < hi && i < kReduceSwHeaderBytes; ++i) {
      m.payload[i] = i < c.sw_head.size() ? c.sw_head[i] : 0;
    }
    if (hi <= kReduceSwHeaderBytes) return;
    uint64_t n = ElemBytes(c.out_dtype);
    uint64_t from = lo <= kReduceSwHeaderBytes ? 0 : (lo - kReduceSwHeaderBytes) / n;
    for (uint64_t k = from; k < c.acc.size(); ++k) {
      uint64_t at = kReduceSwHeaderBytes + k * n;
      if (at >= hi) break;
      if (c.out_dtype == kReduceFp32) {
        uint32_t v = numeric::BitsOf(c.acc[k]);
        for (uint64_t j = 0; j < 4; ++j) {
          m.payload[at + j] = uint8_t((v >> (8 * j)) & 0xFF);
        }
      } else {
        uint16_t v = numeric::ToBf16(c.acc[k]);
        m.payload[at] = uint8_t(v & 0xFF);
        m.payload[at + 1] = uint8_t((v >> 8) & 0xFF);
      }
    }
  }

  // M12：结果按 flit 发。一个包发出首 flit 后锁定到尾 flit，它在队头；后面的
  // 结果没好就占着输出等，不换别的包。
  void EmitOne() {
    // 与各 RouterStation 同一套：Xbar 那一侧上一拍说收得下就发，发了当场出队。
    if (!req->Room() || out_q.empty()) {
      req->IdleReq();
      done->Idle();
      return;
    }
    if (out_q.front().sent == 0 && !PickNext()) {
      req->IdleReq();
      done->Idle();
      return;
    }
    OutItem& it = out_q.front();
    auto cit = ctx.find(it.user_id);
    LOGCHECK(cit != ctx.end(), "ReduceModule: 输出时分区已经没了。");
    Ctx& c = cit->second;

    uint64_t left = it.msg->size - it.sent;
    uint64_t bytes = std::min(left, kFlitBytes);
    uint64_t ready = ReadyBytes(c, it.msg->size);
    if (ready < it.sent + bytes) {
      req->IdleReq();
      done->Idle();
      return;
    }

    // 往哪几个方向发由这条 path 的 flow_dir 定。
    RouteEntry const& e = rtab.Lookup(copy, it.msg->path_id);
    uint64_t mask = 0;
    for (uint64_t d : DownDirsOf(e.flow_dir)) {
      mask |= 1ull << (d == 0 ? kOutMid : (d == 1 ? kOutLeft : kOutRight));
    }
    // 末端汇聚核 flow_dir 全不置位，结果只交给本 core。
    if (mask == 0 || e.EntersCore(it.msg->path_core_mask)) {
      mask |= 1ull << kOutCore;
    }
    bool head = it.sent == 0;
    bool tail = bytes == left;
    PackRange(c, *it.msg, it.sent, it.sent + bytes);
    req->valid = 1;
    req->out_mask = mask;
    req->vc = kReduceVc;
    req->head = head ? 1 : 0;
    req->tail = tail ? 1 : 0;
    req->bytes = bytes;
    req->path_id = it.msg->path_id;
    req->user_id = it.user_id;
    req->enters_core = ((mask >> kOutCore) & 1u) ? 1 : 0;
    req->stall_way = 0;
    req->need_stream = 0;
    // 整个结果都算好了才算手里攥着整包。
    req->whole_packet = ready >= it.msg->size ? 1 : 0;
    req->credit_require = 0;
    req->msg = it.msg;
    req->seq = ++req_seq;
    it.sent += bytes;
    if (!tail) {
      done->Idle();
      return;
    }
    // 结果全部交付后向 core 返回 UserID 与包头里的 reduce_seq（F-034），同时给
    // 上游还 release（F-033）。这笔任务做完，分区空出来给这个用户的下一笔；收到
    // 过 Retire 的就放掉分区。
    done->Drive(it.user_id, it.reduce_seq, it.msg->path_id);
    ReleaseSources(it);
    uint64_t user = it.user_id;
    out_q.pop_front();
    c.busy = false;
    c.in_done_mask = 0;
    c.got = {};
    c.locked_from = 3;
    if (c.retired) ctx.erase(user);
    ++emitted_pending;
  }

  // 队头还没开始发时，挑最早一笔首 flit 已经算好、下游也收得下的任务挪到队头。
  // 一笔任务头一个 flit 发出去之前，表项的 reduce_need 置位就要取得各目标方向
  // 这个用户的下游准入；后面的 flit 只受 VC credit 约束。
  bool PickNext() {
    for (auto i = out_q.begin(); i != out_q.end(); ++i) {
      Ctx const& c = ctx.at(i->user_id);
      uint64_t size = OutSizeOf(c);
      if (ReadyBytes(c, size) < std::min(size, kFlitBytes)) continue;
      RouteEntry const& e = rtab.Lookup(copy, c.path_id);
      std::vector<uint64_t> dirs = DownDirsOf(e.flow_dir);
      if (e.reduce_need) {
        // 多播全有全无：任一方向这个用户还忙着就先不发这一笔。
        bool free = true;
        for (uint64_t d : dirs) free = free && !DownBusy(i->user_id, d);
        if (!free) {
          ++stalled_pending;
          continue;
        }
        for (uint64_t d : dirs) down[i->user_id][d] = true;
      }
      // 结果包的包头照抄首份输入，数据按 flit 填：发哪个 flit 之前写哪一段。
      auto m = std::make_shared<Message>(*c.first_msg);
      m->payload.assign(size, 0);
      m->size = size;
      OutItem item = *i;
      item.msg = m;
      out_q.erase(i);
      out_q.push_front(item);
      return true;
    }
    return false;
  }

  // 本 core 那一份占哪一路（F113）：flow_dir 的 reduce2 置位走 2，reduce1 置位
  // 走 1，都不置走 0。其余几路是同号方向的相邻上游：0 mid、1 left、2 right。
  // 这个判断默认本 core 出了自己那一份。
  static uint64_t LocalLaneOf(RouteEntry const& e) {
    if (e.flow_dir & kFlowReduce2) return 2;
    if (e.flow_dir & kFlowReduce1) return 1;
    return 0;
  }

  // 这笔任务的结果整包交出去了：给每一路上游来源各还一个 release，本 core 那一份
  // 不还。
  void ReleaseSources(OutItem const& it) {
    rel.user = it.user_id;
    for (uint64_t r = 0; r < 3; ++r) {
      if (((it.contrib >> r) & 1u) == 0 || r == it.local_lane) continue;
      rel.up[r] = true;
    }
  }

  // 每个方向一拍送一个：本级自己的与转过来的排在同一个队列里。
  void PublishReleases() {
    for (uint64_t d = 0; d < 3; ++d) {
      if (rel.up[d]) rel_q[d].push_back(rel.user);
      if (!up_rel[d]) {
        rel_q[d].clear();
        continue;
      }
      if (rel_q[d].empty()) {
        up_rel[d]->release.DriveReduce(false, 0);
        continue;
      }
      up_rel[d]->release.DriveReduce(true, rel_q[d].front());
      rel_q[d].pop_front();
    }
  }

  // 从方向 d 进来的 Reduce release 按那个口的 Release 静态路由走（F-018～
  // F-020）：带本级位的交给本级，带方向位的原样转出去，UserID 不变。
  void TakeDownReleases() {
    for (uint64_t d = 0; d < 3; ++d) {
      if (!down_rel[d]) continue;
      ReleaseView r = ReadRelease(down_rel[d]->release);
      if (!r.reduce_valid) continue;
      uint64_t route = rtab.CreditBypass(d);
      LOGCHECK(route != 0,
               "ReduceModule: 这个口收到了 Reduce release，却没配 Release 路由。");
      for (uint64_t o = 0; o < 3; ++o) {
        if ((route >> o) & 1u) rel_q[o].push_back(r.reduce_user);
      }
      if ((route & kReleaseSelf) == 0) continue;
      if (!DownBusy(r.reduce_user, d)) {
        spdlog::error("ReduceModule 第 {} 拍从方向 {} 收到用户 {} 的 Reduce release",
                      CycleNow(), d, r.reduce_user);
      }
      ReturnCredit(r.reduce_user, d);
    }
  }

  // flow_dir 的 R2R 三位映射到 down_busy 的下标。末端汇聚核 flow_dir 全不置位，
  // 结果只交给本 core，不占任何方向。
  static std::vector<uint64_t> DownDirsOf(uint64_t flow_dir) {
    std::vector<uint64_t> dirs;
    if (flow_dir & kFlowMid) dirs.push_back(0);
    if (flow_dir & kFlowLeft) dirs.push_back(1);
    if (flow_dir & kFlowRight) dirs.push_back(2);
    return dirs;
  }

  RouterTable& rtab;
  uint64_t copy;
  std::vector<LinkEndPtr> in_wire;
  std::shared_ptr<XbarReqPort> req;
  std::shared_ptr<ReduceDonePort> done;
  std::vector<std::shared_ptr<ReadyLevelPort>> in_level;
  std::array<LinkEndPtr, 3> up_rel{}, down_rel{};

  // Step 独占。
  RelOut rel;
  std::array<std::deque<uint64_t>, 3> rel_q;
  std::array<std::deque<FlitView>, 3> in_buf;
  std::map<uint64_t, Ctx> ctx;
  // 相邻下游 Reduce 资源映射：各方向的下游是否正做着本级发过去的这个用户的一笔
  // 任务。与本地分区分开记、分开清（F-037）。
  std::map<uint64_t, std::array<bool, 3>> down;
  std::deque<OutItem> out_q;
  uint64_t req_seq = 0;
  uint64_t accepted_pending = 0, emitted_pending = 0, stalled_pending = 0;

  Logic64 accepted, emitted, stalled;
};

}  // namespace bach
}  // namespace latch

#endif
