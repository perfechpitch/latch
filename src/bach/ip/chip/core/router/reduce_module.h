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
//                     等。发送前只查目标 VC credit，作为 Xbar 的第五路输入参与
//                     仲裁
//
// 往下游发 reduce 结果不查下游的 Rmem：Rmem 与 Core Mem 按同样的办法给 16 个
// 用户等分，一个用户在下游拿到的那一项 Stream 资源同时代表那边的 Core Mem 与
// Rmem 容量，所以 reduce 这一路不单独记账、也不单独还，业务级资源只有 Stream
// 那一套，用户任务链跑完退休时还一次。
//
// “全部方向”取自 RouterTable 的 reduce_in_mask：按包头的 path_id 查表，得到
// 这条 path 在本级会有哪几个相邻方向送来分量。首份输入建上下文时把这个集合一并
// 记进用户表，本包收齐前不再重查。
//
// 必须执行 Reduce：SRAM、Bank 或计算单元暂不可用时对输入反压，不允许绕过 Reduce
// 降级为直接存储或转发。
//
// 上下文保护：当前任务的全部输入处理完、结果全部发出之前，同一 User 的下一笔
// 任务不得进来覆盖（F-030）。各核的 task id 各自编号，包头里的 reduce_seq 不
// 参与配对：同一 user、同一 path 上还没到的那一路就是这一笔的输入。这一路已经
// 收过、或者 path 不同，才是下一笔，留在队头等到这一笔做完。
//
// 收不下的 flit 留在本路的输入缓冲里，按缓冲余量对 Xbar 反压。收不下有两种：
// 这一 User 正锁在另一路的包上；新用户进来时 16 个分区都占着。
// 已经进到输入缓冲的下一笔（另一条 path，或这一路已经收过）留在该路队头，
// 不写进当前分区。Xbar 上还没准入的下一笔留在 Xbar 那个 VC 的队头，不进这里。
//
// 分区：16 个用户各一个 kReduceCtxBytes 的分区，按 FP32 驻留数据量算（F-044）。
// 用户第一笔任务真正进来时分配，后面的任务接着用，收到这个用户的 User Retire 才
// 释放（F-029、F-037）。一个包累加出来超过分区容量就报错：软件要按这个容量把一笔
// reduce 拆成链上几项逐级 reduce 任务，一项一包。
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
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "bach/common/flit.h"
#include "bach/common/numeric/accum.h"
#include "bach/common/numeric/formats.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/module_base.h"
#include "base/log.h"

namespace latch {
namespace bach {

// reduce_data_type / reduce_outdata_type 的取值。原文只说“BF16 或 FP32”，
// 编码按这两档定。
enum ReduceDtype : uint64_t {
  kReduceBf16 = 0,
  kReduceFp32 = 1,
  // bit8 置位时 bit0..2 按 mid/left/right 标哪一路是 FP32，其余是 BF16。
  kReduceDtypePerLane = 1u << 8,
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
// 任务的 PID，TS 按 user_id 找到那个 stream、完成它的当前任务。reduce_seq
// 是包头 里的任务边界，TS 不用它。
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
  ReduceModule(ClockPtr clock, const std::string &name, RouterTable &table,
               uint64_t copy_idx, uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick), rtab(table), copy(copy_idx),
        done(std::make_shared<ReduceDonePort>(clock)),
        req(std::make_shared<XbarReqPort>(clock)), accepted(clock),
        emitted(clock), stalled(clock) {
    for (uint64_t r = 0; r < 3; ++r) {
      in_wire.push_back(std::make_shared<LinkEnd>(clock));
      in_level.push_back(std::make_shared<ReadyLevelPort>(clock));
    }
  }

  // 三路输入，接 Xbar 的 reduce_0 / 1 / 2 出口。
  LinkEnd &In(uint64_t r) { return *in_wire.at(r); }
  void AttachIn(uint64_t r, LinkEndPtr wire) {
    in_wire.at(r) = std::move(wire);
  }
  // 结果回注 Xbar 的第五路输入。方向已经由 flow_dir 算好，直接交请求，不再走
  // 一遍 RouterStation 重查表，那样会拿同一个 path_id 判成“又要归约”。
  std::shared_ptr<XbarReqPort> ReqPtr() const { return req; }
  ReduceDonePort &Done() { return *done; }
  std::shared_ptr<ReduceDonePort> DonePtr() const { return done; }
  // 准入电平给 Xbar 读，每路一个：SRAM、Bank 或计算单元暂不可用时对输入反压，
  // 不允许绕过 Reduce 降级为直接存储或转发。判据是本路输入缓冲的余量。
  std::shared_ptr<ReadyLevelPort> LevelPtr(uint64_t r) const {
    return in_level.at(r);
  }
  // Retire 从这个口广播退休的 user。表由本模块的协程改，Retire 只送号。
  void AttachRetire(std::shared_ptr<RetireBroadcastPort> p) {
    retire_in = std::move(p);
  }

  void AttachSlotReuse(std::shared_ptr<RetireBroadcastPort> port) {
    slot_reuse = std::move(port);
  }

  // User Retire 放本地分区（F-037）：这个用户没有在做的任务就当场放，有就等结果
  // 交付完再放。
  void RetireUser(uint64_t user) {
    auto it = ctx.find(user);
    if (it == ctx.end())
      return;
    if (it->second.busy || it->second.reserved) {
      it->second.retired = true;
      return;
    }
    ctx.erase(it);
  }
  // 这个用户占着一个本地分区。
  bool HoldsUser(uint64_t user) const { return ctx.count(user) != 0; }
  void SetHeadNotify(std::function<void(uint64_t, uint64_t)> fn) {
    head_notify = std::move(fn);
  }
  uint64_t ContextUsed() const { return ctx.size(); }

  bool CanAcceptHead(Message const &m) const {
    uint64_t spec = rtab.Lookup(copy, m.path_id).reduce_data_type;
    uint64_t bytes = AccElems(m.size, spec, m.payload_aux) * 4;
    LOGCHECK(bytes <= kReduceCtxBytes, "ReduceModule: 超过 32 KiB。");
    auto it = ctx.find(m.user_id);
    if (it == ctx.end()) {
      bool ok = ctx.size() < kReduceCtxNum;
      if (!ok) {
        deny_why = 1;
        deny_user = m.user_id;
        deny_path = m.path_id;
      }
      return ok;
    }
    auto const &c = it->second;
    // 下一笔是另一条 path。同一 path 上别的核 task id 不同，reduce_seq 不作为
    // 拒绝条件：这一笔还缺的那一路要能进来。
    if (c.busy || c.reserved) {
      bool ok = c.path_id == m.path_id;
      if (!ok) {
        deny_why = 2;
        deny_user = m.user_id;
        deny_path = m.path_id;
        deny_held = c.path_id;
      }
      return ok;
    }
    if (c.retired) {
      deny_why = 3;
      deny_user = m.user_id;
      deny_path = m.path_id;
      deny_held = c.path_id;
    }
    return !c.retired;
  }
  void ReserveHead(Message const &m) {
    LOGCHECK(CanAcceptHead(m), "ReduceModule: 未获准入。");
    auto &c = ctx[m.user_id];
    if (c.busy || c.reserved)
      return;
    c.reserved = true;
    c.reduce_seq = m.reduce_seq;
    c.path_id = m.path_id;
  }

  // 正在做的任务有几笔：从首份输入进来到结果尾 flit 交付。
  uint64_t OutQueued() const { return out_q.size(); }
  bool OutFront(uint64_t &user, uint64_t &sent, uint64_t &size) const {
    if (out_q.empty() || !out_q.front().msg)
      return false;
    user = out_q.front().user_id;
    sent = out_q.front().sent;
    size = out_q.front().msg->size;
    return true;
  }
  uint64_t Accepted() const { return accepted.Get(); }
  uint64_t Emitted() const { return emitted.Get(); }
  uint64_t Stalled() const { return stalled.Get(); }

  struct TaskState {
    uint64_t user_id, path_id, reduce_seq, expect_mask, in_done_mask;
    bool busy = false, reserved = false;
  };
  struct State {
    std::vector<TaskState> tasks;
    std::array<FlitView, 3> input_heads{};
    std::array<uint64_t, 3> input_depth{};
    int deny_why = 0;
    uint64_t deny_user = 0, deny_path = 0, deny_held = 0;
  };
  // 停钟并 JoinAll 后查看未完成任务与输入队头；运行中的其他协程不能读这些容器。
  State Inspect() const {
    State s;
    s.deny_why = deny_why;
    s.deny_user = deny_user;
    s.deny_path = deny_path;
    s.deny_held = deny_held;
    for (auto const &item : ctx) {
      Ctx const &c = item.second;
      if (!c.busy && !c.reserved)
        continue;
      s.tasks.push_back({item.first, c.path_id, c.reduce_seq, c.expect_mask,
                         c.in_done_mask, c.busy, c.reserved});
    }
    for (uint64_t r = 0; r < 3; ++r) {
      s.input_depth[r] = in_buf[r].size();
      if (!in_buf[r].empty())
        s.input_heads[r] = in_buf[r].front();
    }
    return s;
  }

  bool Quiescent() const override {
    for (auto const &b : in_buf) {
      if (!b.empty())
        return false;
    }
    return out_q.empty();
  }

protected:
  void Step() override {
    TakeRetire();
    // 末级先做。
    EmitOne();
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
    if (slot_reuse && slot_reuse->Valid() &&
        slot_reuse->Seq() != last_reuse_seq) {
      last_reuse_seq = slot_reuse->Seq();
      RetireUser(slot_reuse->User());
    }
    if (!retire_in || !retire_in->Valid())
      return;
    if (retire_in->Seq() == last_retire_seq)
      return;
    last_retire_seq = retire_in->Seq();
    RetireUser(retire_in->User());
  }

  std::shared_ptr<RetireBroadcastPort> retire_in;
  uint64_t last_retire_seq = 0, last_reuse_seq = 0;
  std::shared_ptr<RetireBroadcastPort> slot_reuse;

  // 一个用户的本地分区，以及它正在做的那一笔任务。
  struct Ctx {
    bool reserved = false;
    bool busy = false; // 有一笔任务在做：首份输入进来到结果尾 flit 交付
    uint64_t path_id = 0;
    uint64_t reduce_seq = 0;
    uint64_t expect_mask = 0; // 这笔任务开始时记下的 reduce_in_mask
    uint64_t in_done_mask = 0;
    std::array<uint64_t, 3> got{}; // 各路那一份已经收进来的字节
    std::vector<float> acc;        // FP32 中间累加结果
    std::vector<bool> seen;        // 各元素是不是已经写进过第一份
    std::vector<uint8_t> sw_head;  // 首份输入的软件辅助信息，输出时照抄
    uint64_t aux = kReduceSwHeaderBytes;
    uint64_t in_dtype = kReduceBf16, out_dtype = kReduceBf16;
    bool retired = false;     // 收到 Retire，这笔任务交付完就放分区
    uint64_t locked_from = 3; // 锁定到尾 flit 的那一路，3 = 没锁
    MessagePtr first_msg;     // 首份输入的包头，输出时照抄
  };
  // 一笔在做的任务在输出这一侧的进度。
  struct OutItem {
    uint64_t user_id = 0, reduce_seq = 0;
    MessagePtr msg;    // 结果包，发首 flit 之前按累加结果造
    uint64_t sent = 0; // 已经发出去的字节
  };

  // 逐 flit 的入口/出口观测：Perfetto 的 reduce 入口/出口轨道按它折「收包头 → 收完
  // 包尾」/「写包头 → 写完包尾」。side 0..2 是三路入口，与 reduce_in_mask 三位同一套
  // 编号（bit0 mid、bit1 left、bit2 right），3 是出口 out。挂在
  // chip.core.router.reduce 下。
  void TraceReduceFlit(uint64_t side, uint64_t head, uint64_t tail,
                       uint64_t user, uint64_t path, uint64_t size) {
    static const char *kName[4][5] = {
        {"mid_flit_head", "mid_flit_tail", "mid_flit_user", "mid_flit_path",
         "mid_flit_size"},
        {"left_flit_head", "left_flit_tail", "left_flit_user", "left_flit_path",
         "left_flit_size"},
        {"right_flit_head", "right_flit_tail", "right_flit_user",
         "right_flit_path", "right_flit_size"},
        {"out_flit_head", "out_flit_tail", "out_flit_user", "out_flit_path",
         "out_flit_size"},
    };
    uint64_t v[5] = {head, tail, user, path, size};
    for (uint64_t k = 0; k < 5; ++k)
      TracePerCycle(kName[side][k], v[k]);
  }

  // 这一拍线上到的 flit 先进本路的输入缓冲。
  void TakeIn(uint64_t r) {
    FlitView f = ReadFlit(in_wire[r]->flit);
    // 入口逐 flit 观测：无效拍全 0，保证 TracePerCycle 逐拍提交、脉冲不粘连。side = r
    // 就是三路方向。flit_size 是包 size（数据通道字节数，含 topK），挂在 Message 上。
    TraceReduceFlit(r, f.valid && f.head ? 1u : 0u, f.valid && f.tail ? 1u : 0u,
                    f.valid && f.msg ? f.msg->user_id : 0u,
                    f.valid && f.msg ? f.msg->path_id : 0u,
                    f.valid && f.msg ? f.msg->size : 0u);
    if (!f.valid || !f.msg)
      return;
    LOGCHECK(in_buf[r].size() < kReduceInDepth,
             "ReduceModule: 输入缓冲满了，Xbar 没按准入电平停。");
    in_buf[r].push_back(f);
  }

  // M10 与 M11。收不下的 flit 留在本路输入缓冲的队头，下一拍再试。
  void AcceptFrom(uint64_t r) {
    if (in_buf[r].empty())
      return;
    FlitView f = in_buf[r].front();
    uint64_t user = f.msg->user_id;

    RouteEntry const &e = rtab.Lookup(copy, f.msg->path_id);
    // reduce_in_mask 为 0 表示本级不做累加：只转发的 core 是这一档，包按
    // flow_dir 直接转发，不该进到这里。坏 core 上的包一律不进 ReduceModule。
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
    Ctx &c = it->second;

    // 另一条 path，或这一路已经收过一份，是下一笔。同一 path 上 reduce_seq
    // 不同的另一路是这一笔还缺的输入，写进当前分区。
    bool lane_done = f.head && ((c.in_done_mask >> r) & 1ull) != 0;
    bool foreign =
        (c.busy || c.reserved) && (f.msg->path_id != c.path_id || lane_done);
    if (foreign) {
      // 留在本路队头。收进 park 会把这一路的反压放掉，后面的包整段涌进来。
      ++stalled_pending;
      return;
    }

    // 进入后锁定当前包直至尾 flit：三路输入不能在包中间换。
    if (c.locked_from != 3 && c.locked_from != r) {
      ++stalled_pending;
      return;
    }
    in_buf[r].pop_front();
    Apply(c, user, r, f);
  }

  // 一拍 flit 写进这个用户的当前分区。调用时这一笔已经确定属于当前任务。
  void Apply(Ctx &c, uint64_t user, uint64_t r, FlitView const &f) {
    RouteEntry const &e = rtab.Lookup(copy, f.msg->path_id);
    // 包不进 DTE 时，跳过同 path 的 P2P reissue。进核的包仍由 CoreStation 的
    // 正常 trigger 派搬入，这里再发一笔会把还没到的包头提前弹掉。
    if (f.head && head_notify && !e.EntersCore(f.msg->path_core_mask)) {
      head_notify(user, f.msg->path_id);
    }
    if (f.head) {
      if (!c.busy) {
        // 首份输入开始这笔任务，把 expect_mask 一并记下，本任务做完前不再重查。
        c.busy = true;
        c.reserved = false;
        c.path_id = f.msg->path_id;
        c.reduce_seq = f.msg->reduce_seq;
        c.expect_mask = e.reduce_in_mask;
        c.in_done_mask = 0;
        c.got = {};
        c.in_dtype = e.reduce_data_type;
        c.out_dtype = e.reduce_outdata_type;
        c.aux = f.msg->payload_aux;
        uint64_t n = ElemsOf(f.msg->size, LaneDtype(c.in_dtype, r), c.aux);
        c.acc.assign(n, 0.0f);
        c.seen.assign(n, false);
        CheckCtxFits(c);
        c.sw_head.assign(f.msg->payload.begin(),
                         f.msg->payload.begin() +
                             std::min<size_t>(c.aux, f.msg->payload.size()));
        c.first_msg = f.msg;
        out_q.push_back({user, c.reduce_seq, nullptr, 0});
      }
      // 这一路一个新包开始，收了多少从包头起算。
      c.got[r] = 0;
    }
    uint64_t lo = c.got[r];
    uint64_t hi =
        f.tail ? f.msg->size : std::min<uint64_t>(lo + kFlitBytes, f.msg->size);
    AccumRange(c, f.msg->payload, lo, hi, LaneDtype(c.in_dtype, r));
    c.got[r] = hi;
    c.locked_from = f.tail ? 3 : r;
    ++accepted_pending;
    if (f.tail)
      c.in_done_mask |= 1ull << r;
  }

  // 结果包里前多少字节已经算好：一个结果元素要等各路对应的那个元素都进来，收齐
  // 的那一路不再限制。有一路一个元素都还没进来就是 0。
  uint64_t ReadyBytes(Ctx const &c, uint64_t out_size) const {
    if (c.in_done_mask == c.expect_mask)
      return out_size;
    uint64_t out_n = ElemBytes(c.out_dtype);
    uint64_t elems = ~0ull;
    for (uint64_t r = 0; r < 3; ++r) {
      if (((c.expect_mask >> r) & 1u) == 0)
        continue;
      if ((c.in_done_mask >> r) & 1u)
        continue;
      uint64_t in_n = ElemBytes(LaneDtype(c.in_dtype, r));
      if (c.got[r] < c.aux + in_n)
        return 0;
      elems = std::min(elems, (c.got[r] - c.aux) / in_n);
    }
    return std::min(out_size, c.aux + elems * out_n);
  }

  // 结果包的长度：软件辅助信息加上按输出精度排的累加结果。
  static uint64_t OutSizeOf(Ctx const &c) {
    return c.aux + c.acc.size() * ElemBytes(c.out_dtype);
  }

  // 一个包的 FP32 累加结果要装得进这个用户的分区。
  static void CheckCtxFits(Ctx const &c) {
    LOGCHECK(c.acc.size() * 4 <= kReduceCtxBytes,
             "ReduceModule: 一个包的 FP32 累加结果超过一个用户的分区 32 KiB。"
             "软件要按上下文容量把一笔 reduce 拆成链上几项逐级 reduce 任务。");
  }

  // 输入的一个元素占几个字节。
  static uint64_t ElemBytes(uint64_t dtype) {
    return dtype == kReduceFp32 ? 4 : 2;
  }
  static uint64_t LaneDtype(uint64_t spec, uint64_t lane) {
    if ((spec & kReduceDtypePerLane) == 0) {
      return spec == kReduceFp32 ? kReduceFp32 : kReduceBf16;
    }
    return ((spec >> lane) & 1u) ? kReduceFp32 : kReduceBf16;
  }
  static uint64_t AccElems(uint64_t bytes, uint64_t spec, uint64_t aux) {
    uint64_t elem = 4;
    if ((spec & kReduceDtypePerLane) == 0) {
      elem = ElemBytes(spec);
    } else {
      bool bf16 = false;
      for (uint64_t r = 0; r < 3; ++r) {
        if (((spec >> r) & 1u) == 0)
          bf16 = true;
      }
      elem = bf16 ? 2 : 4;
    }
    if (bytes <= aux)
      return 0;
    return (bytes - aux) / elem;
  }

  // 把输入的一个元素解成 FP32：BF16 扩展，FP32 直接进。
  static float ElemOf(std::vector<uint8_t> const &b, uint64_t at,
                      uint64_t dtype) {
    if (dtype == kReduceFp32) {
      uint32_t v = 0;
      for (int k = 0; k < 4; ++k)
        v |= uint32_t(b[at + k]) << (8 * k);
      return numeric::FloatOf(v);
    }
    uint16_t v = uint16_t(uint16_t(b[at]) | (uint16_t(b[at + 1]) << 8));
    return numeric::FromBf16(v);
  }

  // 一份分量有几个元素：软件辅助信息之后的数据段按输入精度算。
  static uint64_t ElemsOf(uint64_t bytes, uint64_t dtype, uint64_t aux) {
    if (bytes <= aux)
      return 0;
    return (bytes - aux) / ElemBytes(dtype);
  }

  static void AccumRange(Ctx &c, std::vector<uint8_t> const &in, uint64_t lo,
                         uint64_t hi, uint64_t dtype) {
    if (hi <= c.aux)
      return;
    uint64_t n = ElemBytes(dtype);
    uint64_t from = lo <= c.aux ? 0 : (lo - c.aux) / n;
    uint64_t to = (hi - c.aux) / n;
    for (uint64_t k = from; k < to; ++k) {
      uint64_t at = c.aux + k * n;
      if (at + n > in.size())
        break;
      if (k >= c.acc.size()) {
        c.acc.resize(k + 1, 0.0f);
        c.seen.resize(k + 1, false);
        CheckCtxFits(c);
      }
      float v = ElemOf(in, at, dtype);
      c.acc[k] = c.seen[k] ? numeric::ClampNanInf(c.acc[k] + v) : v;
      c.seen[k] = true;
    }
  }

  // 结果包的字节段 [lo, hi) 按 reduce_outdata_type 从分区里转出来写进去，软件
  // 辅助信息照抄首份输入的那 16 B。发哪个 flit 之前写哪一段。
  static void PackRange(Ctx const &c, Message &m, uint64_t lo, uint64_t hi) {
    for (uint64_t i = lo; i < hi && i < c.aux; ++i) {
      m.payload[i] = i < c.sw_head.size() ? c.sw_head[i] : 0;
    }
    if (hi <= c.aux)
      return;
    uint64_t n = ElemBytes(c.out_dtype);
    uint64_t from = lo <= c.aux ? 0 : (lo - c.aux) / n;
    for (uint64_t k = from; k < c.acc.size(); ++k) {
      uint64_t at = c.aux + k * n;
      if (at >= hi)
        break;
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
    // 出口逐 flit 观测：本拍没发的先记 0，发出去了在下面覆写成本拍的真实值，保证
    // TracePerCycle 逐拍提交、脉冲不粘连。
    TraceReduceFlit(3, 0, 0, 0, 0, 0);
    if (!req->RoomFor(kReduceVc) || out_q.empty()) {
      req->IdleReq();
      done->Idle();
      return;
    }
    if (out_q.front().sent == 0 && !PickNext()) {
      req->IdleReq();
      done->Idle();
      return;
    }
    OutItem &it = out_q.front();
    auto cit = ctx.find(it.user_id);
    LOGCHECK(cit != ctx.end(), "ReduceModule: 输出时分区已经没了。");
    Ctx &c = cit->second;

    uint64_t left = it.msg->size - it.sent;
    uint64_t bytes = std::min(left, kFlitBytes);
    uint64_t ready = ReadyBytes(c, it.msg->size);
    if (ready < it.sent + bytes) {
      req->IdleReq();
      done->Idle();
      return;
    }

    // 往哪几个方向发由这条 path 的 flow_dir 定。
    RouteEntry const &e = rtab.Lookup(copy, it.msg->path_id);
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
    req->stream_need = 0;
    // 整个结果都算好了才算手里攥着整包。
    req->whole_packet = ready >= it.msg->size ? 1 : 0;
    req->credit_require = 0;
    req->msg = it.msg;
    req->seq = ++req_seq;
    it.sent += bytes;
    // 这一拍真的发出了一笔结果 flit，覆写顶部那组 0：head/tail/user/path/size。
    TraceReduceFlit(3, head ? 1u : 0u, tail ? 1u : 0u, it.user_id,
                    it.msg->path_id, it.msg->size);
    if (!tail) {
      done->Idle();
      return;
    }
    // 结果全部交付后向 core 返回 UserID 与包头里的
    // reduce_seq（F-034）。这笔任务 做完，分区空出来给这个用户的下一笔；收到过
    // Retire 的就放掉分区。
    done->Drive(it.user_id, it.reduce_seq, it.msg->path_id);
    uint64_t user = it.user_id;
    out_q.pop_front();
    c.busy = false;
    c.in_done_mask = 0;
    c.got = {};
    c.locked_from = 3;
    if (c.retired)
      ctx.erase(user);
    ++emitted_pending;
  }

  // 队头还没开始发时，挑最早一笔首 flit 已经算好的任务挪到队头。
  bool PickNext() {
    for (auto i = out_q.begin(); i != out_q.end(); ++i) {
      Ctx const &c = ctx.at(i->user_id);
      uint64_t size = OutSizeOf(c);
      if (ReadyBytes(c, size) < std::min(size, kFlitBytes))
        continue;
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

  // flow_dir 的 R2R 三位映射到方向号。末端汇聚核 flow_dir 全不置位，结果只交给
  // 本 core。
  static std::vector<uint64_t> DownDirsOf(uint64_t flow_dir) {
    std::vector<uint64_t> dirs;
    if (flow_dir & kFlowMid)
      dirs.push_back(0);
    if (flow_dir & kFlowLeft)
      dirs.push_back(1);
    if (flow_dir & kFlowRight)
      dirs.push_back(2);
    return dirs;
  }

  RouterTable &rtab;
  uint64_t copy;
  std::vector<LinkEndPtr> in_wire;
  std::shared_ptr<XbarReqPort> req;
  std::shared_ptr<ReduceDonePort> done;
  std::vector<std::shared_ptr<ReadyLevelPort>> in_level;

  // Step 独占。
  std::array<std::deque<FlitView>, 3> in_buf;
  std::function<void(uint64_t, uint64_t)> head_notify;
  mutable int deny_why = 0;
  mutable uint64_t deny_user = 0, deny_path = 0, deny_held = 0;
  std::map<uint64_t, Ctx> ctx;
  std::deque<OutItem> out_q;
  uint64_t req_seq = 0;
  uint64_t accepted_pending = 0, emitted_pending = 0, stalled_pending = 0;

  Logic64 accepted, emitted, stalled;
};

} // namespace bach
} // namespace latch

#endif
