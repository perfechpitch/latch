// RV32IM 流水线的测试：把 src/module/rv32_kernel 编出来的 C++ demo 装进 ILM，
// 按拍跑到程序自己写退出寄存器为止，核对它的 UART 输出与退出码，
// 再看一眼波形里八个模块的信号都在。
//
// 跑两遍：第一遍关掉波形，量的是模型本身从第一拍到最后一拍的速度；
// 第二遍开着波形，量记录波形之后的速度，并检查波形内容。两遍的周期数与
// 退休指令数必须一模一样，记录波形不该改变任何行为。

#include "module/rv32_pipeline.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>

#include "base/clock.h"
#include "base/recorder.h"
#include "base/runtime.h"
#include "gtest/gtest.h"
#include "utils/trace_reader.h"

using namespace latch;

namespace {

// demo kernel 的二进制镜像，随仓库提交，重新生成见同目录的 Makefile
std::string KernelBinPath() {
  return std::string(PROJECT_TEST_DIR) + "/../src/module/rv32_kernel/kernel.bin";
}

// 跑完一遍之后拷出来的东西，core 已经析构，主线程随便读
struct RunStat {
  bool loaded = false;
  double ms = 0;            // 从第一拍到最后一拍的墙钟耗时
  uint64_t cycles = 0;
  uint64_t instret = 0;
  Rv32Halt halt = Rv32Halt::kRunning;
  uint32_t exit_code = 0;
  uint32_t sp = 0;
  std::string uart;
  std::string stats;

  double MCyclePerSec() const { return ms > 0 ? cycles / ms / 1000.0 : 0; }
  double MInstPerSec() const { return ms > 0 ? instret / ms / 1000.0 : 0; }
  double NsPerCycle() const { return cycles ? ms * 1e6 / cycles : 0; }
};

RunStat RunKernel() {
  // 八个模块就是八个协程，sub_thread × co_thread 是执行槽总数，少于模块数
  // 会直接死锁。6 × 4 = 24 槽，是本机实测里最快的一档——不过从 4×4 到 32×2
  // 各档之间只差百分之几，基本都在测量噪声里，够用就行。
  RT::Reset(6, 4);

  ClockPtr clk = MakeClock(0, 10);
  Rv32Core core(clk, "rv32_core");

  RunStat s;
  s.loaded = core.LoadIlmBin(KernelBinPath());
  if (!s.loaded) return s;

  const auto t0 = std::chrono::steady_clock::now();
  clk->Continue();
  RT::JoinAll();
  const auto t1 = std::chrono::steady_clock::now();

  s.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  s.cycles = core.Cycles();
  s.instret = core.Instret();
  s.halt = core.HaltReason();
  s.exit_code = core.ExitCode();
  s.sp = core.Reg(2);
  s.uart = core.UartOut();
  s.stats = core.StatsText();
  return s;
}

void ReportSpeed(const char* what, const RunStat& s) {
  std::printf("  %-12s %7llu cycles / %7llu instret  %8.2f ms  "
              "%7.2f Mcycle/s  %7.2f Minst/s  %7.1f ns/cycle\n",
              what, static_cast<unsigned long long>(s.cycles),
              static_cast<unsigned long long>(s.instret), s.ms,
              s.MCyclePerSec(), s.MInstPerSec(), s.NsPerCycle());
}

}  // namespace

TEST(Rv32Pipeline, RunsCppKernel) {
  // ---- 第一遍：不记波形

  SetTraceDisabled(true);
  const RunStat fast = RunKernel();
  SetTraceDisabled(false);
  ASSERT_TRUE(fast.loaded) << "装载 " << KernelBinPath()
                           << " 失败，先到 src/module/rv32_kernel 跑 make";

  // ---- 第二遍：记波形

  RT::GetRecorder().Reset();
  const std::string prefix = RT::GetRecorder().PathPrefix();
  const RunStat full = RunKernel();
  RT::FlushRecorder();
  ASSERT_TRUE(full.loaded);

  std::cout << "\n=== UART ===\n" << full.uart << "\n=== stats ===\n"
            << full.stats << "\n\n=== sim speed ===" << std::endl;
  ReportSpeed("no trace", fast);
  ReportSpeed("with trace", full);
  std::printf("  trace overhead x%.2f\n\n",
              fast.ms > 0 ? full.ms / fast.ms : 0.0);

  // ---- 程序确实跑对了

  EXPECT_EQ(full.halt, Rv32Halt::kSimExit);
  EXPECT_EQ(full.exit_code, 0u) << "demo 自检有用例没过";
  EXPECT_NE(full.uart.find("PASS"), std::string::npos);
  EXPECT_EQ(full.uart.find("FAIL"), std::string::npos);
  EXPECT_GT(full.instret, 1000u);
  EXPECT_GT(full.cycles, full.instret);  // 有停顿有冲刷，CPI 大于 1
  // 栈确实用起来了，且没跑出 DLM
  EXPECT_GE(full.sp, Rv32Config().dlm_base);
  EXPECT_LT(full.sp, Rv32Config().dlm_base + Rv32Config().dlm_size);

  // 记不记波形，跑出来的东西必须一模一样
  EXPECT_EQ(fast.cycles, full.cycles);
  EXPECT_EQ(fast.instret, full.instret);
  EXPECT_EQ(fast.uart, full.uart);

  // ---- 波形：八个模块各自的信号都该在里面

  const std::string trace_path = prefix + ".trace";
  std::cout << "=== trace ===\n  " << trace_path << "\n" << std::endl;

  auto data = TraceSlurp(trace_path);
  ASSERT_GT(data.size(), 16u);
  auto meta = TraceParseMeta(data);

  uint64_t core_id = 0;
  for (const auto& r : meta) {
    if (r.name == "rv32_core" && r.parent_id == 0) core_id = r.id;
  }
  ASSERT_NE(core_id, 0u) << "波形里没有 rv32_core";

  std::map<std::string, uint64_t> module_id;
  for (const auto& r : meta) {
    if (r.parent_id == core_id) module_id[r.name] = r.id;
  }
  for (const auto& m : {"if", "id", "ex", "mem", "wb", "ilm", "dlm", "mmio"}) {
    EXPECT_TRUE(module_id.count(m)) << "波形里没有模块: " << m;
  }

  std::map<uint64_t, std::string> owner;
  for (const auto& kv : module_id) owner[kv.second] = kv.first;
  std::map<std::string, uint64_t> sig_id;
  for (const auto& r : meta) {
    auto it = owner.find(r.parent_id);
    if (it != owner.end()) sig_id[it->second + "." + r.name] = r.id;
  }
  EXPECT_GT(sig_id.size(), 100u);

  auto idx = TraceParseDataIndex(data);
  auto seg_max = [&](const std::string& name) {
    auto s = sig_id.find(name);
    if (s == sig_id.end()) return uint64_t(0);
    auto it = idx.find(s->second);
    if (it == idx.end() || it->second.empty()) return uint64_t(0);
    return it->second.back().v_max;
  };

  EXPECT_EQ(seg_max("id.x0_zero"), 0u) << "x0 必须恒为 0";
  EXPECT_GT(seg_max("if.req"), 0u);
  EXPECT_GT(seg_max("id.stall"), 0u);
  EXPECT_GT(seg_max("ex.branch_taken"), 0u);
  EXPECT_GT(seg_max("ex.fwd_a"), 0u);
  EXPECT_GT(seg_max("mem.en"), 0u);
  EXPECT_GE(seg_max("wb.instret"), 1000u);
  EXPECT_GT(seg_max("ilm.i_en"), 0u);
  EXPECT_GT(seg_max("dlm.d_we"), 0u);
  EXPECT_GT(seg_max("mmio.uart_write"), 0u);

  std::cout << "共 " << sig_id.size() << " 根信号，" << module_id.size()
            << " 个模块，波形 " << data.size() / 1024 << " KB\n" << std::endl;
}

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
