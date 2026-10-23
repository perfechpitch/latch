// 48 颗 chip 的有界 exact-token 验证。普通套件里跳过实包执行。
//
// 没设 BACHFLOW_EXACT_RUN 就跳过。设了却缺 bundle、端点或两份 token，失败。
// 地址和物理 core 只从环境来。主线程从编好的包读出来再填。
// 出口期望逻辑 7（fc2_concat，chip 47 物理 8）。进口期望逻辑 8。
// 不要把出口断言成逻辑 8。用户 101 和 202 各一份文件，不改字节；更多用户轮流复用这两份。

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>

#include "bach/ip/bundle_load.h"
#include "bach/ip/chip/chip.h"
#include "bach/ip/lpu_grid.h"
#include "test/bach/ip/chip/bachflow_input.h"

using namespace latch;
using namespace latch::bach;
using namespace latch::bach::ep1exact;

namespace {

std::string ScratchOut() {
  char const *v = std::getenv("BACHFLOW_OUT_DIR");
  if (v && *v)
    return v;
  return "/tmp/ep1-exact-out";
}

std::vector<std::unique_ptr<Chip>> MakeChips(ClockPtr clk) {
  std::vector<std::unique_ptr<Chip>> owned;
  for (uint64_t i = 0; i < kChipNum; ++i) {
    ChipCfg cfg;
    cfg.gx = GxOfChip(i);
    cfg.gy = GyOfChip(i);
    cfg.core_tick = false;
    cfg.chip_tick = false;
    owned.push_back(
        std::make_unique<Chip>(clk, "chip" + std::to_string(i), cfg));
  }
  return owned;
}

// 输入 scale 旁带不由传输写，SF-one 只能由 harness 预填。gate/up 的 VU 输出会落在
// 同一个流的输入区间里，盖掉旁带，所以 SID 换了用户（allocation 变了）就得重填。
struct InputScaleCores {
  uint64_t templates = 0;
  std::set<std::pair<uint64_t, uint64_t>> cores;
};

InputScaleCores FindInputScaleCores(std::string const &path,
                                    std::vector<Chip *> const &chips) {
  std::ifstream input(path);
  LOGCHECK(input.good(), "读不出输入 scale 模板。");
  std::map<std::tuple<uint64_t, uint64_t, uint64_t>, std::map<uint64_t, uint64_t>> templates;
  std::string line;
  while (std::getline(input, line)) {
    std::istringstream row(line);
    std::string tag;
    uint64_t chip, core, unit, offset, value;
    if (!(row >> tag) || tag != "DSAREG") continue;
    if (!(row >> chip >> core >> unit >> offset >> value) || unit != 1) continue;
    if (offset < kMuTemplateBase ||
        offset >= kMuTemplateBase + kMuTemplateNum * kMuTemplateStride) continue;
    uint64_t relative = offset - kMuTemplateBase;
    templates[{chip, core, relative / kMuTemplateStride}][relative % kMuTemplateStride] = value;
  }
  InputScaleCores found;
  for (auto const &entry : templates) {
    auto const &words = entry.second;
    auto mode_pos = words.find(kMuPrimitiveMode - kMuPrimitiveDim);
    if (mode_pos == words.end()) continue;
    uint64_t mode = mode_pos->second;
    // 只预填完整广播 token 区间，不只填本核的 K 切片。
    // FC2 的 ep_reduce 输入由 VU 产生，不能覆盖其 scale。
    if (!(mode & (1ull << kMuADataTypeShift)) || (mode & kMuRouterEpReduceEn)) continue;
    uint64_t dim = words.at(0), base = MuStripDocBase(words.at(kMuAAddr - kMuPrimitiveDim));
    uint64_t stride = words.at(kMuAStreamStride - kMuPrimitiveDim);
    uint64_t k = (dim >> kMuKblockShift) * ((mode & 1) ? 64 : 128);
    auto [chip, core, index] = entry.first;
    LOGCHECK(chip < chips.size() && core < chips[chip]->CoreNum() &&
                 k && k % 32 == 0 && base < kMuDocCmemBytes &&
                 stride == kInputStride && base + k <= kDataBytes,
             "输入 scale 模板不在完整广播 token 区间内。");
    found.cores.insert({chip, core});
    ++found.templates;
  }
  return found;
}

// JoinAll 之后的现场：只写没走空的 core。打拍寄存器此时停在复位值，所以占用
// 读 Occupied()、表项读 Peek()，不读 InFlight() 或 snapshot。
void DumpBusyCore(std::ostream &o, uint64_t c, uint64_t p, Core &core) {
  Router &r = core.GetRouter();
  Xbar &xb = r.GetXbar();
  CoreStation &cs = r.GetCoreStation();
  auto red = r.GetReduce().Inspect();
  uint64_t occupied = core.GetTs().Table().Occupied();
  bool queued = cs.OutBufDepth() || cs.HeaderDepth() || r.GetReduce().OutQueued() ||
                red.input_depth[0] || red.input_depth[1] || red.input_depth[2];
  for (uint64_t d = 0; d < 4; ++d) queued |= r.Station(d).QueueDepth() != 0;
  for (uint64_t d = 0; d < kXbarInNum; ++d) queued |= xb.InDepth(d) != 0;
  if (!occupied && !queued && red.tasks.empty()) return;
  o << c << ' ' << p << " dte_error " << core.GetDte().Completion().Error()
    << " occupied " << occupied << " mu " << core.GetMu().DoneCnt() << '/'
    << core.GetMu().DispatchCnt() << " rdc_ctx " << r.GetReduce().ContextUsed();
  for (uint64_t u = 0; u < 3; ++u)
    o << " pc" << u << ' ' << core.Rv(u).Exec().Pc();
  o << '\n';
  for (uint64_t sid = 0; sid < kStreamNum; ++sid) {
    StreamEntry const &e = core.GetTs().Table().Peek(sid);
    if (!e.valid) continue;
    o << "  task sid " << sid << " user " << e.user_id << (e.user_id_vld ? "" : "?")
      << " t" << e.task_id << " fsm " << int(e.task_fsm) << " done 0x" << std::hex
      << e.done_bitmap << std::dec << " path " << e.task_path_id << '\n';
  }
  if (!queued && red.tasks.empty()) return;
  o << "  router q";
  for (uint64_t d = 0; d < 4; ++d) {
    uint64_t hp = 0, hu = 0;
    o << ' ' << r.Station(d).QueueDepth();
    if (r.Station(d).QueueHead(hp, hu)) o << "(p" << hp << "u" << hu << ")";
  }
  o << " xb";
  for (uint64_t d = 0; d < kXbarInNum; ++d)
    if (xb.InDepth(d)) o << ' ' << d << ':' << xb.InDepth(d);
  o << " cbuf " << cs.OutBufDepth() << " hdr " << cs.HeaderDepth() << " rdc_in "
    << red.input_depth[0] << ' ' << red.input_depth[1] << ' ' << red.input_depth[2]
    << " rdc_outq " << r.GetReduce().OutQueued();
  for (auto const &t : red.tasks)
    o << " red u" << t.user_id << " p" << t.path_id << ' ' << t.received << '/' << t.need;
  o << '\n';
}

} // namespace

TEST(Ep1ExactBundle, RunsPackedTokensWithObservedOwnership) {
  DteInputModeScope input_mode;
  if (std::getenv("BACHFLOW_EXACT_RUN") == nullptr) {
    GTEST_SKIP() << "没设 BACHFLOW_EXACT_RUN，不装 48 chip。";
  }
  std::string root = EnvOr("BACHFLOW_BUNDLE_ROOT", "");
  std::string name = EnvOr("BACHFLOW_BUNDLE_NAME", "");
  if (root.empty() || name.empty()) {
    FAIL() << "设了 BACHFLOW_EXACT_RUN 但没有 BACHFLOW_BUNDLE_ROOT / NAME。";
  }
  Endpoint in = IngressFromEnv();
  Endpoint out = EgressFromEnv();
  if (!EndpointSet(in) || !EndpointSet(out)) {
    FAIL() << "进核与出核的 chip / physical / addr 必须由环境给出。";
  }
  if (in.chip >= kChipNum || out.chip >= kChipNum ||
      in.physical >= kChipCoreNum || out.physical >= kChipCoreNum) {
    FAIL() << "chip 或物理 core 越界。";
  }
  int64_t want_in = static_cast<int64_t>(EnvU64("BACHFLOW_INGRESS_LOGICAL", 8));
  int64_t want_out = static_cast<int64_t>(EnvU64("BACHFLOW_EGRESS_LOGICAL", 7));
  if (want_out == 8) {
    FAIL() << "出口不是逻辑 8。fc2_concat 在逻辑 7。";
  }

  std::string golden = EnvOr("BACHFLOW_GOLDEN_DIR", "");
  if (golden.empty()) {
    FAIL() << "设了 BACHFLOW_EXACT_RUN 但没有 BACHFLOW_GOLDEN_DIR。";
  }
  // 用户 0、1 号是 101、202，第 k 个（k >= 2）是 300 + k。只有 101、202 两份
  // token：偶数 k 用 101 那份，奇数 k 用 202 那份，后面的输出该与同份 token 的一致。
  // pcode 进口按用户号索引 1024 项的流表（不取模），用户号必须 < 1024。
  uint64_t user_count = EnvU64("BACHFLOW_USER_COUNT", 1);
  if (user_count < 1 || user_count > kMaxUsers) {
    FAIL() << "BACHFLOW_USER_COUNT 只能是 1.." << kMaxUsers << "。";
  }
  std::vector<uint8_t> body101, body202;
  std::string why;
  ASSERT_TRUE(LoadSavedFile(golden + "/user101.token", &body101, &why)) << why;
  if (user_count >= 2) {
    ASSERT_TRUE(LoadSavedFile(golden + "/user202.token", &body202, &why)) << why;
    EXPECT_NE(body101, body202);
  }
  std::vector<uint64_t> users;
  std::map<uint64_t, std::vector<uint8_t> const *> body_of;
  for (uint64_t k = 0; k < user_count; ++k) {
    users.push_back(ExactUserId(k));
    body_of[users.back()] = k % 2 == 0 ? &body101 : &body202;
  }

  EnsureSlots(kChipNum * (kMaxCorePerChip + 1));
  ClockPtr clk = MakeClock(0, kPeriod);
  TraceOffScope trace_off;
  auto owned = MakeChips(clk);
  std::vector<Chip *> all;
  for (auto const &c : owned)
    all.push_back(c.get());
  ASSERT_EQ(all.size(), 48u);

  uint64_t weight_count = 0;
  LogicalMap logical = LoadAdapterInput(all, root, name, &weight_count);
  ASSERT_FALSE(logical.empty());
  ASSERT_GE(weight_count, 384u * 9u);
  int64_t got_in = -2, got_out = -2;
  ASSERT_TRUE(LogicalOf(logical, in.chip, in.physical, &got_in));
  ASSERT_TRUE(LogicalOf(logical, out.chip, out.physical, &got_out));
  EXPECT_EQ(got_in, want_in);
  EXPECT_EQ(got_out, want_out);
  EXPECT_NE(got_out, int64_t{8});

  Core &ingress = all[in.chip]->GetCore(in.physical);
  ASSERT_TRUE(PathFree(ingress, kIngressPath))
      << "PathID " << kIngressPath << " 已被占用，拒绝覆盖";
  ingress.GetRouter().Preload(kIngressPath, IngressEntry());

  C2cBridge feed(clk, "feed", StubCfg());
  C2cBridge sink(clk, "sink", StubCfg());
  uint64_t limit = EnvU64("BACHFLOW_CYCLE_LIMIT", 3000);
  ExactHarness harness(clk, all, GridLinks(kGridY, kGridX), feed, sink, in.chip,
                       kChipW, out.chip, kChipE,
                       all[out.chip]->GetCore(out.physical), out.physical,
                       limit);
  harness.expected = user_count;
  harness.max_in_flight = EnvU64("BACHFLOW_MAX_IN_FLIGHT", ~uint64_t{0} / 2);
  if (harness.max_in_flight == 0) {
    FAIL() << "BACHFLOW_MAX_IN_FLIGHT 不能是 0。";
  }
  std::filesystem::create_directories(ScratchOut());
  std::ofstream transport(ScratchOut() + "/transport.txt");
  ASSERT_TRUE(transport.good());
  transport << "seg_shift " << DteSegValidShift() << " template_mode "
            << ingress.GetDte().Regfile().ConfigRead(0x1048) << '\n';
  uint64_t last_desc = 0, last_rx = 0, last_error = 0;
  // 0 = 关。MU 派发与完成总数连续这么多拍不变，就当卡死提前停钟。
  uint64_t stall_cycles = EnvU64("BACHFLOW_STALL_CYCLES", 0);
  uint64_t last_mu = 0, last_mu_cycle = 0;
  uint64_t inlet_count = 0;
  std::set<uint64_t> inlet_users, broadcast_users;
  auto compute_pos = std::find_if(logical.begin(), logical.end(),
      [](auto const &entry) { return entry.first.first == 0 && entry.second == 0; });
  ASSERT_NE(compute_pos, logical.end());
  Core &first_compute = all[0]->GetCore(compute_pos->first.second);
  bool transport_only = EnvU64("BACHFLOW_TRANSPORT_ONLY", 0) != 0;
  bool input_sf_prefill = EnvU64("BACHFLOW_INPUT_SF_PREFILL", 0) != 0;
  bool credit_divert = EnvU64("BACHFLOW_CREDIT_DIVERT", 1) != 0;
  for (auto *chip : all)
    for (uint64_t p = 0; p < chip->CoreNum(); ++p)
      chip->GetCore(p).GetRouter().GetXbar().SetCreditDivert(credit_divert);
  transport << "credit_divert " << (credit_divert ? 1 : 0) << '\n';
  InputScaleCores scale_cores;
  std::map<std::pair<std::pair<uint64_t, uint64_t>, uint64_t>, uint64_t> scale_allocation;
  uint64_t scale_refills = 0;
  if (!transport_only) {
    ASSERT_GE(out.addr, kMuDocCmemBase);
    ASSERT_LT(out.addr, kMuDocCmemBase + kMuDocCmemBytes);
    harness.output_addr = out.addr - kMuDocCmemBase;
    harness.output_bytes = kDataBytes * 2;
  }
  if (input_sf_prefill) {
    ASSERT_TRUE(std::all_of(body101.end() - kScaleBytes, body101.end(),
                           [](uint8_t value) { return value == 0x7f; }));
    if (user_count >= 2) {
      ASSERT_TRUE(std::all_of(body202.end() - kScaleBytes, body202.end(),
                             [](uint8_t value) { return value == 0x7f; }));
    }
    scale_cores = FindInputScaleCores(root + "/" + name + "/" + name + ".bachir", all);
    ASSERT_EQ(scale_cores.templates, 256u) << "Gate/up input templates only; no FC2/VU scale writes.";
    transport << "input_sf_prefill 1 templates " << scale_cores.templates << " cores "
              << scale_cores.cores.size() << '\n';
  }
  harness.observe = [&](uint64_t cycle) {
    for (auto const &at : scale_cores.cores) {
      Core &core = all[at.first]->GetCore(at.second);
      auto snap = core.GetTs().Table().SnapPtr()->Get();
      if (!snap) continue;
      for (uint64_t sid = 0; sid < snap->slot_num && sid < kInputStreams; ++sid) {
        StreamEntry const &e = snap->entry[sid];
        if (!e.valid) continue;
        uint64_t &seen = scale_allocation[{at, sid}];
        if (seen == e.allocation) continue;
        seen = e.allocation;
        core.Cmem().PokeScale(sid * kInputStride, ByteBlock(kScaleBytes, kSavedScaleByte));
        ++scale_refills;
      }
    }
    if (cycle % 256 == 0) {
      uint64_t instructions = 0, mu_dispatch = 0, mu_done = 0;
      for (auto *chip : all) {
        for (uint64_t p = 0; p < chip->CoreNum(); ++p) {
          auto &core = chip->GetCore(p);
          for (uint64_t unit = 0; unit < 3; ++unit)
            instructions += core.Rv(unit).Exec().Insts();
          mu_dispatch += core.GetMu().DispatchCnt();
          mu_done += core.GetMu().DoneCnt();
        }
      }
      transport << "progress cycle " << cycle << " insts " << instructions
                << " mu_dispatch " << mu_dispatch << " mu_done " << mu_done << '\n';
      transport.flush();
      // RV 核在死锁时仍会轮询，指令数照涨；只有 MU 计数停住才算不再前进。
      if (mu_dispatch + mu_done != last_mu) {
        last_mu = mu_dispatch + mu_done;
        last_mu_cycle = cycle;
      } else if (stall_cycles != 0 && cycle - last_mu_cycle >= stall_cycles) {
        transport << "stall cycle " << cycle << " mu_frozen_since "
                  << last_mu_cycle << '\n';
        transport.flush();
        harness.halted = true;
        return;
      }
    }
    auto &dte = ingress.GetDte();
    auto &port = dte.FromRv();
    if (port.Valid() && port.Seq() != last_desc) {
      last_desc = port.Seq();
      auto d = port.Desc();
      transport << "descriptor " << cycle << " seq " << last_desc
                << " task " << d->task_id << " route " << uint64_t(d->route)
                << " error " << d->error;
      for (uint64_t i = 0; i < 4; ++i)
        transport << " seg" << i << ' ' << d->seg[i].valid << ' '
                  << d->seg[i].len << ' ' << d->seg[i].dst_addr;
      transport << '\n';
    }
    auto rx = ReadCoreData(dte.FromRouter());
    if (rx.valid && rx.seq != last_rx) {
      last_rx = rx.seq;
      transport << "rx " << cycle << " seq " << rx.seq << " bytes "
                << rx.bytes << " header " << rx.hdr << " last " << rx.last
                << " total " << (rx.msg ? rx.msg->TotalBytes() : 0) << '\n';
    }
    if (dte.ToTs().Valid() && dte.ToTs().task_id.Get() == 63 &&
        inlet_count < user_count) {
      uint64_t slot = inlet_count++;
      uint64_t user = users[slot];
      auto const &body = *body_of.at(user);
      auto topk = ingress.Cmem().Peek(0x8000 + slot * kTopkBytes, kTopkBytes);
      auto data_sf = ingress.Mmem().Peek(slot * (kDataBytes + kScaleBytes),
                                         kDataBytes + kScaleBytes);
      ByteBlock want_topk(body.begin(), body.begin() + kTopkBytes);
      ByteBlock want_data_sf(body.begin() + kTopkBytes, body.end());
      EXPECT_EQ(topk, want_topk);
      EXPECT_EQ(data_sf, want_data_sf);
      auto header = ingress.Cmem().Peek(slot * 32, 32);
      EXPECT_EQ(uint64_t(header[8]) | uint64_t(header[9]) << 8, user);
      inlet_users.insert(user);
      transport << "inlet_check " << cycle << " user " << user
                << " slot " << slot << " topk " << (topk == want_topk)
                << " data_sf " << (data_sf == want_data_sf) << '\n';
    }
    auto &ack = first_compute.GetDte().ToTs();
    if (ack.Valid() && ack.task_id.Get() == 0 &&
        !broadcast_users.count(ack.user_id.Get())) {
      uint64_t user = ack.user_id.Get(), sid = ack.stream_id.Get();
      EXPECT_TRUE(harness.requested_users.count(user));
      auto found_body = body_of.find(user);
      if (found_body == body_of.end() || found_body->second->size() != kSavedBytes) {
        ADD_FAILURE() << "Unexpected broadcast user " << user;
        harness.halted = true;
        return;
      }
      auto const &body = *found_body->second;
      auto topk = first_compute.GetMu().EpInfo().TopkBytes(sid);
      ByteBlock want_topk(body.begin(), body.begin() + kTopkBytes);
      auto got = first_compute.Cmem().Peek(sid * 0x10000, kDataBytes + kScaleBytes);
      ByteBlock want(body.begin() + kTopkBytes, body.end());
      EXPECT_GE(topk.size(), kTopkBytes);
      if (topk.size() >= kTopkBytes) {
        EXPECT_EQ(ByteBlock(topk.begin(), topk.begin() + kTopkBytes), want_topk);
      }
      EXPECT_EQ(got, want);
      auto associated_sf = first_compute.Cmem().PeekScale(sid * 0x10000, kScaleBytes);
      ByteBlock want_sf(body.end() - kScaleBytes, body.end());
      EXPECT_EQ(associated_sf, want_sf);
      broadcast_users.insert(user);
      transport << "broadcast_check " << cycle << " sid " << sid
                << " user " << user << " topk_bytes " << topk.size()
                << " data_sf " << (got == want)
                << " associated_sf " << (associated_sf == want_sf) << '\n';
      if (transport_only && broadcast_users == harness.requested_users) {
        harness.halted = true;
        clk->Stop();
      }
    }
    if (dte.Completion().Error() != last_error) {
      last_error = dte.Completion().Error();
      transport << "error " << cycle << ' ' << last_error << '\n';
    }
  };
  for (uint64_t user : users) {
    harness.saved.push_back(*body_of.at(user));
    harness.queue.push_back(
        ExactToken(user, kIngressPath, in.addr, *body_of.at(user), user));
  }
  harness.watches.push_back(
      {in.chip, in.physical, &all[in.chip]->GetCore(in.physical)});
  harness.watches.push_back(
      {out.chip, out.physical, &all[out.chip]->GetCore(out.physical)});
  harness.watches.push_back({0, compute_pos->first.second, &first_compute});

  harness.requested_users = std::set<uint64_t>(users.begin(), users.end());

  // 检查排队身份后执行有界时钟。
  EXPECT_EQ(harness.queue.size(), user_count);
  for (uint64_t k = 0; k < user_count; ++k) {
    EXPECT_EQ(harness.queue[k]->user_id, users[k]);
    EXPECT_EQ(harness.queue[k]->size, kSavedBytes);
  }
  if (user_count >= 2) {
    EXPECT_NE(harness.queue[0]->payload, harness.queue[1]->payload);
  }

  std::filesystem::create_directories(ScratchOut());
  clk->Continue(limit + 2);
  RT::JoinAll();
  RunResult const &note = harness.result;
  transport << "input_sf_refills " << scale_refills << '\n';
  std::ofstream diagnostics(ScratchOut() + "/cores.txt");
  ASSERT_TRUE(diagnostics.good());
  for (uint64_t c = 0; c < all.size(); ++c) {
    for (uint64_t p = 0; p < all[c]->CoreNum(); ++p) {
      if (!all[c]->GetCore(p).Bad())
        DumpBusyCore(diagnostics, c, p, all[c]->GetCore(p));
    }
  }
  WriteRecords(ScratchOut() + "/run.txt", note, users);
  WriteBf16(ScratchOut(), note);
  if (transport_only) {
    EXPECT_EQ(inlet_users, harness.requested_users);
    EXPECT_EQ(broadcast_users, harness.requested_users);
    EXPECT_EQ(ingress.GetDte().Completion().Error(), 0u);
    EXPECT_EQ(first_compute.GetDte().Completion().Error(), 0u);
  } else {
    EXPECT_TRUE(note.completed_by_end) << "stopped at cycle " << note.stopped_at;
    EXPECT_EQ(note.stored_outputs.size(), user_count);
    for (uint64_t user : users) {
      auto found = note.stored_outputs.find(user);
      ASSERT_NE(found, note.stored_outputs.end());
      EXPECT_EQ(found->second.bytes.size(), kDataBytes * 2);
    }
  }
  RT::Reset();
}
