#ifndef _LATCH_BACH_KSIM_CORE_
#define _LATCH_BACH_KSIM_CORE_

// 一个核。它是挂时钟的节点，内部两个从属单元跟着它的一个协程走。
//
// 每拍的 stage 顺序按末级先做排：Router 在前，本拍投递到的包执行体这一拍就看得见；
// 执行体本拍发出的包 Router 下一拍才仲裁。这与 Bach 里同刻的函数调用一致。
//
// 观测记在核自己的 SpanRecorder 上，Router 与执行体共用它，JoinAll 之后由主线程收。

#include <cstdint>
#include <memory>
#include <string>

#include "base/clock.h"
#include "base/log.h"
#include "base/module.h"
#include "bach/ksim/exec.h"
#include "bach/ksim/kernel.h"
#include "bach/ksim/router.h"
#include "bach/ksim/topology.h"
#include "bach/observer/span_recorder.h"

namespace latch {
namespace bach {
namespace ksim {

class Core : public ClkModule {
 public:
  Core(ClockPtr clock, uint64_t core_id, CoreKernel const* kernel,
       Topology const& topo, RouterParams const& rp, CoreParams const& cp,
       std::string const& name, uint64_t parent)
      : ClkModule(clock),
        id(core_id),
        self_id(RegisterId(name, parent)),
        router(clock, core_id, topo, rp, &recorder, "router", self_id),
        exec(clock, core_id, kernel, &router, cp, &recorder, "exec", self_id) {
    router.ConnectCore(&exec);
  }

  void Cycle() override {
    DelayCycle(1);
    router.Step();
    exec.Step();
  }

  uint64_t CoreId() const { return id; }
  Router& Rt() { return router; }
  Exec& Unit() { return exec; }
  SpanRecorder const& Recorder() const { return recorder; }

 private:
  uint64_t id;
  SpanRecorder recorder;
  uint64_t self_id;
  Router router;
  Exec exec;
};

}
}
}

#endif
