// 跑一份中间文件，把这一次 run 的观测写成四份产物。
//
// 模型在仿真期只把事件记在内存里：Cycle 体内除起手的 DelayCycle(1) 外不能再让出，
// 文件 IO 放进去会拖住整个 runtime 的每拍推进。所以落盘只能在 JoinAll 之后做，
// 这个工具就是那个出口，四份产物都由它写。
//
// 波形是第五份产物，由 base 的 Recorder 写，走的是另一条路：模块在 Cycle 里逐拍
// 打点，攒够一段就落盘，所以它在仿真期就在写。它的落点默认是进程当前目录下的
// Recorder.trace，一轮盖一轮，这里把前缀改到 --outdir 底下，每轮各存各的。
//
// 用法：
//
//     bach_run --in <file.bachir> --outdir <dir> [--threads N] [--coroutines N]
//              [--no-waveform]
//
// 退出码：0 表示跑完且所有 user 都完成，1 表示没跑完或有丢包，2 表示参数不对。

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "base/clock.h"
#include "base/recorder.h"
#include "base/runtime.h"
#include "base/signal_tracer.h"
#include "bach/observer/jsonl_writer.h"
#include "bach/observer/span_recorder.h"
#include "bach/sim/build.h"
#include "bach/sim/completion.h"
#include "bach/tables/loader.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

struct Options {
  std::string in;
  std::string outdir;
  uint64_t threads = 10;
  uint64_t coroutines = 10;
  bool waveform = true;
};

void Usage() {
  std::cerr << "用法: bach_run --in <file.bachir> --outdir <dir>"
            << " [--threads N] [--coroutines N] [--no-waveform]\n";
}

// 参数少，手写就够，不引第三方。缺值、认不出的名字都当错，不静默跳过。
bool ParseArgs(int argc, char** argv, Options* opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--help" || key == "-h") return false;
    if (key == "--no-waveform") {
      opt->waveform = false;
      continue;
    }
    if (i + 1 >= argc) {
      std::cerr << "参数 " << key << " 缺少取值。\n";
      return false;
    }
    const std::string value = argv[++i];
    if (key == "--in") {
      opt->in = value;
    } else if (key == "--outdir") {
      opt->outdir = value;
    } else if (key == "--threads") {
      opt->threads = std::strtoull(value.c_str(), nullptr, 10);
    } else if (key == "--coroutines") {
      opt->coroutines = std::strtoull(value.c_str(), nullptr, 10);
    } else {
      std::cerr << "认不出的参数 " << key << "。\n";
      return false;
    }
  }
  if (opt->in.empty() || opt->outdir.empty()) {
    std::cerr << "--in 与 --outdir 都是必填的。\n";
    return false;
  }
  if (opt->threads == 0 || opt->coroutines == 0) {
    std::cerr << "--threads 与 --coroutines 都要大于零。\n";
    return false;
  }
  return true;
}

void PrintSummary(BachIr const& ir, RunRecorder const& run,
                  std::string const& outdir, bool waveform) {
  RunResult const& r = run.Result();
  std::cout << "map        " << (ir.meta.count("map_path") != 0
                                     ? ir.meta.at("map_path")
                                     : std::string("(未记录)"))
            << "\n";
  std::cout << "users      " << r.completed_uids.size() << "/"
            << r.expected_users << " 完成";
  if (!r.lost_uids.empty()) std::cout << "，丢 " << r.lost_uids.size();
  std::cout << "\n";
  std::cout << "end_time   " << r.end_time << " ns\n";
  std::cout << "spans      " << run.Spans().size() << "\n";
  std::cout << "waits      " << run.Waits().size() << "\n";
  std::cout << "dropped    " << run.Dropped() << "\n";
  std::cout << "产物写到   " << outdir << "/{unit_spans,unit_waits,"
            << "global_latency}.jsonl 与 run_meta.json\n";
  if (waveform) {
    std::cout << "波形       " << outdir << "/waveform.trace"
              << "（python3 -m utils.insight inspect " << outdir
              << "/waveform）\n";
  } else {
    std::cout << "波形       关（--no-waveform）\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseArgs(argc, argv, &opt)) {
    Usage();
    return 2;
  }

  BachIr ir = LoadBachIr(opt.in);
  std::filesystem::create_directories(opt.outdir);

  RT::Reset(opt.threads, opt.coroutines);
  // 打点前先把落点定下来。第一段一旦落盘，文件就打开了，那时再改改不动。
  SetTraceDisabled(!opt.waveform);
  if (opt.waveform) RT::GetRecorder().SetPathPrefix(opt.outdir + "/waveform");
  RunRecorder run;
  bool finished = false;
  // 挂时钟的那些模块都关在这个作用域里：每个模块析构时才把它最后攒着的那一段打点
  // 交出去，而波形收尾一做完就不再接受新段落，两者顺序反了最后一段会撞在断言上。
  // RunControl 还指着 System 内部，所以它也得在这里面，不能活得比 System 久。
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    std::unique_ptr<System> sys = BuildSystem(clk, ir);
    // 判完成的是谁由中间文件自己说了算：有以太网交换节点就是它，没有就是各汇聚点。
    const CompletionAuthority who =
        ir.eth.present ? CompletionAuthority::kEth : CompletionAuthority::kOut;
    RunControl control(clk, *sys, ExpectedCompletions(ir), "run", 0, who);

    clk->Continue();
    RT::JoinAll();

    sys->Collect(run);
    run.Finalize(ir.total_users);
    finished = control.Finished();
  }
  RT::FlushRecorder();

  WriteRunArtifacts(opt.outdir, run, ir.meta, ir.params);
  PrintSummary(ir, run, opt.outdir, opt.waveform);

  if (!finished) {
    std::cerr << "这一轮没跑完：时钟停下来时还有 user 没到齐。\n";
    return 1;
  }
  return run.Result().Succeeded() ? 0 : 1;
}
