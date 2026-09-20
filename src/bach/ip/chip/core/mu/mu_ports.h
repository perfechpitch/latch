#ifndef _LATCH_BACH_IP_CHIP_CORE_MU_MU_PORTS_
#define _LATCH_BACH_IP_CHIP_CORE_MU_MU_PORTS_

// MU 与 core 内其他单元之间的端口。
//
// 一条 DTE → MU 的 topK 数据线：DTE 搬运时把进核包里的 topK 按 stream_id 直接
// 写进 MU 的 topK_ep_table[stream_id]，256 B，1 拍写入。单拍 fire-and-forget、
// 没有 ready：与 DonePort 同一套脉冲语义，完成事件在硬件里没有重发通路，接收方
// 当拍锁存即可。

#include <cstdint>
#include <memory>

#include "base/logic.h"
#include "bach/ip/chip/core/ports.h"

namespace latch {
namespace bach {

class MuTopkPort : public Logic {
 public:
  Logic64 valid, stream_id, seq;
  LogicPtr<ByteBlock> data;

  explicit MuTopkPort(ClockPtr c)
      : valid(c), stream_id(c), seq(c), data(c) {
    Fields(valid, stream_id, seq, data);
  }

  // DTE 侧每拍二选一调一次。
  void Drive(uint64_t stream, ByteBlockPtr bytes) {
    valid = 1;
    stream_id = stream;
    data = std::move(bytes);
    seq = ++issue_seq;
  }
  void Idle() {
    valid = 0;
    stream_id = 0;
    data = ByteBlockPtr();
    seq = seq.Get();
  }

  // MU 侧读。同一拍数据会连着两拍出现在端口上，按序号认它。
  bool Valid() const { return valid.Get() != 0; }
  uint64_t StreamId() const { return stream_id.Get(); }
  uint64_t Seq() const { return seq.Get(); }
  ByteBlockPtr Data() const { return data.Get(); }

 private:
  uint64_t issue_seq = 0;
};

}  // namespace bach
}  // namespace latch

#endif
