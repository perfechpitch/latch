#ifndef _LATCH_BACH_IP_CHIP_CORE_PORTS_
#define _LATCH_BACH_IP_CHIP_CORE_PORTS_

// Core 内单元之间的调用口，以及 Core 对外的封包出入口。
//
// 单元之间是同刻调用，所以口就是一组纯虚方法，不是 Fifo。之所以要抽出接口而不是
// 互相持有具体类型，是因为调用是双向的：TaskScheduler 派任务给 DTE 与两个计算核，
// 它们完成后又要回调 TaskScheduler。用接口把回调那一侧收窄成"只暴露被调用方真正
// 需要的能力"，装配顺序也就不受构造顺序牵制。
//
// PacketSink 是 Core 对外发包的唯一出口。第一期它接一条直连链路，往后由 Router
// 实现，DTE 那一侧的代码不变：DTE 只负责把一拍交出去，往哪条线上走、什么时候到，
// 由实现方决定。

#include <cstdint>

#include "bach/common/packet.h"

namespace latch {
namespace bach {

// 完成确认。计算核与 CreditUnit 只需要这一件事。
class AckPort {
 public:
  virtual ~AckPort() = default;
  virtual void Ack(uint64_t uid, uint64_t tid) = 0;
};

// DTE 还要问当前 StreamID，以及为收到 reduction 首包的 user 直接开一条流水。
class SchedulerPort : public AckPort {
 public:
  // unknown_to_tail 为真时，没在活跃队列里的 user 排到队尾，而不是报错。收方向的
  // 包可能比它的 USER_INIT 更早到，这时它还没有 StreamID。
  virtual uint64_t StreamPriority(uint64_t uid, bool unknown_to_tail) const = 0;
  virtual void AdmitWithPrecompletedTask(uint64_t uid) = 0;
};

// DTE 收到 RETIRE 包时向上游归还额度。
class CreditReturnPort {
 public:
  virtual ~CreditReturnPort() = default;
  virtual void ReturnCredit(int64_t core_id, uint64_t amount) = 0;
};

// 一拍数据交出去的地方。size 是这一片的字节数，dst 是目的坐标，路由只看坐标。
class PacketSink {
 public:
  virtual ~PacketSink() = default;
  virtual void Inject(CommInstPtr const& payload, Coord dst, uint64_t size) = 0;
};

// 收到一拍数据的地方。DTE 与两个外部节点都实现它。
class PacketTarget {
 public:
  virtual ~PacketTarget() = default;
  virtual void HandleComm(CommInstPtr const& payload) = 0;
};

}
}

#endif
