// Execute exported pcode, never the simulator's internal MoE compiler bundle.
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <map>
#include "test/bach/ip/chip/moe_common.h"

namespace {
using namespace latch;
using namespace latch::bach;
using namespace latch::bach::moetest;

class PcodeHarness : public SpreadHarness {
 public:
  using SpreadHarness::SpreadHarness;
 protected:
  void Step() override {
    SpreadHarness::Step();
    if (CycleNow() % 1000 == 0)
      std::cerr << "pcode cycle " << CycleNow() << ", output packets " << out_msgs.size() << "\n";
  }
};


// Nonzero sparse matrices: one K coefficient per N tile. The full TP reduction
// therefore matters, while the reference can be computed without a 28 GB model.
void LoadFixtureWeights(std::vector<Chip*> const& all, std::string const& root,
                        std::array<uint64_t,6> const& counts) {
  std::ifstream ir(root + "/pcode_ep6_tp8/pcode_ep6_tp8.bachir");
  std::map<std::tuple<uint64_t,uint64_t,uint64_t>, std::array<uint64_t,10>> tpl;
  std::string line;
  while (std::getline(ir, line)) {
    std::istringstream in(line);
    std::string tag;
    uint64_t chip, core, unit;
    std::string off, value;
    if (!(in >> tag) || tag != "DSAREG") continue;
    in >> chip >> core >> unit >> off >> value;
    uint64_t addr = std::stoull(off, nullptr, 0);
    if (unit != 1 || addr < 0x1000 || addr >= 0x1100) continue;
    uint64_t field = (addr - 0x1000) % 0x80 / 4;
    if (field < 10) tpl[{chip,core,(addr - 0x1000)/0x80}][field] = std::stoull(value,nullptr,0);
  }
  for (auto const& it : tpl) {
    uint64_t chip = std::get<0>(it.first), core = std::get<1>(it.first), stage = std::get<2>(it.first);
    uint64_t ep = chip / 8, count = counts[ep];
    auto const& t = it.second;
    uint64_t nb = t[0] & 0xffff, kb = t[0] >> 16;
    uint64_t base = t[6] - 0x48c00000, stride = t[7];
    auto& mem = all[chip]->GetCore(core).Mmem();
    for (uint64_t expert = 0; expert <= count; ++expert) {
      for (uint64_t n = 0; n < nb; ++n) {
        ByteBlock b(8192, 0);
        for (uint64_t col = 0; col < 128; ++col) {
          float w = stage == 0 ? 0.125f :
              (expert == 0 ? 0.03125f : (col % 6 == ep ? float(ep+1)/64.0f : 0.0f));
          if (stage == 1 && (col & 1)) w = -w;
          b[col * 64] = numeric::ToFp8E4m3(w);
        }
        uint64_t at = base + expert * stride + n * kb * 8192;
        mem.Poke(at, b);
        mem.PokeScale(at, ByteBlock(8192/32, 127));
      }
    }
  }
}

TEST(PcodeBundle, Ep6OneToken) {
  char const* root = std::getenv("PCODE_BUNDLE_ROOT");
  ASSERT_NE(root, nullptr) << "set PCODE_BUNDLE_ROOT to the exported bundle root";
  std::string case_name = std::getenv("PCODE_CASE") ? std::getenv("PCODE_CASE") : "full";
  ASSERT_TRUE(case_name == "full" || case_name == "gap");
  std::array<uint64_t,6> counts = case_name == "full" ?
      std::array<uint64_t,6>{2,2,1,1,1,1} : std::array<uint64_t,6>{2,2,0,2,0,2};
  uint64_t active_mask = 0;
  for (uint64_t ep = 0; ep < 6; ++ep) if (counts[ep]) active_mask |= 1ull << ep;
  EnsureSlots(kChipNum * (kMaxCorePerChip + 1));
  auto clk = MakeClock(0, 1);
  auto owned = MakeLpuChips(clk, 0);
  std::vector<Chip*> all;
  for (auto& c : owned) all.push_back(c.get());
  LoadBundle(all, root, "pcode_ep6_tp8");
  if (std::getenv("PCODE_TRACE_REDUCE"))
    for (uint64_t c = 0; c < all.size(); ++c)
      for (uint64_t p = 0; p < 10; ++p)
        std::cerr << "reduce id " << all[c]->GetCore(p).GetRouter().GetReduce().Id() << " chip " << c << " core " << p << "\n";
  LoadFixtureWeights(all, root, counts);
  auto m = std::make_shared<Message>();
  m->path_id = 63;
  m->path_core_mask = active_mask;
  m->user_id = 0;
  m->token_id = 1;
  m->size = 304 + 6336;
  m->payload.assign(m->size, 0);
  for (uint64_t ep = 0; ep < 6; ++ep) {
    uint64_t count = counts[ep];
    m->payload[ep * 50] = uint8_t(count);
    for (uint64_t e = 0; e < count; ++e) {
      m->payload[ep * 50 + 2 + e * 6] = uint8_t(e);
      uint32_t w = numeric::BitsOf(0.125f);
      for (int b = 0; b < 4; ++b) m->payload[ep * 50 + 4 + e * 6 + b] = uint8_t(w >> (8*b));
    }
  }
  std::fill(m->payload.begin() + 304, m->payload.begin() + 304 + 6144, 0x38);
  std::fill(m->payload.begin() + 304 + 6144, m->payload.end(), 127);
  C2cBridge feed(clk, "feed", StubCfg()), sink(clk, "sink", StubCfg());
  uint64_t max_cycles = 30000;
  if (char const* cap = std::getenv("PCODE_MAX_CYCLES")) max_cycles = std::stoull(cap);
  PcodeHarness h(clk, all, GridLinks(12, 4), feed, sink, 0, kChipW, 47,
                  kChipE, 2, max_cycles, m);
  clk->Continue();
  RT::JoinAll();
  std::cerr << "pcode stopped at " << h.stopped_at << ", output packets " << h.out_msgs.size() << "\n";
  for (uint64_t c = 0; c < all.size(); ++c) {
    for (uint64_t p = 0; p < 10; ++p) {
      auto& core = all[c]->GetCore(p);
      if (core.Bad()) continue;
      if (h.out_msgs.size() != 1 && (c < 8 || p == 4)) {
        for (auto const& request : core.GetRouter().GetXbar().InspectRequests()) {
          auto const& f = request.second;
          std::cerr << "queued xbar chip " << c << " core " << p << " input " << request.first << " path " << f.path_id << " head " << f.head << " tail " << f.tail << " mask " << f.out_mask << " size " << (f.msg ? f.msg->size : 0) << "\n";
        }
        auto state = core.GetRouter().GetReduce().Inspect();
        for (auto const& task : state.tasks) std::cerr << "reduce chip " << c << " core " << p << " path " << task.path_id << " seq " << task.reduce_seq << " expect " << task.expect_mask << " done " << task.in_done_mask << " got " << task.got[0] << "/" << task.got[1] << "/" << task.got[2] << "\n";
        for (uint64_t lane = 0; lane < 3; ++lane) {
          auto const& f = state.input_heads[lane];
          if (f.valid && f.msg) std::cerr << "queued reduce chip " << c << " core " << p << " lane " << lane << " path " << f.msg->path_id << " seq " << f.msg->reduce_seq << "\n";
        }
      }
      EXPECT_EQ(core.GetRouter().GetXbar().OverflowCount(), 0u) << "chip " << c << " core " << p;
      EXPECT_EQ(core.GetMu().Drains(), 0u) << "chip " << c << " core " << p;
      for (uint64_t s = 0; s < 16; ++s) {
        auto const& e = core.GetTs().Table().Peek(s);
        if (!core.GetTs().Cfg().SelfStartCore()) {
          EXPECT_FALSE(e.valid && e.user_id_vld) << "pending compute chip " << c << " core " << p;
        }
        if (h.out_msgs.size() != 1 && e.valid && (e.user_id_vld || e.task_id > 0)) {
          std::cerr << "pending chip " << c << " core " << p << " stream " << s
                    << " task " << e.task_id << " path " << e.task_path_id << " RV PC "
                    << core.Rv(0).Exec().Pc() << " " << core.Rv(1).Exec().Pc() << "\n";
          break;
        }
      }
    }
  }
  ASSERT_EQ(h.out_msgs.size(), 1u);
  auto const& result = *h.out_msgs.front();
  EXPECT_EQ(result.user_id, 0u);
  EXPECT_EQ(result.token_id, 1u);
  EXPECT_EQ(result.payload.size(), 6144u * 2);
  std::ofstream actual("build-ep6/actual-" + case_name + ".bf16", std::ios::binary);
  ASSERT_TRUE(actual.good());
  actual.write(reinterpret_cast<char const*>(result.payload.data()), result.payload.size());
}
}
