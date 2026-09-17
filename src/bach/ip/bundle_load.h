#ifndef _LATCH_BACH_IP_BUNDLE_LOAD_
#define _LATCH_BACH_IP_BUNDLE_LOAD_

// 把编译器产的那份 bundle 装进模型。
//
// 一套 bundle 占 `bundle/<拓扑名>/` 一个目录，里面两样：一份 .bachir 与三份
// kernel 镜像。前者一行一条记录，装的是每颗 chip 的 core_bad_mask、算好的路由表、
// 任务链与各 core 的全局项；后者是三个 RV core 的镜像。每套自己带全，装载只看
// 自己这个目录。
//
// 真机上这是 SCP 的活：boot 期经 ctrl_noc 把每张表写进各 IP。这里走的是同一批
// 配置口，只是直接调方法而不过 ctrl_noc，写入顺序照《SCP 工作流程》那一节：
//   1. 全部 core 先写 core_bad_mask，再铺 RouterTable 与 Release 静态路由
//   2. 好 core 写 kernel、DTE 的表、进核配置、全局项与逐项任务链，最后写
//      TS_INIT_FINISH，自启动的 core 随即建满表项；坏 core 跳过这一步
//
// 应用之前先做装载检查，检查不过就断言，一项都不写。CORE 记录的角色只用在检查
// 里，不写进模型。
//
// 记录的字段顺序与 compiler/hwconfig/bachir.py 的 render_plan 一一对应，改一边
// 要一起改。

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "base/log.h"
#include "bach/ip/chip/chip.h"
#include "bach/ip/lpu_grid.h"

namespace latch {
namespace bach {

// 装完之后报一下装了多少，用例拿它核对 bundle 不是空的。
struct BundleStat {
  uint64_t chips = 0;
  uint64_t cores = 0;
  uint64_t entries = 0;
  uint64_t tasks = 0;
};

namespace bundle_detail {

using CoreKey = std::pair<uint64_t, uint64_t>;   // (chip 号, 片内 core 号)

// 一个 core 上要装的全部东西，读完再一起应用：TS 的合规性检查在写
// TS_INIT_FINISH 那一步做，任务链得先写全。
struct CorePlan {
  bool has_role = false;
  CoreRole role = CoreRole::kSpare;
  uint64_t stream_num = 1;
  bool self_start = false;
  uint64_t bcast_dirs = 0;
  bool trigger_chain_en = true;
  bool has_cfg = false;
  std::map<uint64_t, TaskEntry> chain;
  std::map<uint64_t, RouteEntry> rtab;
  std::vector<uint64_t> dte_rtab;
  std::map<uint64_t, uint64_t> path_task;          // DTE 那一份 path_task_map
  std::map<uint64_t, std::pair<uint64_t, uint64_t>> ts_route;  // PID → {方向, VCID}
  std::map<uint64_t, uint64_t> release_route;   // 入口方向 → Release 路由的出方向掩码
  uint64_t datain_pc = 0;
  bool datain_weights = false;
  bool has_datain = false;
  InboundCfg dtein;                              // 业务模式下进核那一笔
  bool has_dtein = false;
};

inline uint64_t Num(std::string const& s) {
  return uint64_t(std::stoull(s, nullptr, 0));
}

// 检查不过时报出是哪个 core、哪一条。
inline void Require(bool ok, CoreKey const& key, char const* what) {
  if (ok) return;
  spdlog::error("LoadBundle: chip {} core {}：{}", key.first, key.second, what);
  LOGCHECK(false, "LoadBundle: 装载检查没过。");
}

// core_bad_mask 至多 2 位为 1，每行至多 1 位。
inline void CheckBadMask(uint64_t chip, uint64_t mask) {
  uint64_t row_bad[2] = {0, 0};
  for (uint64_t c = 0; c < kChipCoreNum; ++c) {
    if ((mask >> c) & 1u) ++row_bad[c / kChipCols];
  }
  if (mask < (1ull << kChipCoreNum) && row_bad[0] <= 1 && row_bad[1] <= 1) {
    return;
  }
  spdlog::error("LoadBundle: chip {} 的 core_bad_mask 是 0x{:03x}", chip, mask);
  LOGCHECK(false, "LoadBundle: 一颗 chip 至多 2 个坏 core，一行至多 1 个。");
}

// 坏 core：不派角色，只有 RTAB 与 RELROUTE 两种记录，RTAB 表项一律不进 core、
// op_type 为 transfer、不查 stream 表、不转存。
inline void CheckBadCore(CoreKey const& key, CorePlan const& c) {
  Require(c.role == CoreRole::kSpare, key, "坏 core 的角色要是不派角色");
  Require(!c.has_cfg && c.chain.empty() && !c.has_datain && !c.has_dtein &&
              c.ts_route.empty() && c.dte_rtab.empty() && c.path_task.empty(),
          key, "坏 core 上只能有 RTAB 与 RELROUTE");
  for (auto const& one : c.rtab) {
    RouteEntry const& e = one.second;
    Require(e.path_core_bypass && e.op_type == OpType::kTransfer &&
                !e.stream_table_enable && !e.stall_way,
            key, "坏 core 的 RTAB 表项只能转发：不进 core、transfer、不查 stream "
                 "表、留在 VC 等");
  }
}

// 好 core：角色与体现角色的那几项配置对得上。
inline void CheckRole(CoreKey const& key, CorePlan const& c) {
  bool b = c.role == CoreRole::kBroadcast;
  bool r = c.role == CoreRole::kReduce;
  Require(c.bcast_dirs == 0 || b, key, "只有 B core 的 B_CORE_DIRECTION 非 0");
  switch (c.role) {
    case CoreRole::kCompute:
      Require(!c.self_start, key, "计算 core 的 SELF_START 要是 0");
      Require(c.has_dtein && c.dtein.route == Route::kRouterToCm, key,
              "计算 core 的 DTEIN 要落 Core Mem");
      break;
    case CoreRole::kBroadcast:
    case CoreRole::kReduce:
      Require(c.has_cfg && c.self_start, key,
              b ? "B core 的 SELF_START 要是 1" : "R core 的 SELF_START 要是 1");
      Require(c.has_dtein && c.dtein.route == Route::kRouterToMm, key,
              r ? "R core 的 DTEIN 要落 Matrix Mem"
                : "B core 的 DTEIN 要落 Matrix Mem");
      break;
    case CoreRole::kSpare:
      Require(!c.has_cfg && c.chain.empty() && !c.has_datain && !c.has_dtein,
              key, "不派角色的 core 不能有 CFGMISC、TCHAIN、DATAIN、DTEIN");
      break;
  }
}

}  // namespace bundle_detail

// 装一套 bundle。root 是 bundle 根目录，name 是拓扑名：这一套在 root/name/ 下，
// 配置是 name.bachir，kernel 镜像按记录里的文件名从同一个目录取。
inline BundleStat LoadBundle(std::vector<Chip*> const& chips,
                             std::string const& root, std::string const& name) {
  using bundle_detail::CoreKey;
  using bundle_detail::CorePlan;
  using bundle_detail::Num;
  using bundle_detail::Require;

  std::string dir = root + "/" + name;
  std::string path = dir + "/" + name + ".bachir";
  std::ifstream fp(path);
  if (!fp.good()) {
    spdlog::error("LoadBundle: 打不开 {}", path);
    LOGCHECK(false, "LoadBundle: bundle 里没有这份配置。");
  }

  std::map<CoreKey, CorePlan> plan;
  std::map<uint64_t, uint64_t> bad_mask;   // chip 号 → core_bad_mask
  std::vector<std::string> images;   // 三个 RV core 各一份，按 RV core 号排
  images.resize(3);
  BundleStat stat;

  std::string line;
  bool head_ok = false;
  while (std::getline(fp, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    std::vector<std::string> tok;
    std::string one;
    while (ss >> one) tok.push_back(one);
    if (tok.empty()) continue;
    std::string const& tag = tok[0];

    if (tag == "BACHIR") {
      LOGCHECK(tok.size() == 2 && tok[1] == "9",
               "LoadBundle: 只认 BACHIR 9 那一版产物。");
      head_ok = true;
      continue;
    }
    if (tag == "SOURCE" || tag == "NAME") continue;
    if (tag == "CHIP") {
      LOGCHECK(tok.size() == 3, "LoadBundle: CHIP 记录要两个字段。");
      bad_mask[Num(tok[1])] = Num(tok[2]);
      ++stat.chips;
      continue;
    }
    if (tag == "KERNEL") {
      LOGCHECK(tok.size() == 3, "LoadBundle: KERNEL 记录要两个字段。");
      uint64_t rv = Num(tok[1]);
      LOGCHECK(rv < images.size(), "LoadBundle: KERNEL 的 RV core 号越界。");
      images[rv] = tok[2];
      continue;
    }

    if (tok.size() < 3) {
      spdlog::error("LoadBundle: 记录 {} 少了 core 定位", tag);
      LOGCHECK(false, "LoadBundle: 记录少了 core 定位。");
    }
    CoreKey key(Num(tok[1]), Num(tok[2]));
    LOGCHECK(key.second < kChipCoreNum, "LoadBundle: core 号越界。");
    CorePlan& c = plan[key];

    if (tag == "CORE") {
      LOGCHECK(tok.size() == 4, "LoadBundle: CORE 记录要三个字段。");
      uint64_t role = Num(tok[3]);
      LOGCHECK(role <= uint64_t(CoreRole::kSpare),
               "LoadBundle: CORE 的角色只有 0～3。");
      c.role = CoreRole(role);
      c.has_role = true;
      ++stat.cores;
    } else if (tag == "CFGMISC") {
      LOGCHECK(tok.size() == 7, "LoadBundle: CFGMISC 记录要六个字段。");
      c.stream_num = Num(tok[3]);
      c.self_start = Num(tok[4]) != 0;
      c.bcast_dirs = Num(tok[5]);
      c.trigger_chain_en = Num(tok[6]) != 0;
      c.has_cfg = true;
    } else if (tag == "TCHAIN") {
      LOGCHECK(tok.size() == 13, "LoadBundle: TCHAIN 记录要十二个字段。");
      TaskEntry e;
      e.task_pc = Num(tok[4]);
      e.send_unit = SendUnit(Num(tok[5]));
      e.recv_unit = RecvUnit(Num(tok[6]));
      e.wait_wake = Num(tok[7]) != 0;
      e.task_type = TaskType(Num(tok[8]));
      e.p2p_reissue_tid = Num(tok[9]);
      e.credit_en = Num(tok[10]) != 0;
      e.path_id = Num(tok[11]);
      e.end = Num(tok[12]) != 0;
      c.chain[Num(tok[3])] = e;
      ++stat.tasks;
    } else if (tag == "DATAIN") {
      LOGCHECK(tok.size() == 5, "LoadBundle: DATAIN 记录要四个字段。");
      c.datain_pc = Num(tok[3]);
      c.datain_weights = Num(tok[4]) != 0;
      c.has_datain = true;
    } else if (tag == "DTEIN") {
      LOGCHECK(tok.size() == 7, "LoadBundle: DTEIN 记录要六个字段。");
      uint64_t route = Num(tok[3]);
      LOGCHECK(route <= 1,
               "LoadBundle: DTEIN 的 route 只有 0 落 Core Mem、1 落 Matrix Mem。");
      c.dtein.route = route == 0 ? Route::kRouterToCm : Route::kRouterToMm;
      c.dtein.no_ack = Num(tok[4]) != 0;
      c.dtein.flag_base = Num(tok[5]);
      c.dtein.flag_entry_bytes = Num(tok[6]);
      c.has_dtein = true;
    } else if (tag == "TSRTAB") {
      LOGCHECK(tok.size() == 6, "LoadBundle: TSRTAB 记录要五个字段。");
      c.ts_route[Num(tok[3])] = {Num(tok[4]), Num(tok[5])};
    } else if (tag == "PATHTASK") {
      LOGCHECK(tok.size() == 5, "LoadBundle: PATHTASK 记录要四个字段。");
      c.path_task[Num(tok[3])] = Num(tok[4]);
    } else if (tag == "RELROUTE") {
      LOGCHECK(tok.size() == 5, "LoadBundle: RELROUTE 记录要四个字段。");
      c.release_route[Num(tok[3])] = Num(tok[4]);
    } else if (tag == "RTABDTE") {
      LOGCHECK(tok.size() == 4, "LoadBundle: RTABDTE 记录要三个字段。");
      c.dte_rtab.push_back(Num(tok[3]));
    } else if (tag == "RTAB") {
      LOGCHECK(tok.size() == 32, "LoadBundle: RTAB 记录要三十一个字段。");
      RouteEntry e;
      e.valid = true;
      e.op_type = OpType(Num(tok[4]));
      e.flow_dir = Num(tok[5]);
      e.cur_vc = Num(tok[6]);
      for (uint64_t i = 0; i < e.nxt_vc.size(); ++i) {
        e.nxt_vc[i] = Num(tok[7 + i]);
      }
      e.path_core_mask_enable = Num(tok[12]) != 0;
      e.path_core_mask_idx = Num(tok[13]);
      e.path_core_bypass = Num(tok[14]) != 0;
      e.need_buffer = Num(tok[15]) != 0;
      e.stream_table_enable = Num(tok[16]) != 0;
      e.cur_credit_type = Num(tok[17]);
      e.cur_credit_require = Num(tok[18]);
      for (uint64_t i = 0; i < e.nxt_credit_type.size(); ++i) {
        e.nxt_credit_type[i] = Num(tok[19 + i]);
        e.nxt_credit_require[i] = Num(tok[22 + i]);
      }
      e.reduce_data_type = Num(tok[25]);
      e.reduce_outdata_type = Num(tok[26]);
      e.reduce_in_mask = Num(tok[27]);
      e.operation = Operation(Num(tok[28]));
      e.stall_way = Num(tok[29]) != 0;
      e.ext_dst = Num(tok[30]);
      e.reduce_need = Num(tok[31]) != 0;
      c.rtab[Num(tok[3])] = e;
      ++stat.entries;
    } else {
      spdlog::error("LoadBundle: 不认识的记录 {}", tag);
      LOGCHECK(false, "LoadBundle: 不认识的记录。");
    }
  }
  if (!head_ok) {
    spdlog::error("LoadBundle: {} 没有版本头", path);
    LOGCHECK(false, "LoadBundle: 产物没有版本头。");
  }

  // ── 装载检查 ──
  for (auto const& it : bad_mask) {
    LOGCHECK(it.first < chips.size(), "LoadBundle: chip 号越界。");
    bundle_detail::CheckBadMask(it.first, it.second);
    for (uint64_t core = 0; core < kChipCoreNum; ++core) {
      auto found = plan.find(CoreKey(it.first, core));
      Require(found != plan.end() && found->second.has_role,
              CoreKey(it.first, core), "每颗 chip 的 10 个 core 都要有 CORE 记录");
    }
  }
  for (auto const& it : plan) {
    auto chip_it = bad_mask.find(it.first.first);
    Require(chip_it != bad_mask.end(), it.first, "所在的 chip 没有 CHIP 记录");
    bool bad = ((chip_it->second >> it.first.second) & 1u) != 0;
    if (bad) {
      bundle_detail::CheckBadCore(it.first, it.second);
    } else {
      bundle_detail::CheckRole(it.first, it.second);
    }
  }

  // ── 装：先 core_bad_mask，再 Router 那两样，好 core 最后写 TS_INIT_FINISH ──
  for (auto const& it : bad_mask) {
    chips[it.first]->SetCoreBadMask(it.second);
  }
  for (auto const& it : plan) {
    CorePlan const& c = it.second;
    Core& core = chips[it.first.first]->GetCore(it.first.second);

    for (auto const& one : c.rtab) {
      core.GetRouter().Preload(one.first, one.second);
    }
    for (auto const& one : c.release_route) {
      core.GetRouter().SetCreditBypass(one.first, one.second);
    }
    if (core.Bad()) continue;

    for (uint64_t i = 0; i < images.size(); ++i) {
      if (!images[i].empty()) core.Rv(i).LoadImage(dir + "/" + images[i]);
    }
    for (uint64_t path_id : c.dte_rtab) {
      auto found = c.rtab.find(path_id);
      LOGCHECK(found != c.rtab.end(),
               "LoadBundle: RTABDTE 指的 path 在 RTAB 里没有。");
      core.GetDte().Tables().PreloadRtab(path_id, found->second);
    }
    for (auto const& one : c.path_task) {
      core.GetDte().Tables().PreloadPathTask(one.first, one.second);
    }
    if (c.has_dtein) core.SetBusinessInboundCfg(c.dtein);
    for (auto const& one : c.ts_route) {
      core.GetTs().Cfg().WriteRouterTable(one.first, one.second.first,
                                          one.second.second);
    }
    if (c.has_cfg) {
      core.GetTs().Cfg().SetStreamNum(c.stream_num);
      core.GetTs().Cfg().SetSelfStart(c.self_start);
      core.GetTs().Cfg().SetBCoreDirection(c.bcast_dirs);
      core.GetTs().Cfg().SetTriggerChainEn(c.trigger_chain_en);
    }
    for (auto const& one : c.chain) {
      core.GetTs().Cfg().WriteTask(one.first, one.second);
    }
    if (c.has_datain) {
      core.GetTs().Cfg().WriteDatainTask(c.datain_pc, c.datain_weights);
    }
    // 写 TS_INIT_FINISH：查整张配置表，自启动的 core 随即建满表项。
    core.GetTs().InitFinish();
  }
  return stat;
}

}  // namespace bach
}  // namespace latch

#endif
