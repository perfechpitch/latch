// 观测层的行为基线：两条过滤规则、汇总的确定性、起跑与完成的配对、落盘格式，
// 以及"仿真期在协程里记、JoinAll 之后主线程收"这条路径本身。

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/module.h"
#include "base/runtime.h"
#include "bach/observer/jsonl_writer.h"
#include "bach/observer/span_recorder.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

std::vector<std::string> ReadLines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}

std::string OutDir() { return std::string(PROJECT_TEST_BUILD_DIR); }

// 仿真期在自己的 Cycle 里记事件的模块。记录器是它的 Cycle 独占状态，主线程
// 在 JoinAll 之后才读。
class RecordingCore : public ClkModule {
 public:
  RecordingCore(ClockPtr c, uint64_t id, const std::string& name)
      : ClkModule(c), node_id(id) {
    RegisterName(name);
  }

  void Cycle() override {
    DelayCycle(1);
    ++cyc;
    if (cyc > 3) return;
    const Time now = RT::Now();
    // 一段有时长的占用，一段等待，再加一段零长的，零长那条应该被丢掉
    rec.Span(node_id, Unit::kMatrix, cyc, cyc, now, now + 1, SpanState::kCompute, 64);
    rec.Wait(node_id, Unit::kDte, cyc, cyc, now, now + 2, WaitReason::kDownstreamCredit);
    rec.Span(node_id, Unit::kDte, cyc, cyc, now, now, SpanState::kExecution);
  }

  SpanRecorder const& Rec() const { return rec; }

 private:
  uint64_t node_id;
  SpanRecorder rec;
  uint64_t cyc = 0;
};

}  // namespace

// ---------------------------------------------------------------- 过滤规则

TEST(BachObserver, DropsSpansWithoutAttributableLength) {
  SpanRecorder rec;
  rec.Span(0, Unit::kDte, 1, 2, 10, 20, SpanState::kExecution);   // 留
  rec.Span(0, Unit::kDte, 1, 3, 20, 10, SpanState::kExecution);   // end 小于 start，丢
  rec.Span(0, Unit::kDte, 1, 4, 30, 30, SpanState::kExecution);   // 零长，丢
  rec.Span(0, Unit::kDte, 1, 5, 40, 40, SpanState::kExecution, 0, /*allow_zero=*/true);

  EXPECT_EQ(rec.Spans().size(), 2u);
  EXPECT_EQ(rec.Dropped(), 2u);
  EXPECT_EQ(rec.Spans()[0].tid, 2u);
  EXPECT_EQ(rec.Spans()[1].tid, 5u);
}

TEST(BachObserver, DropsWaitsThatDidNotActuallyWait) {
  SpanRecorder rec;
  rec.Wait(0, Unit::kCredit, 1, 2, 10, 20, WaitReason::kDownstreamCredit);  // 留
  rec.Wait(0, Unit::kCredit, 1, 3, 20, 20, WaitReason::kCreditLock);        // 丢
  rec.Wait(0, Unit::kCredit, 1, 4, 30, 20, WaitReason::kCreditLock);        // 丢

  EXPECT_EQ(rec.Waits().size(), 1u);
  EXPECT_EQ(rec.Dropped(), 2u);
}

TEST(BachObserver, RecordsNothingWhenDisabled) {
  SpanRecorder rec(false);
  rec.Span(0, Unit::kDte, 1, 2, 10, 20, SpanState::kExecution);
  rec.Wait(0, Unit::kDte, 1, 2, 10, 20, WaitReason::kCoreSetup);
  rec.GlobalStart(1, 0, 1);
  EXPECT_TRUE(rec.Spans().empty());
  EXPECT_TRUE(rec.Waits().empty());
  EXPECT_TRUE(rec.Marks().empty());
  EXPECT_EQ(rec.Dropped(), 0u);
}

TEST(BachObserver, SplitsWaitReasonsIntoDependencyAndResource) {
  EXPECT_TRUE(IsDependencyWait(WaitReason::kOwnPrevTask));
  EXPECT_TRUE(IsDependencyWait(WaitReason::kStreamPredecessor));
  EXPECT_TRUE(IsDependencyWait(WaitReason::kIncomingBarrier));
  EXPECT_FALSE(IsDependencyWait(WaitReason::kDownstreamCredit));
  EXPECT_FALSE(IsDependencyWait(WaitReason::kStreamSlot));
  EXPECT_FALSE(IsDependencyWait(WaitReason::kCoreSetup));
}

// ---------------------------------------------------------------- 汇总

TEST(BachObserver, SortsIntoATotalOrderRegardlessOfCollectOrder) {
  SpanRecorder a, b;
  a.Span(3, Unit::kDte, 1, 1, 50, 60, SpanState::kExecution);
  a.Span(3, Unit::kDte, 1, 0, 10, 20, SpanState::kExecution);
  b.Span(1, Unit::kMatrix, 2, 0, 30, 40, SpanState::kCompute);

  RunRecorder forward;
  forward.Collect(a);
  forward.Collect(b);
  forward.Finalize(0);

  RunRecorder backward;
  backward.Collect(b);
  backward.Collect(a);
  backward.Finalize(0);

  ASSERT_EQ(forward.Spans().size(), 3u);
  ASSERT_EQ(backward.Spans().size(), 3u);
  for (size_t i = 0; i < forward.Spans().size(); ++i) {
    EXPECT_EQ(forward.Spans()[i].node_id, backward.Spans()[i].node_id);
    EXPECT_EQ(forward.Spans()[i].start, backward.Spans()[i].start);
  }
  // 先按节点，再按起始时刻
  EXPECT_EQ(forward.Spans()[0].node_id, 1u);
  EXPECT_EQ(forward.Spans()[1].node_id, 3u);
  EXPECT_EQ(forward.Spans()[1].start, 10u);
  EXPECT_EQ(forward.Spans()[2].start, 50u);
}

TEST(BachObserver, PairsInjectionWithCompletion) {
  // 起跑与收齐由不同的模块记，数分片与配对都在主线程做
  SpanRecorder host, out;
  host.GlobalStart(0, 100, 1);
  host.GlobalStart(1, 200, 2);   // 这个要收够两个分片才算完成
  host.GlobalStart(2, 300, 1);   // 这个一个分片都没收到
  out.GlobalEnd(1, 900);
  out.GlobalEnd(0, 1500);
  out.GlobalEnd(1, 1600);

  RunRecorder rec;
  rec.Collect(host);
  rec.Collect(out);
  rec.Finalize(3);

  ASSERT_EQ(rec.Latency().size(), 2u);
  // 完成集合按收够那一刻排，uid 0 只要一个分片，先收够
  EXPECT_EQ(rec.Result().completed_uids, (std::vector<uint64_t>{0, 1}));
  EXPECT_EQ(rec.Result().lost_uids, (std::vector<uint64_t>{2}));
  EXPECT_EQ(rec.Result().end_time, 1600u);
  EXPECT_FALSE(rec.Result().Succeeded());

  EXPECT_EQ(rec.Latency()[0].uid, 0u);
  EXPECT_EQ(rec.Latency()[0].end - rec.Latency()[0].start, 1400u);
  EXPECT_EQ(rec.Latency()[1].uid, 1u);
  // 收够那一刻才算完成，第一个分片到得再早也不算
  EXPECT_EQ(rec.Latency()[1].end - rec.Latency()[1].start, 1400u);
  EXPECT_EQ(rec.Latency()[1].expected_fragments, 2u);
}

TEST(BachObserver, ExtraFragmentsDoNotMoveTheCompletion) {
  SpanRecorder r;
  r.GlobalStart(0, 10, 1);
  r.GlobalEnd(0, 90);
  r.GlobalEnd(0, 50);   // 只要一个分片，多出来的这条不改结论

  RunRecorder rec;
  rec.Collect(r);
  rec.Finalize(1);

  ASSERT_EQ(rec.Latency().size(), 1u);
  EXPECT_EQ(rec.Latency()[0].end, 50u);
  EXPECT_TRUE(rec.Result().Succeeded());
}

// ---------------------------------------------------------------- 落盘

TEST(BachObserver, WritesFourArtifacts) {
  SpanRecorder r;
  r.Span(2, Unit::kMatrix, 7, 3, 100, 196, SpanState::kCompute, 8192);
  r.Wait(2, Unit::kCredit, 7, 4, 200, 260, WaitReason::kDownstreamCredit);
  r.Wait(2, Unit::kScheduler, 7, 5, 300, 316, WaitReason::kOwnPrevTask);
  r.GlobalStart(7, 50, 1);
  r.GlobalEnd(7, 400);

  RunRecorder rec;
  rec.Collect(r);
  rec.Finalize(1);

  std::map<std::string, std::string> source;
  source["map_path"] = "/home/me/maps/minimal.map";
  source["compiler_sha256"] = "6ec9038";
  Params params;

  WriteRunArtifacts(OutDir(), rec, source, params);

  const std::vector<std::string> spans = ReadLines(OutDir() + "/unit_spans.jsonl");
  ASSERT_EQ(spans.size(), 2u);   // schema 行加一条记录
  EXPECT_NE(spans[0].find("\"schema\": \"unit_spans.v1\""), std::string::npos);
  EXPECT_NE(spans[1].find("\"unit\": \"MC\""), std::string::npos);
  EXPECT_NE(spans[1].find("\"state\": \"compute\""), std::string::npos);
  EXPECT_NE(spans[1].find("\"volume\": 8192"), std::string::npos);

  const std::vector<std::string> waits = ReadLines(OutDir() + "/unit_waits.jsonl");
  ASSERT_EQ(waits.size(), 3u);
  // 等待归因带一列 kind，资源与依赖分开，改参数只动得了资源那一半。
  // 两条记录按起始时刻排，start 200 的 credit 等待在前
  EXPECT_NE(waits[1].find("\"reason\": \"downstream_credit\""), std::string::npos);
  EXPECT_NE(waits[1].find("\"kind\": \"resource\""), std::string::npos);
  EXPECT_NE(waits[2].find("\"reason\": \"own_prev_task\""), std::string::npos);
  EXPECT_NE(waits[2].find("\"kind\": \"dependency\""), std::string::npos);

  const std::vector<std::string> lat = ReadLines(OutDir() + "/global_latency.jsonl");
  ASSERT_EQ(lat.size(), 2u);
  EXPECT_NE(lat[1].find("\"latency\": 350"), std::string::npos);

  const std::vector<std::string> meta = ReadLines(OutDir() + "/run_meta.json");
  std::string joined;
  for (const std::string& l : meta) joined += l;
  EXPECT_NE(joined.find("\"map_path\""), std::string::npos);
  EXPECT_NE(joined.find("minimal.map"), std::string::npos);
  EXPECT_NE(joined.find("\"dte_setup_time\": 85"), std::string::npos);
  EXPECT_NE(joined.find("\"succeeded\": true"), std::string::npos);
  EXPECT_NE(joined.find("\"completed_uids\": [7]"), std::string::npos);
  EXPECT_NE(joined.find("\"lost_uids\": []"), std::string::npos);
}

TEST(BachObserver, EscapesStringsInTheSourceBlock) {
  RunRecorder rec;
  rec.Finalize(0);
  std::map<std::string, std::string> source;
  source["note"] = "a\"b\\c";
  WriteRunArtifacts(OutDir(), rec, source, Params());

  std::string joined;
  for (const std::string& l : ReadLines(OutDir() + "/run_meta.json")) joined += l;
  EXPECT_NE(joined.find("a\\\"b\\\\c"), std::string::npos);
}

// ---------------------------------------------------------------- 跨协程

TEST(BachObserver, CollectsWhatWasRecordedInsideCoroutines) {
  RT::Reset(4, 4);
  RT::GetRecorder().Reset();

  RunRecorder rec;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    RecordingCore a(clk, 1, "bach_obs_a");
    RecordingCore b(clk, 2, "bach_obs_b");

    clk->Continue(16 * kPeriod);
    RT::JoinAll();

    // 协程里写的普通成员，JoinAll 之后主线程收是安全的
    rec.Collect(a.Rec());
    rec.Collect(b.Rec());
  }
  rec.Finalize(0);

  // 两个模块各三拍，每拍留下一条占用与一条等待，零长那条被丢
  EXPECT_EQ(rec.Spans().size(), 6u);
  EXPECT_EQ(rec.Waits().size(), 6u);
  EXPECT_EQ(rec.Dropped(), 6u);

  // 排完序之后是先节点后时刻，与两个模块被调度的先后无关
  EXPECT_EQ(rec.Spans()[0].node_id, 1u);
  EXPECT_EQ(rec.Spans()[3].node_id, 2u);
  for (size_t i = 1; i < 3; ++i) {
    EXPECT_LT(rec.Spans()[i - 1].start, rec.Spans()[i].start);
  }
}
