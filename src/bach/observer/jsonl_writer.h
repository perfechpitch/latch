#ifndef _LATCH_BACH_OBSERVER_JSONL_WRITER_
#define _LATCH_BACH_OBSERVER_JSONL_WRITER_

// 观测产物落盘。一个目录三个桶加一份来源信息：
//
//   unit_spans.jsonl      单元占用区间
//   unit_waits.jsonl      等待区间与归因
//   global_latency.jsonl  每个 uid 的端到端延迟
//   run_meta.json         中间文件的来源、参数快照、结束时刻、完成与丢失的 uid
//
// 每个桶的首行是 schema 声明，其后每行一条记录，字段名齐全，直接能用 jq 读。
// 不做列存：这份产物是给人和脚本对账用的，可读比省几个字节重要。
//
// 没有 run_meta.json 的观测产物没法对账，所以它跟三个桶一起写：一次运行的结论
// 必须带着它的适用范围一起交付，光有事件不知道是哪份输入、哪套参数跑出来的。
//
// 字段值只有整数与固定的枚举名，枚举名都是 ASCII 标识符，所以这里不需要通用的
// JSON 转义。唯一可能带特殊字符的是来源信息里的字符串，那一处单独转义。

#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/common/params.h"
#include "bach/observer/span_recorder.h"

namespace latch {
namespace bach {

namespace detail {

inline std::string JsonEscape(std::string const& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
  }
  return out;
}

inline std::ofstream OpenOrDie(std::string const& path) {
  std::ofstream out(path, std::ios::trunc);
  if (!out.is_open()) {
    spdlog::error("观测产物写不出去：{}", path);
    LOGCHECK(false, "cannot open observer output file.");
  }
  return out;
}

inline void WriteUidList(std::ofstream& out, const char* name,
                         std::vector<uint64_t> const& uids) {
  out << "  \"" << name << "\": [";
  for (size_t i = 0; i < uids.size(); ++i) {
    if (i) out << ", ";
    out << uids[i];
  }
  out << "]";
}

}  // namespace detail

inline void WriteUnitSpans(std::string const& path,
                           std::vector<UnitSpan> const& spans) {
  std::ofstream out = detail::OpenOrDie(path);
  out << R"({"schema": "unit_spans.v1", "columns": ["node_id", "unit", "uid", )"
      << R"("tid", "start", "end", "state", "volume"]})" << "\n";
  for (UnitSpan const& s : spans) {
    out << "{\"node_id\": " << s.node_id
        << ", \"unit\": \"" << UnitName(s.unit) << "\""
        << ", \"uid\": " << s.uid
        << ", \"tid\": " << s.tid
        << ", \"start\": " << s.start
        << ", \"end\": " << s.end
        << ", \"state\": \"" << SpanStateName(s.state) << "\""
        << ", \"volume\": " << s.volume << "}\n";
  }
}

inline void WriteUnitWaits(std::string const& path,
                           std::vector<UnitWait> const& waits) {
  std::ofstream out = detail::OpenOrDie(path);
  out << R"({"schema": "unit_waits.v1", "columns": ["node_id", "unit", "uid", )"
      << R"("tid", "start", "end", "reason", "kind"]})" << "\n";
  for (UnitWait const& w : waits) {
    out << "{\"node_id\": " << w.node_id
        << ", \"unit\": \"" << UnitName(w.unit) << "\""
        << ", \"uid\": " << w.uid
        << ", \"tid\": " << w.tid
        << ", \"start\": " << w.start
        << ", \"end\": " << w.end
        << ", \"reason\": \"" << WaitReasonName(w.reason) << "\""
        << ", \"kind\": \""
        << (IsDependencyWait(w.reason) ? "dependency" : "resource") << "\"}\n";
  }
}

inline void WriteGlobalLatency(std::string const& path,
                               std::vector<GlobalLatency> const& latency) {
  std::ofstream out = detail::OpenOrDie(path);
  out << R"({"schema": "global_latency.v1", "columns": ["uid", "start", "end", )"
      << R"("latency", "expected_fragments"]})" << "\n";
  for (GlobalLatency const& g : latency) {
    out << "{\"uid\": " << g.uid
        << ", \"start\": " << g.start
        << ", \"end\": " << g.end
        << ", \"latency\": " << (g.end - g.start)
        << ", \"expected_fragments\": " << g.expected_fragments << "}\n";
  }
}

// source 是中间文件里那些 META 键值，原样带过来，这样一份产物能追回是哪张 Map、
// 哪一版编译链做的。
inline void WriteRunMeta(std::string const& path,
                         std::map<std::string, std::string> const& source,
                         Params const& params, RunResult const& result,
                         uint64_t span_count, uint64_t wait_count,
                         uint64_t dropped) {
  std::ofstream out = detail::OpenOrDie(path);
  out << "{\n";

  out << "  \"source\": {";
  bool first = true;
  for (auto const& kv : source) {
    if (!first) out << ",";
    first = false;
    out << "\n    \"" << detail::JsonEscape(kv.first) << "\": \""
        << detail::JsonEscape(kv.second) << "\"";
  }
  out << (first ? "}" : "\n  }") << ",\n";

  out << "  \"params\": {\n"
      << "    \"ts_logic_time\": " << params.ts_logic_time << ",\n"
      << "    \"dte_setup_time\": " << params.dte_setup_time << ",\n"
      << "    \"mu_setup_time\": " << params.mu_setup_time << ",\n"
      << "    \"vu_setup_time\": " << params.vu_setup_time << ",\n"
      << "    \"setup_ahead_depth\": " << params.setup_ahead_depth << ",\n"
      << "    \"dte_reduce_time\": " << params.dte_reduce_time << ",\n"
      << "    \"noc_bandwidth\": " << params.noc_bandwidth << ",\n"
      << "    \"pcie_bandwidth\": " << params.pcie_bandwidth << ",\n"
      << "    \"dte_bandwidth\": " << params.dte_bandwidth << ",\n"
      << "    \"cross_chip_delay\": " << params.cross_chip_delay << ",\n"
      << "    \"cross_node_delay\": " << params.cross_node_delay << ",\n"
      << "    \"host_push_delay\": " << params.host_push_delay << ",\n"
      << "    \"stream_count\": " << params.stream_count << ",\n"
      << "    \"num_users\": " << params.num_users << ",\n"
      << "    \"matrix_fifo_credit\": " << params.matrix_fifo_credit << ",\n"
      << "    \"dte_execution_mode\": \""
      << (params.dte_execution_mode == DteExecutionMode::kSplit ? "split" : "shared")
      << "\",\n"
      << "    \"dte_dsa_mode\": \""
      << (params.dte_dsa_mode == DteDsaMode::kOff ? "off" : "five_route")
      << "\"\n  },\n";

  out << "  \"result\": {\n"
      << "    \"end_time_ns\": " << result.end_time << ",\n"
      << "    \"expected_users\": " << result.expected_users << ",\n"
      << "    \"completed_users\": " << result.completed_uids.size() << ",\n"
      << "    \"succeeded\": " << (result.Succeeded() ? "true" : "false") << ",\n";
  detail::WriteUidList(out, "completed_uids", result.completed_uids);
  out << ",\n";
  detail::WriteUidList(out, "lost_uids", result.lost_uids);
  out << "\n  },\n";

  out << "  \"observer\": {\n"
      << "    \"unit_spans\": " << span_count << ",\n"
      << "    \"unit_waits\": " << wait_count << ",\n"
      << "    \"dropped\": " << dropped << "\n  }\n";

  out << "}\n";
}

// 一次性写全四份。dir 要已经存在。
inline void WriteRunArtifacts(std::string const& dir, RunRecorder const& rec,
                              std::map<std::string, std::string> const& source,
                              Params const& params) {
  WriteUnitSpans(dir + "/unit_spans.jsonl", rec.Spans());
  WriteUnitWaits(dir + "/unit_waits.jsonl", rec.Waits());
  WriteGlobalLatency(dir + "/global_latency.jsonl", rec.Latency());
  WriteRunMeta(dir + "/run_meta.json", source, params, rec.Result(),
               rec.Spans().size(), rec.Waits().size(), rec.Dropped());
}

}
}

#endif
