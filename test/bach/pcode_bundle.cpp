// Execute exported pcode, never the simulator's internal MoE compiler bundle.
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <map>
#include "test/bach/ip/chip/moe_common.h"
#include "test/bach/pcode_case.h"
#include "test/bach/pcode_dense_model.h"

namespace {
using namespace latch;
using namespace latch::bach;
using namespace latch::bach::moetest;

class PcodeHarness : public SpreadHarness {
 public:
  using SpreadHarness::SpreadHarness;
  std::vector<Chip*> observed_chips;
  std::set<uint64_t> observed_users;
  uint64_t compute_overlap_cycle = 0;
 protected:
  void Step() override {
    SpreadHarness::Step();
    if (observed_users.size() == 2 && !compute_overlap_cycle) {
      std::set<uint64_t> active;
      std::set<uint64_t> snapshot_users;
      for (auto* chip : observed_chips) {
        for (uint64_t core_id = 0; core_id < 10; ++core_id) {
          auto& core = chip->GetCore(core_id);
          if (core.Bad() || core.GetTs().Cfg().SelfStartCore()) continue;
          auto snapshot = core.GetTs().Table().SnapPtr()->Get();
          if (!snapshot) continue;
          for (auto const& entry : snapshot->entry) {
            if (entry.valid && entry.user_id_vld) snapshot_users.insert(entry.user_id);
            if (entry.valid && entry.user_id_vld && observed_users.count(entry.user_id))
              active.insert(entry.user_id);
          }
        }
      }
      if (CycleNow() % 1000 == 0) {
        std::cerr << "pcode snapshot active users";
        for (auto user : snapshot_users) std::cerr << " " << user;
        std::cerr << "\n";
      }
      if (active == observed_users) {
        compute_overlap_cycle = CycleNow();
        std::cerr << "pcode concurrent compute users at " << compute_overlap_cycle;
        for (auto user : active) std::cerr << " " << user;
        std::cerr << "\n";
      }
    }
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

TEST(PcodeBundle, Ep6ExportedTokens) {
  char const* root = std::getenv("PCODE_BUNDLE_ROOT");
  ASSERT_NE(root, nullptr) << "set PCODE_BUNDLE_ROOT to the exported bundle root";
  std::string case_name = std::getenv("PCODE_CASE") ? std::getenv("PCODE_CASE") : "full";
  ASSERT_TRUE(case_name == "full" || case_name == "gap");
  std::array<uint64_t,6> counts = case_name == "full" ?
      std::array<uint64_t,6>{2,2,1,1,1,1} : std::array<uint64_t,6>{2,2,0,2,0,2};
  uint64_t active_mask = 0;
  for (uint64_t ep = 0; ep < 6; ++ep) if (counts[ep]) active_mask |= 1ull << ep;
  std::unique_ptr<pcode_test::Fixture> fixture;
  if (auto path = std::getenv("PCODE_CASE_FILE")) fixture = std::make_unique<pcode_test::Fixture>(path);
  std::unique_ptr<pcode_test::Fixture> second;
  if (auto path = std::getenv("PCODE_SECOND_CASE_FILE")) {
    ASSERT_NE(fixture, nullptr);
    second = std::make_unique<pcode_test::Fixture>(path);
  }
  size_t expected_outputs = second ? 2 : 1;
  if (auto threads = std::getenv("PCODE_THREADS")) {
    uint64_t n = std::stoull(threads); ASSERT_GT(n,0u); ASSERT_LE(n,16u);
    RT::Reset(n, (600 + n - 1) / n);
  } else EnsureSlots(kChipNum * (kMaxCorePerChip + 1));
  uint64_t trace_chips = 0;
  if (auto count = std::getenv("PCODE_TRACE_CHIPS")) trace_chips = std::stoull(count);
  ASSERT_LE(trace_chips, kChipNum);
  auto trace_prefix = std::getenv("PCODE_TRACE_PREFIX");
  if (trace_prefix) TraceInto(trace_prefix);
  // Finalize after the subsequently constructed modules flush their signals,
  // while the runtime module table is still available to name the waveform.
  struct TraceFinalizer {
    bool enabled;
    ~TraceFinalizer() { if (enabled) TraceDone(); }
  } trace_finalizer{trace_chips > 0 || trace_prefix != nullptr};
  auto clk = MakeClock(0, 1);
  auto owned = MakeLpuChips(clk, trace_chips);
  std::vector<Chip*> all;
  for (auto& c : owned) all.push_back(c.get());
  LoadBundle(all, root, "pcode_ep6_tp8");
  if (std::getenv("PCODE_TRACE_HEADER"))
    for (uint64_t c = 0; c < all.size(); ++c)
      for (uint64_t p = 0; p < 10; ++p)
        std::cerr << "parser id " << all[c]->GetCore(p).GetDte().Parser().Id() << " chip " << c << " core " << p << "\n";
  if (std::getenv("PCODE_TRACE_REDUCE"))
    for (uint64_t c = 0; c < all.size(); ++c)
      for (uint64_t p = 0; p < 10; ++p)
        std::cerr << "reduce id " << all[c]->GetCore(p).GetRouter().GetReduce().Id() << " chip " << c << " core " << p << "\n";
  if (fixture) fixture->Load(all,root);
  else LoadFixtureWeights(all, root, counts);
  if (second) second->Load(all,root);
  if (auto model = std::getenv("PCODE_DENSE_MODEL")) {
    ASSERT_NE(fixture, nullptr);
    ASSERT_TRUE(fixture->weights.empty());
    pcode_test::ValidateDenseFixture(*fixture);
    if (second) {
      ASSERT_TRUE(second->weights.empty());
      pcode_test::ValidateDenseFixture(*second);
    }
    pcode_test::LoadDenseModel(all,model,root);
  }
  auto m = std::make_shared<Message>();
  m->path_id = 63;
  m->path_core_mask = active_mask;
  m->user_id = 0;
  m->token_id = 1;
  m->size = 336 + 6336;
  m->payload.assign(m->size, 0);
  for (uint64_t ep = 0; ep < 6; ++ep) {
    uint64_t count = counts[ep];
    m->payload[ep * 56] = uint8_t(count);
    for (uint64_t e = 0; e < count; ++e) {
      m->payload[ep * 56 + 2 + e * 6] = uint8_t(e);
      uint32_t w = numeric::BitsOf(0.125f);
      for (int b = 0; b < 4; ++b) m->payload[ep * 56 + 4 + e * 6 + b] = uint8_t(w >> (8*b));
    }
  }
  std::fill(m->payload.begin() + 336, m->payload.begin() + 336 + 6144, 0x38);
  std::fill(m->payload.begin() + 336 + 6144, m->payload.end(), 127);
  if (fixture) m = fixture->Message();
  C2cBridge feed(clk, "feed", StubCfg()), sink(clk, "sink", StubCfg());
  uint64_t max_cycles = 30000;
  if (char const* cap = std::getenv("PCODE_MAX_CYCLES")) max_cycles = std::stoull(cap);
  PcodeHarness h(clk, all, GridLinks(12, 4), feed, sink, 0, kChipW, 47,
                  kChipE, 2, max_cycles, m);
  std::vector<MessagePtr> inputs{m};
  if (second) inputs.push_back(second->Message());
  std::vector<uint64_t> sent_at;
  bool require_compute_overlap = false;
  if (second) {
    ASSERT_FALSE(inputs[0]->user_id == inputs[1]->user_id &&
                 inputs[0]->token_id == inputs[1]->token_id);
    std::string mode = std::getenv("PCODE_INJECT_MODE") ? std::getenv("PCODE_INJECT_MODE") : "overlap";
    ASSERT_TRUE(mode == "overlap" || mode == "sequential");
    if (mode == "overlap") {
      ASSERT_NE(inputs[0]->user_id, inputs[1]->user_id);
      h.observed_chips = all;
      h.observed_users = {inputs[0]->user_id, inputs[1]->user_id};
      // Legacy sparse campaigns only require overlapping packet lifetimes.
      // Dense GLM5 pairs explicitly request simultaneous compute occupancy.
      require_compute_overlap = std::getenv("PCODE_REQUIRE_COMPUTE_OVERLAP") != nullptr;
    }
    uint64_t gap = std::getenv("PCODE_TOKEN_GAP") ? std::stoull(std::getenv("PCODE_TOKEN_GAP")) : 1;
    h.expected_outputs = expected_outputs;
    h.input_source = [&, mode, gap](uint64_t now) -> MessagePtr {
      size_t index = sent_at.size();
      if (index == inputs.size() || now < 2 + index * gap) return {};
      if (mode == "sequential" && index > 0) {
        if (h.out_msgs.size() < index || now < h.out_at.back() + 32) return {};
        for (auto* chip : all)
          for (uint64_t core = 0; core < 10; ++core)
            if (!chip->GetCore(core).Bad() && !chip->GetCore(core).GetTs().Cfg().SelfStartCore() &&
                chip->GetCore(core).GetTs().Table().InFlight()) return {};
      }
      sent_at.push_back(now);
      std::cerr << "pcode sent token " << index << " at " << now << " user "
                << inputs[index]->user_id << " token " << inputs[index]->token_id << "\n";
      return inputs[index];
    };
  }
  clk->Continue();
  RT::JoinAll();
  if (auto prefix = std::getenv("PCODE_SNAPSHOT_PREFIX")) {
    auto dump=[](std::string const& path, ByteBlock const& bytes) {
      std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<char const*>(bytes.data()),bytes.size());
    };
    for (uint64_t chip=0;chip<48;++chip) {
      for(uint64_t p=0;p<10;++p) {
        auto base=std::string(prefix)+".c"+std::to_string(chip)+".p"+std::to_string(p);
        dump(base+".cmem",all[chip]->GetCore(p).Cmem().Peek(0,expected_outputs*65536));
        dump(base+".sf",all[chip]->GetCore(p).Cmem().PeekScale(0,expected_outputs*65536/32));
        dump(base+".smem",all[chip]->GetCore(p).Smem().Peek(0,32768));
        for(uint64_t sid=0;sid<expected_outputs;++sid)
          dump(base+".topk"+std::to_string(sid),all[chip]->GetCore(p).GetMu().EpInfo().TopkBytes(sid));
      }
      auto& mem=all[chip]->GetCore(9).Cmem();
      std::string path=std::string(prefix)+".chip"+std::to_string(chip);
      dump(path+".fc13",mem.Peek(0x6000,9*512*2));
      dump(path+".quant",mem.Peek(0,9*256));
      dump(path+".scales",mem.PeekScale(0,9*256/32));
      for (uint64_t core=0;core<10;++core)
        dump(path+".core"+std::to_string(core)+".fc2",all[chip]->GetCore(core).Cmem().Peek(0x1200,768*4));
    }
  }
  std::cerr << "pcode stopped at " << h.stopped_at << ", output packets " << h.out_msgs.size() << "\n";
  for (uint64_t c = 0; c < all.size(); ++c) {
    for (uint64_t p = 0; p < 10; ++p) {
      auto& core = all[c]->GetCore(p);
      if (core.Bad()) continue;
      if (h.out_msgs.size() == expected_outputs) {
        auto state = core.GetRouter().GetReduce().Inspect();
        EXPECT_TRUE(state.tasks.empty()) << "unfinished reduction chip " << c << " core " << p;
        EXPECT_EQ(core.GetRouter().GetReduce().OutQueued(), 0u) << "chip " << c << " core " << p;
        for (auto depth : state.input_depth)
          EXPECT_EQ(depth, 0u) << "queued reduction input chip " << c << " core " << p;
      }
      if (h.out_msgs.size() != expected_outputs) {
        auto& station = core.GetRouter().GetCoreStation();
        if (station.HeaderDepth() || station.OutBufDepth())
          std::cerr << "station chip " << c << " core " << p << " headers "
                    << station.HeaderDepth() << " payload flits " << station.OutBufDepth() << "\n";
        for (auto const& request : core.GetRouter().GetXbar().InspectRequests()) {
          auto const& f = request.second;
          std::cerr << "queued xbar chip " << c << " core " << p << " input " << request.first << " vc " << f.vc << " user " << f.user_id << " path " << f.path_id << " head " << f.head << " tail " << f.tail << " mask " << f.out_mask << " size " << (f.msg ? f.msg->size : 0) << "\n";
          auto& xbar=core.GetRouter().GetXbar();
          for (uint64_t output=0;output<kXbarOutNum;++output) {
            if (!(f.out_mask & (1ull<<output))) continue;
            std::cerr << "xbar owner chip " << c << " core " << p << " output " << output << " vc " << f.vc << " input+1 " << xbar.PacketOwner(output,f.vc);
            if (output<kR2RNum) std::cerr << " private credit " << xbar.VcCredit(output,f.vc) << " shared credit " << xbar.SharedCredit(output);
            std::cerr << "\n";
          }
        }
        auto state = core.GetRouter().GetReduce().Inspect();
        for (auto const& task : state.tasks) std::cerr << "reduce chip " << c << " core " << p << " user " << task.user_id << " path " << task.path_id << " seq " << task.reduce_seq << " expect " << task.expect_mask << " done " << task.in_done_mask << " got " << task.got[0] << "/" << task.got[1] << "/" << task.got[2] << "\n";
        for (uint64_t lane = 0; lane < 3; ++lane) {
          auto const& f = state.input_heads[lane];
          if (f.valid && f.msg) std::cerr << "queued reduce chip " << c << " core " << p << " lane " << lane << " path " << f.msg->path_id << " seq " << f.msg->reduce_seq << "\n";
        }
      }
      EXPECT_EQ(core.GetRouter().GetXbar().OverflowCount(), 0u) << "chip " << c << " core " << p;
      EXPECT_EQ(core.GetDte().Completion().Error(), 0u) << "DTE exception chip " << c << " core " << p;
      EXPECT_EQ(core.GetMu().Drains(), 0u) << "chip " << c << " core " << p;
      for (uint64_t s = 0; s < 16; ++s) {
        auto const& e = core.GetTs().Table().Peek(s);
        if (!core.GetTs().Cfg().SelfStartCore()) {
          EXPECT_FALSE(e.valid && e.user_id_vld) << "pending compute chip " << c << " core " << p;
        }
        if (h.out_msgs.size() != expected_outputs && e.valid && (e.user_id_vld || e.task_id > 0)) {
          std::cerr << "pending chip " << c << " core " << p << " stream " << s
                    << " user " << e.user_id << " task " << e.task_id << " path " << e.task_path_id << " RV PC "
                    << core.Rv(0).Exec().Pc() << " " << core.Rv(1).Exec().Pc()
                    << " fsm " << uint64_t(e.task_fsm) << " rmem " << e.rmem_busy
                    << " done " << e.done_bitmap << "\n";
          if (core.GetTs().Cfg().SelfStartCore()) break;
        }
      }
      if (h.out_msgs.size() != expected_outputs && !core.GetTs().Cfg().SelfStartCore() &&
          core.GetTs().Table().Occupied())
        std::cerr << "dte chip " << c << " core " << p << " sent "
                  << core.GetDte().OutArb().Sent() << " local forwarded "
                  << core.GetRouter().Station(3).FwdCount() << " credit requests "
                  << core.GetRouter().GetMonitor().QueueLen() << "\n";
    }
  }
  ASSERT_EQ(h.out_msgs.size(), expected_outputs);
  std::string actual_path = std::getenv("PCODE_ACTUAL") ? std::getenv("PCODE_ACTUAL") : "build-ep6/actual-" + case_name + ".bf16";
  std::set<std::pair<uint64_t,uint64_t>> seen;
  for (size_t i = 0; i < h.out_msgs.size(); ++i) {
    auto const& result = *h.out_msgs[i];
    size_t index = inputs.size();
    for (size_t j = 0; j < inputs.size(); ++j)
      if (result.user_id == inputs[j]->user_id && result.token_id == inputs[j]->token_id) index = j;
    ASSERT_LT(index, inputs.size()) << "unexpected output identity";
    ASSERT_TRUE(seen.insert({result.user_id,result.token_id}).second) << "duplicate output";
    ASSERT_EQ(result.payload.size(), 6144u * 2);
    std::cerr << "pcode received token " << index << " at " << h.out_at[i] << " user "
              << result.user_id << " token " << result.token_id << "\n";
    auto path = second ? actual_path + ".token" + std::to_string(index) + ".bf16" : actual_path;
    std::ofstream actual(path, std::ios::binary);
    ASSERT_TRUE(actual.good());
    actual.write(reinterpret_cast<char const*>(result.payload.data()), result.payload.size());
  }
  if (require_compute_overlap) ASSERT_GT(h.compute_overlap_cycle, 0u);
}
}
