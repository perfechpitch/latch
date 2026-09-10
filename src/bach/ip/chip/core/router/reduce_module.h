#ifndef _LATCH_BACH_IP_CHIP_CORE_ROUTER_REDUCE_MODULE_
#define _LATCH_BACH_IP_CHIP_CORE_ROUTER_REDUCE_MODULE_

// ReduceModule：Router 里做逐级归约的那一块。
//
// 三级：
//   M10 三路输入仲裁  仲裁 SRAM、Bank 与计算资源；进入后锁定当前包直至尾 flit
//   M11 RMW 累加      同一 User 同一包的第一份输入分配上下文并写入，后续方向的
//                     输入读出当前值、累加、写回，原位 Read-Modify-Write
//   M12 输出准入      全部方向到齐后结果进输出队列，发送前查目标 VC credit 与
//                     该方向的下游 Reduce credit，作为 Xbar 的第五路输入重新
//                     参与仲裁
//
// 「全部方向」取自 RouterTable 的 reduce_in_mask：按包头的 path_id 查表，得到
// 这条 path 在本级会有哪几个相邻方向送来分量。首份输入建上下文时把这个集合一并
// 记进用户表，本包收齐前不再重查。
//
// 必须执行 Reduce：SRAM、Bank 或计算单元暂不可用时对输入反压，不允许绕过 Reduce
// 降级为直接存储或转发。
//
// 上下文保护：当前包的全部输入完成并输出前，同一 User 的下一个包不得覆盖。
//
// 16 个用户上下文与进 core 的 stream credit 表项一一对应，同为 16 项、同在建表时
// 占用、同在 Retire 时回收，所以不会出现 Stream 已授权而这里没有上下文的情况；
// 反压只覆盖 SRAM、Bank 与计算单元三种暂时不可用。
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

// 用户上下文数，与进 core 的 stream credit 表项一一对应。
constexpr uint64_t kReduceCtxNum = 16;
// 输出队列深度，建模计划的默认值。
constexpr uint64_t kReduceOutQDepth = 8;
// 每个用户每个方向的下游 Reduce credit 初值，建模计划的默认值。
constexpr uint64_t kReduceCreditInit = 64;

// ReduceModule → TS：一个整包 reduce 完成。TS 只认这一路把 reduce task 置
// FINISH，并按 reduce_seq 与 DTE 的那一半配对。
class ReduceDonePort : public Logic {
 public:
  Logic64 valid, user_id, reduce_seq;

  explicit ReduceDonePort(ClockPtr c)
      : valid(c), user_id(c), reduce_seq(c) {
    Fields(valid, user_id, reduce_seq);
  }

  void Drive(uint64_t user, uint64_t seq) {
    valid = 1;
    user_id = user;
    reduce_seq = seq;
  }
  void Idle() {
    valid = 0;
    user_id = 0;
    reduce_seq = 0;
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
        level(std::make_shared<ReadyLevelPort>(clock)),
        req(std::make_shared<XbarReqPort>(clock)),
        accepted(clock),
        emitted(clock),
        stalled(clock) {
    for (uint64_t r = 0; r < 3; ++r) {
      in_wire.push_back(std::make_shared<LinkEnd>(clock));
    }
  }

  // 三路输入，接 Xbar 的 reduce_0 / 1 / 2 出口。
  LinkEnd& In(uint64_t r) { return *in_wire.at(r); }
  void AttachIn(uint64_t r, LinkEndPtr wire) { in_wire.at(r) = std::move(wire); }
  // 结果回注 Xbar 的第五路输入。方向已经由 flow_dir 算好，直接交请求，不再走
  // 一遍 RouterStation 重查表 —— 那样会拿同一个 path_id 判成「又要归约」。
  std::shared_ptr<XbarReqPort> ReqPtr() const { return req; }
  ReduceDonePort& Done() { return *done; }
  std::shared_ptr<ReduceDonePort> DonePtr() const { return done; }
  // 准入电平给 Xbar 读：SRAM、Bank 或计算单元暂不可用时对输入反压，不允许绕过
  // Reduce 降级为直接存储或转发。这一层的判据就是输出队列还装不装得下。
  std::shared_ptr<ReadyLevelPort> LevelPtr() const { return level; }

  // 用户建 stream credit 表项时占一个上下文，Retire 时回收。
  void AllocContext(uint64_t user) {
    if (ctx.count(user) != 0) return;
    LOGCHECK(ctx.size() < kReduceCtxNum,
             "ReduceModule: 上下文用光了。它与进 core 的 stream 表一一对应，"
             "到这一步说明两边的分配逻辑不一致。");
    Ctx c;
    c.down_credit.fill(kReduceCreditInit);
    ctx[user] = c;
  }
  // 延迟回收：先记 Retire，待相邻下游各方向的 credit 全部恢复到初值后才删。
  // Retire 从这个口广播退休的 user。表由本模块的协程改，Retire 只送号。
  void AttachRetire(std::shared_ptr<RetireBroadcastPort> p) {
    retire_in = std::move(p);
  }

  void RetireUser(uint64_t user) {
    auto it = ctx.find(user);
    if (it == ctx.end()) return;
    it->second.retired = true;
  }
  bool HoldsUser(uint64_t user) const { return ctx.count(user) != 0; }
  uint64_t ContextUsed() const { return ctx.size(); }
  uint64_t DownCredit(uint64_t user, uint64_t dir) const {
    auto it = ctx.find(user);
    if (it == ctx.end()) return 0;
    return it->second.down_credit[dir];
  }
  // 下游发出一个 flit 就回一个 release。
  void ReturnCredit(uint64_t user, uint64_t dir) {
    auto it = ctx.find(user);
    if (it == ctx.end()) return;
    LOGCHECK(it->second.down_credit[dir] < kReduceCreditInit,
             "ReduceModule: Reduce credit 归还超过初值。");
    ++it->second.down_credit[dir];
  }

  // 累加完等着发出去的还有几笔。
  uint64_t OutQueued() const { return out_q.size(); }
  uint64_t Accepted() const { return accepted.Get(); }
  uint64_t Emitted() const { return emitted.Get(); }
  uint64_t Stalled() const { return stalled.Get(); }

  bool Quiescent() const override { return out_q.empty(); }

 protected:
  void Step() override {
    TakeRetire();
    // 末级先做。
    EmitOne();
    for (uint64_t r = 0; r < 3; ++r) AcceptFrom(r);

    level->Drive(out_q.size() < kReduceOutQDepth);
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

  struct Ctx {
    bool busy = false;               // 当前有一个包正在收
    uint64_t path_id = 0;
    uint64_t reduce_seq = 0;
    uint64_t expect_mask = 0;        // 建上下文时记下的 reduce_in_mask
    uint64_t in_done_mask = 0;
    std::vector<float> acc;          // FP32 中间累加结果
    std::vector<uint8_t> sw_head;    // 首份输入的软件辅助信息，输出时照抄
    uint64_t in_dtype = kReduceBf16, out_dtype = kReduceBf16;
    std::array<uint64_t, 3> down_credit{};
    bool retired = false;
    uint64_t locked_from = 3;        // 锁定到尾 flit 的那一路，3 = 没锁
    MessagePtr first_msg;            // 首份输入的包头，输出时照抄
  };
  struct OutItem {
    uint64_t user_id = 0, reduce_seq = 0;
    MessagePtr msg;
  };

  // M10 与 M11。
  void AcceptFrom(uint64_t r) {
    FlitView f = ReadFlit(in_wire[r]->flit);
    if (!f.valid || !f.msg) return;
    uint64_t user = f.msg->user_id;
    auto it = ctx.find(user);
    LOGCHECK(it != ctx.end(),
             "ReduceModule: 收到没有上下文的用户分量。上下文与 stream 表一一"
             "对应，到这一步说明建表那一步漏了。");
    Ctx& c = it->second;

    RouteEntry const& e = rtab.Lookup(copy, f.msg->path_id);
    // reduce_in_mask 为 0 表示本级不做累加：不派角色的 core 与纯透传的中继核
    // 都是这一档，包按 flow_dir 直接转发，不该进到这里。
    LOGCHECK(e.reduce_in_mask != 0,
             "ReduceModule: 这条 path 在本级 reduce_in_mask 是 0，不该进来。");

    // 进入后锁定当前包直至尾 flit：三路输入不能在包中间换。
    if (c.locked_from != 3 && c.locked_from != r) {
      ++stalled_pending;
      return;
    }

    // 上下文保护：当前包的全部输入完成并输出前，同一 User 的下一个包不得覆盖。
    if (c.busy && c.reduce_seq != f.msg->reduce_seq) {
      ++stalled_pending;
      return;
    }

    // 一路的一份分量占几个 flit 由包长定，整包的字节都挂在同一个 Message 上，
    // 每个 flit 读到的 payload 都是整包。所以只在首 flit 那一拍动累加器，后面
    // 的 flit 只推进这一路的收包状态，否则一份分量会被算上好几遍。
    if (f.head) {
      if (!c.busy) {
        // 首份输入建上下文，把 expect_mask 一并记下，本包收齐前不再重查。
        c.busy = true;
        c.path_id = f.msg->path_id;
        c.reduce_seq = f.msg->reduce_seq;
        c.expect_mask = e.reduce_in_mask;
        c.in_done_mask = 0;
        c.in_dtype = e.reduce_data_type;
        c.out_dtype = e.reduce_outdata_type;
        c.acc = FirstOf(f.msg->payload, c.in_dtype);
        c.sw_head.assign(f.msg->payload.begin(),
                         f.msg->payload.begin() +
                             std::min<size_t>(kReduceSwHeaderBytes,
                                              f.msg->payload.size()));
        c.first_msg = f.msg;
      } else {
        Accumulate(c.acc, f.msg->payload, c.in_dtype);
      }
    }
    c.locked_from = f.tail ? 3 : r;
    ++accepted_pending;

    if (!f.tail) return;
    c.in_done_mask |= 1ull << r;
    // all_in = (in_done_mask == expect_mask)
    if (c.in_done_mask != c.expect_mask) return;

    if (out_q.size() >= kReduceOutQDepth) {
      // 输出队列满时停止 RMW 输出，对输入反压，不绕过 Reduce。
      ++stalled_pending;
      return;
    }
    auto m = std::make_shared<Message>(*c.first_msg);
    m->payload = PackOf(c.acc, c.sw_head, c.out_dtype);
    m->size = m->payload.size();
    out_q.push_back({user, c.reduce_seq, m});
    c.busy = false;
    c.in_done_mask = 0;
    c.locked_from = 3;
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

  // 首份输入建上下文：软件辅助信息原样留着，数据段解成 FP32 存进累加器。
  static std::vector<float> FirstOf(std::vector<uint8_t> const& in,
                                    uint64_t dtype) {
    std::vector<float> acc;
    uint64_t n = ElemBytes(dtype);
    for (uint64_t i = kReduceSwHeaderBytes; i + n <= in.size(); i += n) {
      acc.push_back(ElemOf(in, i, dtype));
    }
    return acc;
  }

  // 中间累加固定 FP32，逐元素原位加。
  static void Accumulate(std::vector<float>& acc,
                         std::vector<uint8_t> const& add, uint64_t dtype) {
    uint64_t n = ElemBytes(dtype);
    uint64_t k = 0;
    for (uint64_t i = kReduceSwHeaderBytes; i + n <= add.size(); i += n, ++k) {
      float v = ElemOf(add, i, dtype);
      if (k < acc.size()) {
        acc[k] = numeric::ClampNanInf(acc[k] + v);
      } else {
        acc.push_back(v);
      }
    }
  }

  // 输出按 reduce_outdata_type 转回去，软件辅助信息照抄首份输入的那 16 B。
  static std::vector<uint8_t> PackOf(std::vector<float> const& acc,
                                     std::vector<uint8_t> const& head,
                                     uint64_t out_dtype) {
    uint64_t n = ElemBytes(out_dtype);
    std::vector<uint8_t> out(kReduceSwHeaderBytes + acc.size() * n, 0);
    for (uint64_t i = 0; i < kReduceSwHeaderBytes && i < head.size(); ++i) {
      out[i] = head[i];
    }
    for (uint64_t k = 0; k < acc.size(); ++k) {
      uint64_t at = kReduceSwHeaderBytes + k * n;
      if (out_dtype == kReduceFp32) {
        uint32_t v = numeric::BitsOf(acc[k]);
        for (int j = 0; j < 4; ++j) out[at + j] = uint8_t((v >> (8 * j)) & 0xFF);
      } else {
        uint16_t v = numeric::ToBf16(acc[k]);
        out[at] = uint8_t(v & 0xFF);
        out[at + 1] = uint8_t((v >> 8) & 0xFF);
      }
    }
    return out;
  }

  // M12：发送前查下游 Reduce credit，每发一个 flit 扣一个。
  void EmitOne() {
    ReapRetired();
    // 与各 RouterStation 同一套：Xbar 那一侧上一拍说收得下就发，发了当场出队。
    if (!req->Room()) {
      req->IdleReq();
      done->Idle();
      return;
    }
    if (out_q.empty()) {
      req->IdleReq();
      done->Idle();
      return;
    }
    OutItem it = out_q.front();
    auto cit = ctx.find(it.user_id);
    LOGCHECK(cit != ctx.end(), "ReduceModule: 输出时上下文已经没了。");

    // 往哪几个方向发由这条 path 的 flow_dir 定，下游 Reduce credit 按 UserID
    // 加目标方向记账，每发一个 flit 每个方向各扣一个。
    RouteEntry const& e = rtab.Lookup(copy, it.msg->path_id);
    std::vector<uint64_t> dirs = DownDirsOf(e.flow_dir);
    // 多播全有全无：任一方向的 credit 不够就整体等，不让分支独立前进。
    for (uint64_t d : dirs) {
      if (cit->second.down_credit[d] == 0) {
        ++stalled_pending;
        req->IdleReq();
        done->Idle();
        return;
      }
    }
    for (uint64_t d : dirs) --cit->second.down_credit[d];

    uint64_t mask = 0;
    for (uint64_t d : dirs) {
      mask |= 1ull << (d == 0 ? kOutMid : (d == 1 ? kOutLeft : kOutRight));
    }
    // 末端汇聚核 flow_dir 全不置位，结果只交给本 core。
    if (mask == 0 || e.EntersCore(it.msg->path_core_mask)) {
      mask |= 1ull << kOutCore;
    }
    req->valid = 1;
    req->out_mask = mask;
    req->vc = kReduceVc;
    req->head = 1;
    req->tail = 1;
    req->bytes = it.msg->size;
    req->path_id = it.msg->path_id;
    req->user_id = it.user_id;
    req->enters_core = ((mask >> kOutCore) & 1u) ? 1 : 0;
    req->stall_way = 0;
    req->need_stream = 0;
    req->msg = it.msg;
    req->seq = ++req_seq;
    // 整包发出后向 core 返回 UserID 与包头里的 reduce_seq。TS 只认这一路把
    // reduce task 置 FINISH。
    done->Drive(it.user_id, it.reduce_seq);
    out_q.pop_front();
    ++emitted_pending;
  }

  // flow_dir 的 R2R 三位映射到 down_credit 的下标。末端汇聚核 flow_dir 全不
  // 置位，结果只交给本 core，不占任何下游 Reduce credit。
  static std::vector<uint64_t> DownDirsOf(uint64_t flow_dir) {
    std::vector<uint64_t> dirs;
    if (flow_dir & kFlowMid) dirs.push_back(0);
    if (flow_dir & kFlowLeft) dirs.push_back(1);
    if (flow_dir & kFlowRight) dirs.push_back(2);
    return dirs;
  }

  // 延迟回收：credit 全部恢复到初值才删。
  void ReapRetired() {
    for (auto it = ctx.begin(); it != ctx.end();) {
      bool all_back = true;
      for (uint64_t c : it->second.down_credit) {
        if (c != kReduceCreditInit) all_back = false;
      }
      if (it->second.retired && !it->second.busy && all_back) {
        it = ctx.erase(it);
      } else {
        ++it;
      }
    }
  }

  RouterTable& rtab;
  uint64_t copy;
  std::vector<LinkEndPtr> in_wire;
  std::shared_ptr<XbarReqPort> req;
  std::shared_ptr<ReduceDonePort> done;
  std::shared_ptr<ReadyLevelPort> level;

  // Step 独占。
  std::map<uint64_t, Ctx> ctx;
  std::deque<OutItem> out_q;
  uint64_t req_seq = 0;
  uint64_t accepted_pending = 0, emitted_pending = 0, stalled_pending = 0;

  Logic64 accepted, emitted, stalled;
};

}  // namespace bach
}  // namespace latch

#endif
