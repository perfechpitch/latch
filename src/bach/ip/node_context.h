#ifndef _LATCH_BACH_IP_NODE_CONTEXT_
#define _LATCH_BACH_IP_NODE_CONTEXT_

// 任何一个挂时钟的节点都有的三样东西：它在观测里的编号、全局参数表、它自己的事件
// 记录器。核有任务表，外部节点没有，所以任务表不在这一层。
//
// 记录器是节点的 Cycle 独占状态，各单元往里追加事件，但只旁路记录，不参与任何判定。

#include <cstdint>

#include "base/log.h"
#include "bach/common/params.h"
#include "bach/observer/span_recorder.h"

namespace latch {
namespace bach {

struct NodeContext {
  uint64_t node_id = 0;
  Params const* params = nullptr;
  SpanRecorder* rec = nullptr;

  Params const& P() const { return *params; }

  void ValidateNode() const {
    LOGCHECK(params != nullptr, "NodeContext: params is null.");
    LOGCHECK(rec != nullptr, "NodeContext: span recorder is null.");
  }

  // allow_zero 只给那些零拍也算发生过的占用用，默认零长视为无可归因时长而丢弃。
  void Span(Unit unit, uint64_t uid, uint64_t tid, Time start, Time end,
            SpanState state, uint64_t volume = 0,
            bool allow_zero = false) const {
    rec->Span(node_id, unit, uid, tid, start, end, state, volume, allow_zero);
  }

  void Wait(Unit unit, uint64_t uid, uint64_t tid, Time start, Time end,
            WaitReason reason) const {
    rec->Wait(node_id, unit, uid, tid, start, end, reason);
  }
};

}
}

#endif
