#ifndef _LATCH_BACH_IP_BUNDLE_LOAD_
#define _LATCH_BACH_IP_BUNDLE_LOAD_

// 把编译器产的那份 bundle 装进模型。
//
// 一套 bundle 占 `bundle/<拓扑名>/` 一个目录，里面两样：一份 .bachir 与三份
// kernel 镜像。前者一行一条记录，装的是算好的路由表、任务链与 TS 的全局项；后者
// 是三个 RV core 的镜像。每套自己带全，装载只看自己这个目录。
//
// 真机上这是 SCP 的活：boot 期经 ctrl_noc 把每张表写进各 IP。这里走的是同一批
// 配置口，只是直接调方法而不过 ctrl_noc，写入顺序照《SCP 工作流程》那一节：全局
// 项与逐项任务链先写，最后写 TS_INIT_FINISH，自启动的 core 随即建满表项。
//
// 记录的字段顺序与 compiler/hwconfig/bachir.py 的 render_plan 一一对应，改一边
// 要一起改。

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/ip/chip/chip.h"

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

// 一个 core 上要装的全部东西，读完再一起应用：TS 的合规性检查在写
// TS_INIT_FINISH 那一步做，任务链得先写全。
struct CorePlan {
  bool present = false;
  uint64_t role = 0;              // 0 计算 1 广播 2 归约 3 不派角色
  uint64_t stream_num = 1;
  uint64_t bcast_dirs = 0;
  bool trigger_chain_en = true;
  bool has_cfg = false;
  std::map<uint64_t, TaskEntry> chain;
  std::map<uint64_t, RouteEntry> rtab;
  std::vector<uint64_t> dte_rtab;
  std::map<uint64_t, uint64_t> path_task;
  uint64_t datain_pc = 0;
  bool datain_weights = false;
  bool has_datain = false;
};

inline uint64_t Num(std::string const& s) {
  return uint64_t(std::stoull(s, nullptr, 0));
}

}  // namespace bundle_detail

// 装一套 bundle。root 是 bundle 根目录，name 是拓扑名：这一套在 root/name/ 下，
// 配置是 name.bachir，kernel 镜像按记录里的文件名从同一个目录取。
inline BundleStat LoadBundle(std::vector<Chip*> const& chips,
                             std::string const& root, std::string const& name) {
  using bundle_detail::CorePlan;
  using bundle_detail::Num;

  std::string dir = root + "/" + name;
  std::string path = dir + "/" + name + ".bachir";
  std::ifstream fp(path);
  if (!fp.good()) {
    spdlog::error("LoadBundle: 打不开 {}", path);
    LOGCHECK(false, "LoadBundle: bundle 里没有这份配置。");
  }

  std::map<std::pair<uint64_t, uint64_t>, CorePlan> plan;
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
      LOGCHECK(tok.size() == 2 && tok[1] == "6",
               "LoadBundle: 只认 BACHIR 6 那一版产物。");
      head_ok = true;
      continue;
    }
    if (tag == "SOURCE" || tag == "NAME") continue;
    if (tag == "CHIP") {
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
    auto key = std::make_pair(Num(tok[1]), Num(tok[2]));
    CorePlan& c = plan[key];
    c.present = true;

    if (tag == "CORE") {
      LOGCHECK(tok.size() == 4, "LoadBundle: CORE 记录要三个字段。");
      c.role = Num(tok[3]);
      ++stat.cores;
    } else if (tag == "CFGMISC") {
      LOGCHECK(tok.size() == 7, "LoadBundle: CFGMISC 记录要六个字段。");
      c.stream_num = Num(tok[3]);
      c.role = Num(tok[4]);
      c.bcast_dirs = Num(tok[5]);
      c.trigger_chain_en = Num(tok[6]) != 0;
      c.has_cfg = true;
    } else if (tag == "TCHAIN") {
      LOGCHECK(tok.size() == 19, "LoadBundle: TCHAIN 记录要十八个字段。");
      TaskEntry e;
      e.task_pc = Num(tok[4]);
      e.send_unit = SendUnit(Num(tok[5]));
      e.recv_unit = RecvUnit(Num(tok[6]));
      e.self_start = Num(tok[7]) != 0;
      e.wait_wake = Num(tok[8]) != 0;
      e.broadcast_reissue = Num(tok[9]) != 0;
      e.p2p_reissue = Num(tok[10]) != 0;
      e.reduce = Num(tok[11]) != 0;
      e.credit_en = Num(tok[12]) != 0;
      e.exe_mask = Num(tok[13]) != 0;
      e.path_id = Num(tok[14]);
      e.end = Num(tok[15]) != 0;
      e.dsa_en = Num(tok[16]) != 0;
      e.reduce_num = Num(tok[17]);
      e.exe_dest = Num(tok[18]);
      c.chain[Num(tok[3])] = e;
      ++stat.tasks;
    } else if (tag == "DATAIN") {
      LOGCHECK(tok.size() == 5, "LoadBundle: DATAIN 记录要四个字段。");
      c.datain_pc = Num(tok[3]);
      c.datain_weights = Num(tok[4]) != 0;
      c.has_datain = true;
    } else if (tag == "PATHTASK") {
      LOGCHECK(tok.size() == 5, "LoadBundle: PATHTASK 记录要四个字段。");
      c.path_task[Num(tok[3])] = Num(tok[4]);
    } else if (tag == "RTABDTE") {
      LOGCHECK(tok.size() == 4, "LoadBundle: RTABDTE 记录要三个字段。");
      c.dte_rtab.push_back(Num(tok[3]));
    } else if (tag == "RTAB") {
      LOGCHECK(tok.size() == 31, "LoadBundle: RTAB 记录要三十个字段。");
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

  // 装：kernel 与路由表先，任务链与全局项后，最后写 TS_INIT_FINISH。
  for (auto const& it : plan) {
    uint64_t chip_id = it.first.first;
    uint64_t core_id = it.first.second;
    LOGCHECK(chip_id < chips.size(), "LoadBundle: chip 号越界。");
    Core& core = chips[chip_id]->GetCore(core_id);

    for (auto const& one : it.second.rtab) {
      core.GetRouter().Preload(one.first, one.second);
    }
    if (core.Context().router_only) continue;

    for (uint64_t i = 0; i < images.size(); ++i) {
      if (!images[i].empty()) core.Rv(i).LoadImage(dir + "/" + images[i]);
    }
    for (uint64_t path_id : it.second.dte_rtab) {
      auto found = it.second.rtab.find(path_id);
      LOGCHECK(found != it.second.rtab.end(),
               "LoadBundle: RTABDTE 指的 path 在 RTAB 里没有。");
      core.GetDte().Tables().PreloadRtab(path_id, found->second);
    }
    for (auto const& one : it.second.path_task) {
      core.GetTs().Cfg().WritePathMap(one.first, one.second);
      core.GetDte().Tables().PreloadPathTask(one.first, one.second);
    }
    for (auto const& one : it.second.chain) {
      core.GetTs().Cfg().WriteTask(one.first, one.second);
    }
    if (it.second.has_cfg) {
      core.GetTs().Cfg().SetStreamNum(it.second.stream_num);
      core.GetTs().Cfg().SetCoreType(CoreType(it.second.role));
      core.GetTs().Cfg().SetBCoreDirection(it.second.bcast_dirs);
      core.GetTs().Cfg().SetTriggerChainEn(it.second.trigger_chain_en);
    }
    if (it.second.has_datain) {
      core.GetTs().Cfg().WriteDatainTask(it.second.datain_pc,
                                         it.second.datain_weights);
    }
    core.GetTs().Cfg().SetInitFinish();
    // B core 与 R core 复位后直接建满表项，不等 Router trigger。
    if (core.GetTs().Cfg().SelfStartCore()) core.GetTs().SelfStart();
  }
  return stat;
}

}  // namespace bach
}  // namespace latch

#endif
