#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_TAIL_ROUTE_
#define _LATCH_BACH_IP_CHIP_CORE_VU_TAIL_ROUTE_

// VEXE 之后的整级跳过。
//
// 本条没有 MEXE 指令就不进 MEXE，没有 SEXE 指令就不进 SEXE，直接交给后面还在
// 这条依赖链上的那一级。跳过的级不加握手拍（F16b、F40d）。用到的那一级仍按
// 它自己的首拍延迟走。
//
// 装配把本模块拆成两段调用：Pre 在 VEXE 之前，只为了让 VEXE 本拍就看见 ready；
// Post 在 VEXE 之后，收下刚交出来的段并送到下一级。下一级本拍已经跑过，下一拍
// 才看见，这一拍就是级间那一拍，不是被跳过的单元自己的拍。

#include <deque>
#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/vu/vu_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class VuTailRoute : public BachModule {
 public:
  VuTailRoute(ClockPtr clock, const std::string& name, uint64_t parent = 0,
              bool tick = true)
      : BachModule(clock, name, parent, tick),
        vexe_in(std::make_shared<VuFlowPort>(clock)),
        mexe_in(std::make_shared<VuFlowPort>(clock)),
        sexe_in(std::make_shared<VuFlowPort>(clock)),
        to_mexe(std::make_shared<VuFlowPort>(clock)),
        to_sexe(std::make_shared<VuFlowPort>(clock)),
        to_dmux(std::make_shared<VuFlowPort>(clock)) {}

  void AttachVexe(std::shared_ptr<VuFlowPort> p) { vexe_in = std::move(p); }
  void AttachMexe(std::shared_ptr<VuFlowPort> p) { mexe_in = std::move(p); }
  void AttachSexe(std::shared_ptr<VuFlowPort> p) { sexe_in = std::move(p); }
  void AttachToMexe(std::shared_ptr<VuFlowPort> p) { to_mexe = std::move(p); }
  void AttachToSexe(std::shared_ptr<VuFlowPort> p) { to_sexe = std::move(p); }
  void AttachToDmux(std::shared_ptr<VuFlowPort> p) { to_dmux = std::move(p); }

  // VEXE 还没跑。先收下 MEXE / SEXE 交回来的段，并放开 VEXE 的 ready，
  // 这样 VEXE 本拍就能交出下一段。
  void Pre() {
    Pull(mexe_in, last_mexe, Dest::kFromMexe, resume, &in_mexe);
    Pull(sexe_in, last_sexe, Dest::kFromSexe, resume, &in_sexe);
    vexe_in->DriveReady(Room());
  }

  // VEXE 已经把本拍的段放到出口上。
  void Post() {
    driven[0] = driven[1] = driven[2] = false;
    Release(Dest::kMexe);
    Release(Dest::kSexe);
    Release(Dest::kDmux);
    Pull(vexe_in, last_vexe, Dest::kFromVexe, arrive, nullptr);
    Pump();
    if (!driven[0]) to_mexe->Idle();
    if (!driven[1]) to_sexe->Idle();
    if (!driven[2]) to_dmux->Idle();
    mexe_in->DriveReady(Room());
    sexe_in->DriveReady(Room());
  }

  bool Quiescent() const override {
    return arrive.empty() && resume.empty() && in_mexe == 0 && in_sexe == 0 &&
           !hold[0] && !hold[1] && !hold[2];
  }

 protected:
  void Step() override { Post(); }

 private:
  enum class Dest { kMexe = 0, kSexe = 1, kDmux = 2, kFromVexe, kFromMexe,
                    kFromSexe };

  struct Beat {
    VuFlowPtr f;
    uint64_t seq = 0;
    Dest dest = Dest::kDmux;
  };

  static constexpr uint64_t kCap = 8;

  static bool MexeOn(VuUops const& u) { return u.cfg.mexe.Active(); }
  static bool SexeOn(VuUops const& u) {
    return u.cfg.sexe[0].Active() || u.cfg.sexe[1].Active() ||
           u.cfg.sexe[2].Active();
  }

  static Dest FirstHop(VuFlow const& f) {
    if (MexeOn(f.uops)) return Dest::kMexe;
    if (SexeOn(f.uops)) return Dest::kSexe;
    return Dest::kDmux;
  }

  static Dest AfterMexe(VuFlow const& f) {
    return SexeOn(f.uops) ? Dest::kSexe : Dest::kDmux;
  }

  bool Room() const {
    return arrive.size() + resume.size() + 3 <= kCap;
  }

  VuFlowPort& OutOf(Dest d) {
    if (d == Dest::kMexe) return *to_mexe;
    if (d == Dest::kSexe) return *to_sexe;
    return *to_dmux;
  }

  void Pull(std::shared_ptr<VuFlowPort>& in, uint64_t& last, Dest kind,
            std::deque<Beat>& q, int* inside) {
    if (!in->Valid() || in->Seq() == last) return;
    VuFlowPtr f = in->Flow();
    if (!f) return;
    if (arrive.size() + resume.size() >= kCap) return;
    Beat b;
    b.f = f;
    b.seq = in->Seq();
    if (kind == Dest::kFromVexe) b.dest = FirstHop(*f);
    else if (kind == Dest::kFromMexe) b.dest = AfterMexe(*f);
    else b.dest = Dest::kDmux;
    if (inside != nullptr) {
      LOGCHECK(*inside > 0, "tail: stage output with nothing in flight");
      --(*inside);
    }
    q.push_back(std::move(b));
    last = in->Seq();
  }

  void Release(Dest d) {
    int i = static_cast<int>(d);
    if (!hold[i]) return;
    VuFlowPort& p = OutOf(d);
    if (!p.Ready()) {
      p.Drive(held[i], held_seq[i]);
      driven[i] = true;
      return;
    }
    hold[i] = false;
    held[i] = VuFlowPtr();
  }

  // 更早的段还在 MEXE 或 SEXE 里时，后一段不能从旁边绕到更后面的级，否则会
  // 超车。沿着同一级继续灌（MEXE 的下一段仍进 MEXE）是允许的，那一级自己保序。
  bool CanIssue(Beat const& b) const {
    if (in_mexe > 0 && b.dest != Dest::kMexe) return false;
    if (in_sexe > 0 && b.dest != Dest::kSexe) return false;
    if (!resume.empty() && b.dest != Dest::kMexe) return false;
    return true;
  }

  bool SendFront(std::deque<Beat>& q, bool gate) {
    if (q.empty()) return false;
    Beat& b = q.front();
    if (gate && !CanIssue(b)) return false;
    int i = static_cast<int>(b.dest);
    if (hold[i]) return false;
    VuFlowPort& p = OutOf(b.dest);
    if (!p.Ready()) return false;
    hold[i] = true;
    held[i] = b.f;
    held_seq[i] = b.seq;
    p.Drive(held[i], held_seq[i]);
    driven[i] = true;
    if (b.dest == Dest::kMexe) ++in_mexe;
    if (b.dest == Dest::kSexe) ++in_sexe;
    q.pop_front();
    return true;
  }

  void Pump() {
    while (SendFront(resume, false)) {
    }
    while (SendFront(arrive, true)) {
    }
  }

  std::shared_ptr<VuFlowPort> vexe_in, mexe_in, sexe_in;
  std::shared_ptr<VuFlowPort> to_mexe, to_sexe, to_dmux;
  std::deque<Beat> arrive;
  std::deque<Beat> resume;
  VuFlowPtr held[3];
  uint64_t held_seq[3] = {};
  bool hold[3] = {};
  bool driven[3] = {};
  uint64_t last_vexe = 0, last_mexe = 0, last_sexe = 0;
  int in_mexe = 0, in_sexe = 0;
};

}  // namespace bach
}  // namespace latch

#endif
