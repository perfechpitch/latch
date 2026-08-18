#ifndef _LATCH_BACH_SIM_COMPLETION_
#define _LATCH_BACH_SIM_COMPLETION_

// 什么时候算跑完，以及跑不完时怎么停。
//
// 它自己是一个挂时钟的模块，每拍看一次判完了多少：够了就停时钟，超过时限还没够就
// 当场停机报错。
//
// 由谁来判，一次 run 里只能有一处：
//
//   汇聚点      没有 Phase 时是它。数的是包不是 user，因为一个 user 的结果可能落在
//               好几个汇聚点上，每个汇聚点只知道自己收了几包，加起来等于所有 user
//               的分片数之和才算跑完
//   交换节点    有 Phase 时是它。数的是一次次工作：Phase1 那两路都投出去了算一次，
//               或者残差与结果在汇合点碰上了算一次，两种判法同时只有一个生效
//
// 它只读不写：读的是那一处提交出来的计数，那是一个跨拍可读的量，别的协程读到的是上
// 一拍的值，所以判完成会晚一拍。这一拍只影响时钟什么时候停，不影响任何模型量。直接
// 读对方的内部容器是不行的，那是另一个协程正在改的东西。
//
// 时限用的是全局生命周期上限。收包收不齐、注入源等不到额度这两类卡死，各自在发生的
// 地方就能看出来，不必也不该由这里去猜。

#include <cstdint>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/log.h"
#include "base/module.h"
#include "base/runtime.h"
#include "bach/ip/system.h"

namespace latch {
namespace bach {

// 完成由谁来判。装配时定下，一次 run 里不变。
enum class CompletionAuthority : uint32_t {
  kOut = 0,  // 汇聚点，数的是包
  kEth = 1,  // 以太网交换节点，数的是一次次工作
};

class RunControl : public ClkModule {
 public:
  // 汇聚点判完成时 expected 是所有 user 的分片数之和；交换节点判完成时它是一次 run
  // 里有多少次工作，也就是各条通道产出的 user 数之和。
  RunControl(ClockPtr clock, System& sys, uint64_t expected,
             std::string const& name, uint64_t parent,
             CompletionAuthority who = CompletionAuthority::kOut)
      : ClkModule(clock),
        expected_packets(expected),
        limit(sys.P().watchdog_lifespan),
        authority(who) {
    RegisterId(name, parent);
    if (authority == CompletionAuthority::kEth) {
      LOGCHECK(sys.HasEth(),
               "RunControl: the eth switch was asked to judge completion but "
               "there is no eth switch.");
      eth = &sys.Eth();
      return;
    }
    for (auto const& o : sys.Outs()) outs.push_back(o.get());
    LOGCHECK(!outs.empty(), "RunControl: there is no output node to watch.");
  }

  void Cycle() override {
    DelayCycle(1);
    if (finished) return;

    const Time now = RT::Now();
    uint64_t done = 0;
    if (authority == CompletionAuthority::kEth) {
      done = static_cast<uint64_t>(eth->DoneCount());
    } else {
      for (Out const* o : outs) done += static_cast<uint64_t>(o->DoneCount());
    }
    completed = done;

    if (done >= expected_packets) {
      finished = true;
      finish_time = now;
      clk->Stop();
      return;
    }

    LOGCHECK(now <= limit,
             "RunControl: the run passed its lifetime limit with users still "
             "in flight.");
  }

  // 以下几个在 JoinAll 之后由主线程读。
  bool Finished() const { return finished; }
  Time FinishTime() const { return finish_time; }
  // 判完的数量。汇聚点判时它是收齐的包数，交换节点判时它是完成的工作次数，两者
  // 都不是完成的 user 数。
  uint64_t CompletedNum() const { return completed; }
  CompletionAuthority Authority() const { return authority; }

 private:
  uint64_t expected_packets;
  Time limit;
  CompletionAuthority authority;
  std::vector<Out const*> outs;
  EthSwitch const* eth = nullptr;

  bool finished = false;
  Time finish_time = 0;
  uint64_t completed = 0;
};

}
}

#endif
