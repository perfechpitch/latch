// 跑一份 kernel 文件，把这一轮的时间与瓶颈写成观测产物。
//
// 核只做 FFN，前后的计算在外围模块里，它照文件里的时序表按拍把输入喂进阵列、再把结果
// 收回来。每个核是一个挂时钟的节点，由 runtime 每拍驱动它的 Cycle。
//
// 时钟的粒度可调，默认 100。模块内部算的还是精确的拍数，只是每隔 granularity 拍才醒
// 一次看状态，所以一件事做完到被看见之间最多差一个粒度。粒度取 1 就是逐拍，最准也最
// 慢；取大一些唤醒次数成比例地少，代价是比粒度还短的那些事（一次 setup、一跳线延迟）
// 会被抹平。同一份输入实测：粒度 1 要 24.5 秒，粒度 100 只要 0.25 秒，end_time 高
// 出 0.3%。看波形、做回放用 1，试参数用默认的就够。
//
// 观测走两条路。四份 jsonl 由各模块在仿真期记在自己的 SpanRecorder 上，JoinAll 之后
// 由主线程收上来写盘：Cycle 体内除起手的 DelayCycle(1) 外不能再让出，文件 IO 放进去
// 会拖住整个 runtime 的每拍推进。波形由 base 的 Recorder 写，模块在信号跳变的时刻打点，
// 攒够一段就落盘，所以它在仿真期就在写。
//
// 挂时钟的模块每拍各要占住一个协程：每个核一个，外围模块与停机判据各一个。协程槽不够
// 时多出来的模块每拍都得排队等一个空的，同一份输入能从半分钟变成跑不完，所以协程数不
// 由使用者记着调，按阵列的规模算出来；--coroutines 给的值只当下限。
//
// 输入与产物都有默认路径，一条链上下一步接着上一步，平时不用填：编译器默认把 kernel
// 写到 build/playback/deepseek.bachk，这里默认读它、默认把观测产物写回同一个目录。
//
// 用法：
//
//     ksim_run [--in <file.bachk>] [--outdir <dir>] [--no-waveform]
//              [--chips RxC] [--cores RxC] [--bw N] [--pcie-bw N]
//              [--pcie-lat N] [--hop N] [--setup N] [--credit N] [--limit N]
//              [--threads N] [--coroutines N]
//
// 退出码：0 表示跑完，1 表示到上限还没跑完，2 表示参数不对。

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
#include "bach/common/params.h"
#include "bach/ksim/loader.h"
#include "bach/ksim/system.h"
#include "bach/observer/jsonl_writer.h"

using namespace latch;
using namespace latch::bach;
using namespace latch::bach::ksim;

namespace {

struct Options {
  // 默认跟编译器那头对上，见文件开头。
  std::string in = "build/playback/deepseek.bachk";
  std::string outdir = "build/playback";
  bool waveform = true;
  Topology topo{16, 4, 2, 4};
  RouterParams rp{32, 1, 32, 400};
  CoreParams cp{4, 32};
  uint64_t credit = 2;
  Time limit = 100000000;
  uint64_t threads = 10;
  // 零表示按阵列的规模算，见 CoroutinesFor。
  uint64_t coroutines = 0;
  Time granularity = 100;
};

void Usage() {
  std::cerr << "用法: ksim_run [--in <file.bachk>] [--outdir <dir>] [--no-waveform]"
            << " [--chips RxC] [--cores RxC] [--bw N] [--pcie-bw N]"
            << " [--pcie-lat N] [--hop N] [--setup N] [--credit N]"
            << " [--limit N] [--threads N] [--coroutines N]"
            << " [--granularity N]\n";
}

// "16x4" 拆成两个数。
bool ParsePair(std::string const& v, uint64_t* a, uint64_t* b) {
  const std::size_t x = v.find('x');
  if (x == std::string::npos) return false;
  *a = std::strtoull(v.substr(0, x).c_str(), nullptr, 10);
  *b = std::strtoull(v.substr(x + 1).c_str(), nullptr, 10);
  return *a > 0 && *b > 0;
}

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
    const std::string v = argv[++i];
    const uint64_t n = std::strtoull(v.c_str(), nullptr, 10);
    if (key == "--in") {
      opt->in = v;
    } else if (key == "--outdir") {
      opt->outdir = v;
    } else if (key == "--chips") {
      if (!ParsePair(v, &opt->topo.chip_rows, &opt->topo.chip_cols)) return false;
    } else if (key == "--cores") {
      if (!ParsePair(v, &opt->topo.core_rows, &opt->topo.core_cols)) return false;
    } else if (key == "--bw") {
      opt->rp.bandwidth = n;
      opt->cp.link_bw = n;
    } else if (key == "--pcie-bw") {
      opt->rp.pcie_bandwidth = n;
    } else if (key == "--pcie-lat") {
      opt->rp.pcie_latency = n;
    } else if (key == "--hop") {
      opt->rp.hop_latency = n;
    } else if (key == "--setup") {
      opt->cp.dte_setup = n;
    } else if (key == "--credit") {
      opt->credit = n;
    } else if (key == "--limit") {
      opt->limit = n;
    } else if (key == "--threads") {
      opt->threads = n;
    } else if (key == "--coroutines") {
      opt->coroutines = n;
    } else if (key == "--granularity") {
      opt->granularity = n;
    } else {
      std::cerr << "认不出的参数 " << key << "。\n";
      return false;
    }
  }
  if (opt->granularity == 0) {
    std::cerr << "--granularity 要大于零。\n";
    return false;
  }
  if (opt->threads == 0) {
    std::cerr << "--threads 要大于零。\n";
    return false;
  }
  if (opt->in.empty() || opt->outdir.empty()) {
    std::cerr << "--in 与 --outdir 给了就不能是空的。\n";
    return false;
  }
  return true;
}

// 每线程该开几个协程。挂时钟的模块摊到各线程上，每个模块一个，命令行给的值只当下限。
uint64_t CoroutinesFor(Options const& opt, uint64_t clocked) {
  const uint64_t need = (clocked + opt.threads - 1) / opt.threads;
  return opt.coroutines > need ? opt.coroutines : need;
}

std::string Mb(uint64_t bytes) {
  return std::to_string(bytes / 1000000) + "." +
         std::to_string(bytes % 1000000 / 100000) + " MB";
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseArgs(argc, argv, &opt)) {
    Usage();
    return 2;
  }

  // 每个核一个，外围模块与停机判据各一个。
  const uint64_t clocked = opt.topo.CoreCount() + 2;
  opt.coroutines = CoroutinesFor(opt, clocked);
  // 协程号是十六位的，全部线程的协程加起来放不下就不是慢，是直接建不出来。
  if (opt.threads * opt.coroutines >= 65535) {
    std::cerr << "阵列太大：" << clocked << " 个挂时钟的模块要 " << opt.threads
              << " x " << opt.coroutines << " 个协程，超过了协程号的上限 65535。\n";
    return 2;
  }

  KernelImage img = LoadKernels(opt.in);
  std::filesystem::create_directories(opt.outdir);

  // 这一轮实际用的机器参数记进产物的来源里，两份产物才比得出是同一套配置跑的。
  img.meta["kernel_file"] = opt.in;
  img.meta["chips"] = std::to_string(opt.topo.chip_rows) + "x" +
                      std::to_string(opt.topo.chip_cols);
  img.meta["cores_per_chip"] = std::to_string(opt.topo.core_rows) + "x" +
                               std::to_string(opt.topo.core_cols);
  img.meta["bandwidth"] = std::to_string(opt.rp.bandwidth);
  img.meta["pcie_bandwidth"] = std::to_string(opt.rp.pcie_bandwidth);
  img.meta["pcie_latency"] = std::to_string(opt.rp.pcie_latency);
  img.meta["hop_latency"] = std::to_string(opt.rp.hop_latency);
  img.meta["dte_setup"] = std::to_string(opt.cp.dte_setup);
  img.meta["credit"] = std::to_string(opt.credit);

  RT::Reset(opt.threads, opt.coroutines);
  // 打点前先把落点定下来。第一段一旦落盘，文件就打开了，那时再改改不动。
  SetTraceDisabled(!opt.waveform);
  if (opt.waveform) RT::GetRecorder().SetPathPrefix(opt.outdir + "/waveform");

  std::cout << "kernel     " << img.cores.size() << " 个核，" << img.OpCount()
            << " 条\n";
  std::cout << "时序表     " << img.feeds.size() << " 条 FEED，"
            << img.drains.size() << " 条 DRAIN\n";
  std::cout << "阵列       " << opt.topo.chip_rows << "x" << opt.topo.chip_cols
            << " chip，每 chip " << opt.topo.core_rows << "x"
            << opt.topo.core_cols << " 核，共 " << opt.topo.CoreCount() << "\n";
  std::cout << "链路       核间 " << opt.rp.bandwidth << " 字节每拍，PCIe "
            << opt.rp.pcie_bandwidth << " 字节每拍，延迟 "
            << opt.rp.pcie_latency << "\n";
  std::cout << "额度       每个核 " << opt.credit << " 份\n";
  std::cout << "时钟粒度   " << opt.granularity << " 拍\n";
  std::cout << "并行       " << opt.threads << " 个线程，每线程 " << opt.coroutines
            << " 个协程，挂时钟的模块 " << clocked << " 个\n\n";

  RunRecorder run;
  RunStat stat;
  // 挂时钟的那些模块都关在这个作用域里：每个模块析构时才把它最后攒着的那一段打点交出
  // 去，而波形收尾一做完就不再接受新段落，两者顺序反了最后一段会撞在断言上。
  {
    ClockPtr clk = MakeClock(0, opt.granularity);
    Array arr(clk, img, opt.topo, opt.rp, opt.cp);
    Host host(clk, img, &arr, opt.topo, opt.rp.pcie_latency, opt.credit, "host",
              0);
    arr.ConnectHost(&host);
    Control ctl(clk, &arr, &host, opt.limit);

    clk->Continue();
    RT::JoinAll();

    for (uint64_t i = 0; i < arr.Size(); ++i) {
      Core* c = arr.At(i);
      run.Collect(c->Recorder());
      Exec& e = c->Unit();
      if (!e.HasProgram()) continue;
      ++stat.active_cores;
      stat.compute_cycles += e.ComputeCycles();
      stat.wait_cycles += e.WaitCyclesAt(ctl.EndTime());
      stat.sent_bytes += e.SentBytes();
      stat.recv_bytes += e.RecvBytes();
    }
    run.Collect(host.Recorder());
    run.Finalize(host.FedCount());

    stat.end_time = ctl.EndTime();
    stat.finished = ctl.Finished();
    stat.fed_bytes = host.FedBytes();
    stat.drained_bytes = host.DrainedBytes();
    stat.expect_bytes = host.ExpectBytes();
  }
  RT::FlushRecorder();

  Params params;
  WriteRunArtifacts(opt.outdir, run, img.meta, params);

  std::cout << "end_time   " << stat.end_time << " 拍\n";
  std::cout << "在跑的核   " << stat.active_cores << "\n";
  std::cout << "计算       " << stat.compute_cycles << " 拍（各核相加）\n";
  std::cout << "等待       " << stat.wait_cycles << " 拍（各核停在 Recv 上）\n";
  std::cout << "喂进去     " << Mb(stat.fed_bytes) << "\n";
  std::cout << "收回来     " << Mb(stat.drained_bytes) << " / "
            << Mb(stat.expect_bytes) << "\n";
  std::cout << "核间收发   收 " << Mb(stat.recv_bytes) << "，发 "
            << Mb(stat.sent_bytes) << "\n";
  std::cout << "观测       " << opt.outdir << "/{unit_spans,unit_waits,"
            << "global_latency}.jsonl 与 run_meta.json\n";
  if (opt.waveform) {
    std::cout << "波形       " << opt.outdir << "/waveform.trace\n";
  }

  if (!stat.finished) {
    std::cerr << "\n到上限还没跑完。\n";
    return 1;
  }
  return 0;
}
