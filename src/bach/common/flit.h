#ifndef _LATCH_BACH_COMMON_FLIT_
#define _LATCH_BACH_COMMON_FLIT_

// 链路上流动的最小单位与它的端口束。
//
// flit 定长 FLIT_BYTES，尾 flit 用 bytes 给出有效字节数。同一个 Message 的多个
// flit 共享一份 payload：msg 是 LogicPtr<Message>，搬的是 shared_ptr。
//
// 端口的每个信号都是上拍写、下拍读的寄存器。发送侧每拍必须在 Drive() 与 Idle()
// 里二选一调一次：Logic64 一拍只能 Set 一次，同拍两次触发断言；而一拍都不写，
// Get() 会回落到上一拍的值，同一个 flit 就发了两遍。

#include <cstdint>

#include "base/logic.h"
#include "bach/common/message.h"

namespace latch {
namespace bach {

// R2R / C2C 一拍 256 B。
constexpr uint64_t kFlitBytes = 256;

// VC 编号 2 bit，四条。
constexpr uint64_t kVcNum = 4;

// 一个整包要几个 flit。
inline uint64_t FlitsOf(uint64_t bytes) {
  if (bytes <= kFlitBytes) return 1;
  return (bytes + kFlitBytes - 1) / kFlitBytes;
}

// 数据通道：flit_valid · flit_vc[1:0] · flit_head · flit_tail · flit_bytes[8:0]
// · flit_msg。
class FlitPort : public Logic {
 public:
  Logic64 valid, vc, head, tail, bytes;
  LogicPtr<Message> msg;

  explicit FlitPort(ClockPtr c)
      : valid(c), vc(c), head(c), tail(c), bytes(c), msg(c) {
    Fields(valid, vc, head, tail, bytes, msg);
  }

  void Drive(uint64_t vc_id, bool is_head, bool is_tail, uint64_t nbytes,
             MessagePtr const& m) {
    valid = 1;
    vc = vc_id;
    head = is_head ? 1 : 0;
    tail = is_tail ? 1 : 0;
    bytes = nbytes;
    msg = m;
  }

  void Idle() {
    valid = 0;
    vc = 0;
    head = 0;
    tail = 0;
    bytes = 0;
    msg = MessagePtr();
  }

  bool Valid() const { return valid.Get() != 0; }
};

// 一拍从端口上读下来的值。跨模块搬运时用它，不复用端口对象本身。
struct FlitView {
  bool valid = false;
  uint64_t vc = 0;
  bool head = false;
  bool tail = false;
  uint64_t bytes = 0;
  MessagePtr msg;
};

inline FlitView ReadFlit(FlitPort const& p) {
  FlitView v;
  v.valid = p.valid.Get() != 0;
  if (!v.valid) return v;
  v.vc = p.vc.Get();
  v.head = p.head.Get() != 0;
  v.tail = p.tail.Get() != 0;
  v.bytes = p.bytes.Get();
  v.msg = p.msg.Get();
  return v;
}

// 三种 release 各走各的通道，都是单拍脉冲。
// vc_release   链路层：一个 flit 腾出下游一个 VC 槽就还一个
// stream_release / reduce_release  业务层：按 user 还
class ReleasePort : public Logic {
 public:
  Logic64 vc_valid, vc_id;
  Logic64 stream_valid, stream_user;
  Logic64 reduce_valid, reduce_user;

  explicit ReleasePort(ClockPtr c)
      : vc_valid(c), vc_id(c), stream_valid(c), stream_user(c),
        reduce_valid(c), reduce_user(c) {
    Fields(vc_valid, vc_id, stream_valid, stream_user, reduce_valid,
           reduce_user);
  }

  void Idle() {
    vc_valid = 0;
    vc_id = 0;
    stream_valid = 0;
    stream_user = 0;
    reduce_valid = 0;
    reduce_user = 0;
  }

  // 三条通道互相独立，一拍可以同时有。所以给的是整拍一次写全的接口。
  void Drive(bool vc_rel, uint64_t vc, bool stream_rel, uint64_t stream_u,
             bool reduce_rel, uint64_t reduce_u) {
    vc_valid = vc_rel ? 1 : 0;
    vc_id = vc;
    stream_valid = stream_rel ? 1 : 0;
    stream_user = stream_u;
    reduce_valid = reduce_rel ? 1 : 0;
    reduce_user = reduce_u;
  }
  // 同一根回线上 VC 那一类与 Reduce 那一类由不同的模块写：各写各的那几项，
  // 不碰另一类。
  void DriveVc(bool vc_rel, uint64_t vc) {
    vc_valid = vc_rel ? 1 : 0;
    vc_id = vc;
  }
  void DriveReduce(bool reduce_rel, uint64_t reduce_u) {
    reduce_valid = reduce_rel ? 1 : 0;
    reduce_user = reduce_u;
  }
};

struct ReleaseView {
  bool vc_valid = false;
  uint64_t vc_id = 0;
  bool stream_valid = false;
  uint64_t stream_user = 0;
  bool reduce_valid = false;
  uint64_t reduce_user = 0;

  bool Any() const { return vc_valid || stream_valid || reduce_valid; }
};

inline ReleaseView ReadRelease(ReleasePort const& p) {
  ReleaseView v;
  v.vc_valid = p.vc_valid.Get() != 0;
  v.vc_id = p.vc_id.Get();
  v.stream_valid = p.stream_valid.Get() != 0;
  v.stream_user = p.stream_user.Get();
  v.reduce_valid = p.reduce_valid.Get() != 0;
  v.reduce_user = p.reduce_user.Get();
  return v;
}

// 链路一个方向的完整端口：数据加三种 release。两端各持一个，装配层对接。
class LinkEnd : public Logic {
 public:
  FlitPort flit;
  ReleasePort release;

  explicit LinkEnd(ClockPtr c) : flit(c), release(c) {
    Fields(flit, release);
  }

  void Idle() {
    flit.Idle();
    release.Idle();
  }
};

using LinkEndPtr = std::shared_ptr<LinkEnd>;

}  // namespace bach
}  // namespace latch

#endif
