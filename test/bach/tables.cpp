// 中间文件解析的行为基线。
//
// 两组用例：一组读 test/bach/fixture/ 下那份真实导出产物，锁住字段含义；一组用最小
// 文件逐条试各种坏输入，锁住"遇到不认识的就停"。坏输入走 TryLoadBachIr，它把原因
// 写进字符串返回 false，不用起 death test。

#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "bach/tables/loader.h"

using namespace latch;
using namespace latch::bach;

namespace {

std::string FixturePath() {
  return std::string(PROJECT_TEST_DIR) + "/bach/fixture/bach_topology.bachir";
}

// 另一份真实产出，它有前一份没有的三样：显式网关、BROADCAST 核、两个 user。
std::string BroadcastFixturePath() {
  return std::string(PROJECT_TEST_DIR) + "/bach/fixture/moe_bc_core.bachir";
}

// 中间文件格式文档里那个最小完整例子，逐字搬过来。它同时是那份文档的活检查：
// 例子改坏了，这里就红。
const char* const kMinimal = R"(BACHIR 5

META map_path /home/me/maps/minimal.map
META map_sha256 59e128d6cb3dcfb5067774f0c1dd5e7ad0f5eb82912fc0ecaf203229964825a8
META compiler_sha256 6ec90383e5db3e3163ea6e2a448e3a2097b6673c5a9bcb7201e825e8f5763f0b
META generated_at 2026-08-17T11:09:07
META generator src/bach/tools/export_bachir.py

PARAM stream_count 1
PARAM num_users 1
MODE dte_execution_mode split
MODE dte_dsa_mode off

DIM 1 1 1 2 1 1

CORE 0 -1 NORMAL
CORE 1 -1 NORMAL

# Core 0：等 Host 的包，验资，发给 Core 1，算一次 Matrix，退休回 Host
TASK 0 0 SKIP USER_INIT 0 0 0 0 0 1024
TASK 0 1 CU DONTCARE 0 0 0 0 0 0
CREDIT 0 1 1
TASK 0 2 DTE USER_INIT 0 0 1 0 1 1024
TASK 0 3 MC DONTCARE 0 0 0 0 0 96
TASK 0 4 DTE RETIRE 0 -1 0 0 -1 0

# Core 1：等 Core 0 的包，搬给 Out，退休回 Core 0
TASK 1 0 SKIP USER_INIT 0 0 1 0 1 1024
TASK 1 1 DTE MOVE 0 0 out 0 2 1024
TASK 1 2 DTE RETIRE 0 0 0 0 0 0

EXTNODE host HOST 0 -1 0 1024 PCIE_WEST 0 0
EXTNODE out OUT 0 2 1 1024 PCIE_EAST 0 0

TOTALUSERS 1
TOTALPACKETS 1
USER 0 HOST host
USERTARGET 0 host 0 0
HITMAP 0
OUTFRAG 0 1
)";

std::string WriteTemp(const std::string& name, const std::string& body) {
  std::string path = std::string(PROJECT_TEST_BUILD_DIR) + "/" + name;
  std::ofstream out(path, std::ios::trunc);
  out << body;
  out.close();
  return path;
}

// 在最小文件末尾追加几行，返回解析失败时的原因；解析成功则返回空串。
std::string ErrorAfterAppending(const std::string& name, const std::string& extra) {
  BachIr ir;
  std::string err;
  const std::string path = WriteTemp(name, std::string(kMinimal) + extra);
  if (TryLoadBachIr(path, &ir, &err)) return std::string();
  return err;
}

std::string ErrorOf(const std::string& name, const std::string& body) {
  BachIr ir;
  std::string err;
  const std::string path = WriteTemp(name, body);
  if (TryLoadBachIr(path, &ir, &err)) return std::string();
  return err;
}

}  // namespace

// ---------------------------------------------------------------- 尺寸换算

TEST(BachLoader, DimMapsCoordsBothWays) {
  Dim d;
  d.chip_rows = 1;
  d.chip_cols = 4;
  d.core_rows_per_chip = 2;
  d.core_cols_per_chip = 4;
  d.node_chip_rows = 1;
  d.node_chip_cols = 1;

  EXPECT_EQ(d.GlobalRows(), 2u);
  EXPECT_EQ(d.GlobalCols(), 16u);
  EXPECT_EQ(d.CoreNum(), 32u);

  EXPECT_EQ(d.CoordOf(0), (Coord{0, 0}));
  EXPECT_EQ(d.CoordOf(16), (Coord{1, 0}));
  EXPECT_EQ(d.CoordOf(31), (Coord{1, 15}));
  EXPECT_EQ(d.CoreIdAt(Coord{1, 0}), 16);

  // 外部节点的坐标不在阵列内
  EXPECT_FALSE(d.Inside(Coord{0, -1}));
  EXPECT_FALSE(d.Inside(Coord{1, 16}));
  EXPECT_EQ(d.CoreIdAt(Coord{0, -1}), -1);

  // 每个 chip 自成一个 node，所以任意两个 chip 之间都算跨 node
  EXPECT_EQ(d.ChipOf(3), (Coord{0, 0}));
  EXPECT_EQ(d.ChipOf(4), (Coord{0, 1}));
  EXPECT_NE(d.NodeOfChip(d.ChipOf(3)), d.NodeOfChip(d.ChipOf(4)));
}

// ---------------------------------------------------------------- 真实产出

TEST(BachLoader, ReadsRealExporterOutput) {
  BachIr ir = LoadBachIr(FixturePath());

  EXPECT_EQ(ir.dim.chip_rows, 1u);
  EXPECT_EQ(ir.dim.chip_cols, 4u);
  EXPECT_EQ(ir.dim.CoreNum(), 32u);
  EXPECT_EQ(ir.cores.size(), 32u);
  EXPECT_EQ(ir.tasks.size(), 32u);
  EXPECT_EQ(ir.total_users, 1u);

  // 参数：导出时传的覆盖值与 Bach 的默认值都应该到位
  EXPECT_EQ(ir.params.stream_count, 1u);
  EXPECT_EQ(ir.params.num_users, 1u);
  EXPECT_EQ(ir.params.dte_setup_time, 85u);
  EXPECT_EQ(ir.params.dte_reduce_time, 32u);
  EXPECT_EQ(ir.params.cross_node_delay, 5000u);
  EXPECT_EQ(ir.params.noc_router_delay, 1u);  // Bach 没配，导出时落到 1
  EXPECT_EQ(ir.params.dte_execution_mode, DteExecutionMode::kSplit);
  EXPECT_EQ(ir.params.dte_dsa_mode, DteDsaMode::kOff);

  EXPECT_FALSE(ir.meta["map_sha256"].empty());
  EXPECT_FALSE(ir.meta["compiler_sha256"].empty());
  EXPECT_EQ(ir.meta["generator"], "src/bach/tools/export_bachir.py");

  // core 0：等包、验资、发给 core 1、算三段、reduce 给 core 1、退休
  const CoreTaskTable* t0 = ir.Tasks(0);
  ASSERT_NE(t0, nullptr);
  ASSERT_EQ(t0->Size(), 8u);
  EXPECT_EQ(t0->At(0).unit, UnitType::kSkip);
  EXPECT_EQ(t0->At(0).opcode, Opcode::kUserInit);
  EXPECT_EQ(t0->At(1).unit, UnitType::kCu);
  EXPECT_EQ(t0->At(2).unit, UnitType::kDte);
  EXPECT_EQ(t0->At(2).down_cid, 1);
  EXPECT_EQ(t0->At(2).dst, (Coord{0, 1}));
  EXPECT_EQ(t0->At(2).time_or_vol, 8192u);
  EXPECT_EQ(t0->At(3).unit, UnitType::kMc);
  EXPECT_EQ(t0->At(3).time_or_vol, 1835u);   // MC 行装的是计算时间
  EXPECT_EQ(t0->At(6).opcode, Opcode::kReduce);
  EXPECT_EQ(t0->At(6).tag, 6u);              // 接收核上对应任务的 task id
  EXPECT_EQ(t0->At(7).opcode, Opcode::kRetire);

  // CU 行才有 credit，SKIP 行没有
  const std::vector<int64_t>* cu = ir.credits.Targets(0, 1);
  ASSERT_NE(cu, nullptr);
  ASSERT_EQ(cu->size(), 1u);
  EXPECT_EQ((*cu)[0], 1);
  EXPECT_EQ(ir.credits.Targets(0, 0), nullptr);

  // 屏障 SKIP 行才有来源
  const SkipSource* src = ir.skip_sources.Get(1, 6);
  ASSERT_NE(src, nullptr);
  EXPECT_EQ(src->sender, 0);
  EXPECT_EQ(src->phase_type, "REDUCE");
  EXPECT_EQ(src->phase_idx, 2u);
  EXPECT_TRUE(src->sender_group_ids.empty());
  EXPECT_EQ(ir.skip_sources.Get(1, 0), nullptr);

  // 目标是外部节点的那一行：down_cid 不可用，看 down_ext
  const CoreTaskTable* t15 = ir.Tasks(15);
  ASSERT_NE(t15, nullptr);
  const TaskEntry& to_out = t15->At(6);
  EXPECT_EQ(to_out.opcode, Opcode::kMove);
  ASSERT_TRUE(to_out.TargetsExtNode());
  EXPECT_EQ(ir.ext_nodes[to_out.down_ext].name, "out");
  EXPECT_FALSE(ir.dim.Inside(to_out.dst));
  ASSERT_NE(ir.ExtAt(to_out.dst), nullptr);
  EXPECT_EQ(ir.ExtAt(to_out.dst)->kind, ExtKind::kOut);

  // core 16 由 Host 直接驱动，退休回 Host：up_cid 是 -1，坐标是 Host 的坐标
  const TaskEntry& retire16 = ir.Tasks(16)->At(8);
  EXPECT_EQ(retire16.opcode, Opcode::kRetire);
  EXPECT_EQ(retire16.up_cid, -1);
  ASSERT_NE(ir.ExtAt(retire16.dst), nullptr);
  EXPECT_EQ(ir.ExtAt(retire16.dst)->kind, ExtKind::kHost);

  // 外部节点
  ASSERT_EQ(ir.ext_nodes.size(), 2u);
  const ExtNode& host = ir.ext_nodes[ir.ext_by_name.at("host")];
  EXPECT_EQ(host.kind, ExtKind::kHost);
  EXPECT_EQ(host.coord, (Coord{0, -1}));
  EXPECT_EQ(host.target_core, 16u);
  EXPECT_EQ(host.port, Port::kPcieWest);

  // 片间链路：1x4 chip，三对，六条有向记录
  EXPECT_EQ(ir.chip_links.size(), 6u);
  EXPECT_EQ(ir.chip_links[0].src_core, 3u);
  EXPECT_EQ(ir.chip_links[0].src_port, Port::kPcieEast);
  EXPECT_EQ(ir.chip_links[0].dst_core, 4u);
  EXPECT_EQ(ir.chip_links[0].delay, 5000u);   // 每个 chip 自成一 node

  // user
  const UserInfo* u = ir.User(0);
  ASSERT_NE(u, nullptr);
  EXPECT_EQ(u->src_kind, SrcKind::kHost);
  EXPECT_EQ(ir.ext_nodes[u->src_ext].name, "host");
  ASSERT_EQ(u->targets.size(), 1u);
  EXPECT_EQ(u->targets[0].target_core, 16u);
  EXPECT_EQ(u->expected_fragments, 1u);
  EXPECT_TRUE(u->hit_map.empty());   // dense，不激活任何 EPGroup
}

TEST(BachLoader, ReadsGatewaysAndBroadcastCores) {
  BachIr ir = LoadBachIr(BroadcastFixturePath());

  EXPECT_EQ(ir.cores.size(), 64u);
  EXPECT_EQ(ir.dim.CoreNum(), 64u);
  EXPECT_EQ(ir.total_users, 2u);
  EXPECT_EQ(ir.users.size(), 2u);
  EXPECT_EQ(ir.params.stream_count, 2u);

  // 这张 map 的 chip_rules 是显式写的，所以每个 chip 四个方向都有网关
  ASSERT_FALSE(ir.gateways.empty());
  uint32_t left_of_chip0 = 0;
  bool found = false;
  for (const Gateway& g : ir.gateways) {
    if (g.chip_id == 0 && g.dir == Direction::kLeft) {
      ASSERT_EQ(g.local_cores.size(), 1u);
      left_of_chip0 = g.local_cores[0];
      found = true;
    }
  }
  ASSERT_TRUE(found);
  EXPECT_EQ(left_of_chip0, 4u);

  // credit 额度按下游核的类型定，所以核类型必须原样读进来
  uint32_t broadcast = 0;
  for (const auto& kv : ir.cores) {
    if (kv.second.type == CoreType::kBroadcast) ++broadcast;
  }
  EXPECT_EQ(broadcast, 2u);

  // 两个 user 各有自己的注入目标
  ASSERT_NE(ir.User(0), nullptr);
  ASSERT_NE(ir.User(1), nullptr);
  EXPECT_EQ(ir.User(0)->targets.size(), 1u);
  EXPECT_EQ(ir.User(1)->targets.size(), 1u);
}

// ---------------------------------------------------------------- 最小例子

TEST(BachLoader, ReadsTheMinimalExampleFromTheSpec) {
  BachIr ir;
  std::string err;
  ASSERT_TRUE(TryLoadBachIr(WriteTemp("minimal.bachir", kMinimal), &ir, &err)) << err;

  EXPECT_EQ(ir.dim.CoreNum(), 2u);
  EXPECT_EQ(ir.cores.size(), 2u);
  EXPECT_EQ(ir.Tasks(0)->Size(), 5u);
  EXPECT_EQ(ir.Tasks(1)->Size(), 3u);
  EXPECT_EQ(ir.params.stream_count, 1u);
  EXPECT_EQ(ir.meta["map_path"], "/home/me/maps/minimal.map");
  EXPECT_TRUE(ir.chip_links.empty());   // 单 chip 没有片间链路

  const TaskEntry& to_out = ir.Tasks(1)->At(1);
  ASSERT_TRUE(to_out.TargetsExtNode());
  EXPECT_EQ(ir.ext_nodes[to_out.down_ext].name, "out");
  EXPECT_EQ(ir.Tasks(0)->At(4).up_cid, -1);
}

// ---------------------------------------------------------------- 坏输入

TEST(BachLoader, StopsOnAnythingItDoesNotRecognize) {
  EXPECT_NE(ErrorAfterAppending("bad_tag.bachir", "WAT 1 2\n").find("不认识的记录标签"),
            std::string::npos);
  EXPECT_NE(ErrorAfterAppending("bad_param.bachir", "PARAM nope 3\n").find("不认识的参数名"),
            std::string::npos);
  EXPECT_NE(ErrorAfterAppending("bad_mode.bachir", "MODE dte_dsa_mode maybe\n").find("dte_dsa_mode"),
            std::string::npos);
  EXPECT_NE(ErrorAfterAppending("bad_tmeta.bachir", "TMETA 0 0 nope 1\n").find("不认识的任务元数据键"),
            std::string::npos);
  EXPECT_NE(ErrorAfterAppending("bad_unit.bachir", "TASK 5 0 WAT MOVE 0 0 0 0 0 0\n").find("不认识的 unit"),
            std::string::npos);
  EXPECT_NE(ErrorAfterAppending("bad_port.bachir", "EXTNODE x OUT 9 9 0 1 SIDEWAYS 0 0\n").find("不认识的端口名"),
            std::string::npos);
}

TEST(BachLoader, RejectsWrongFormatVersion) {
  std::string body = kMinimal;
  body.replace(body.find("BACHIR 5"), 8, "BACHIR 1");
  EXPECT_NE(ErrorOf("bad_version.bachir", body).find("格式版本"), std::string::npos);
}

TEST(BachLoader, RequiresContiguousTaskIds) {
  // 新核的第一行 task id 必须是 0
  const std::string err =
      ErrorAfterAppending("gap.bachir", "CORE 9 -1 NORMAL\nTASK 9 3 SKIP MOVE 0 0 0 0 0 0\n");
  EXPECT_NE(err.find("连续递增"), std::string::npos);
}

TEST(BachLoader, ChecksWhichRowARecordMayHangOn) {
  // task 0 是 SKIP 行，挂不了 CREDIT
  EXPECT_NE(ErrorAfterAppending("credit_on_skip.bachir", "CREDIT 0 0 1\n").find("CU 行"),
            std::string::npos);
  // task 1 是 CU 行，挂不了 SKIPSRC
  EXPECT_NE(ErrorAfterAppending("skip_on_cu.bachir", "SKIPSRC 0 1 0 REDUCE 2\n").find("SKIP 行"),
            std::string::npos);
  // 指向不存在的任务行
  EXPECT_NE(ErrorAfterAppending("dangling_task.bachir", "TMETA 0 99 recv_init 1\n").find("不存在"),
            std::string::npos);
}

TEST(BachLoader, ChecksCrossRecordReferences) {
  // 任务目标写了一个没有 EXTNODE 的名字
  EXPECT_NE(ErrorAfterAppending("dangling_ext.bachir",
                                "CORE 9 -1 NORMAL\nTASK 9 0 DTE MOVE 0 0 nowhere 0 9 8\n")
                .find("没有对应的 EXTNODE"),
            std::string::npos);
  // 落在核阵列外的目标坐标没有对应节点
  EXPECT_NE(ErrorAfterAppending("dangling_coord.bachir",
                                "CORE 9 -1 NORMAL\nTASK 9 0 DTE MOVE 0 0 1 7 7 8\n")
                .find("落在核阵列外"),
            std::string::npos);
  // USERTARGET 指向的不是 Host
  EXPECT_NE(ErrorAfterAppending("target_not_host.bachir", "USERTARGET 0 out 0 0\n")
                .find("不是 HOST"),
            std::string::npos);
  // 有 uid 的记录但没有 USER
  EXPECT_NE(ErrorAfterAppending("dangling_uid.bachir", "OUTFRAG 7 1\n")
                .find("没有 USER"),
            std::string::npos);
}

TEST(BachLoader, RequiresChipLinksToBePaired) {
  const std::string err =
      ErrorAfterAppending("half_link.bachir", "CHIPLINK 0 PCIE_EAST 1 PCIE_WEST 0 400\n");
  EXPECT_NE(err.find("缺反方向"), std::string::npos);
}

TEST(BachLoader, RequiresTheMandatoryRecords) {
  std::string body = kMinimal;
  const size_t dim = body.find("DIM 1 1 1 2 1 1");
  body.erase(dim, std::string("DIM 1 1 1 2 1 1").size());
  EXPECT_NE(ErrorOf("no_dim.bachir", body).find("缺 DIM"), std::string::npos);

  std::string dup = kMinimal;
  dup += "CORE 0 -1 NORMAL\n";
  EXPECT_NE(ErrorOf("dup_core.bachir", dup).find("两条 CORE"), std::string::npos);

  std::string two_dim = kMinimal;
  two_dim += "DIM 1 1 1 2 1 1\n";
  EXPECT_NE(ErrorOf("two_dim.bachir", two_dim).find("只能有一条"), std::string::npos);
}

TEST(BachLoader, RejectsMalformedNumbersAndFieldCounts) {
  EXPECT_NE(ErrorAfterAppending("nan.bachir", "OUTFRAG 0 xyz\n").find("整数"),
            std::string::npos);
  EXPECT_NE(ErrorAfterAppending("short.bachir", "DIM 1 1\n").find("字段"),
            std::string::npos);
}

TEST(BachLoader, ReportsThePathAndLineOfTheOffendingRecord) {
  const std::string err = ErrorAfterAppending("located.bachir", "WAT\n");
  EXPECT_NE(err.find("located.bachir:"), std::string::npos);
}

// ---------------------------------------------------------------- Phase

// Phase1 那一段的行与它引用的外部节点必须对得上，交换节点的四个开关之间有前置条件。
TEST(BachLoader, ChecksThePhaseRecordsAgainstEachOther) {
  const std::string lane =
      "EXTNODE L0 PHASE1_LANE 0 -2 0 1024 PCIE_WEST 0 0\n"
      "EXTNODE S0 PHASE1_SINK 0 -3 0 1024 PCIE_WEST 0 0\n"
      "EXTNODE S1 PHASE1_SINK 0 -4 0 1024 PCIE_WEST 0 0\n"
      "PHASE1SINK S0 BYPASS\nPHASE1SINK S1 MOE\n";

  // 落点要交给交换节点，就得先有交换节点
  EXPECT_NE(ErrorAfterAppending("no_eth.bachir", lane).find("ETHSW"),
            std::string::npos);

  const std::string eth =
      "ETHSW 200 128 6144 2 4 8\n"
      "ETHPORT res_ingress 60 0 64 64\n"
      "ETHPORT phase1_moe_ingress 30 0 64 64\n"
      "ETHPORT phase2_moe_result_ingress 60 0 64 64\n"
      "ETHPORT phase2_moe_request_egress 30 0 64 64\n"
      "ETHPORT phase3_res_join_egress 60 0 64 64\n"
      "ETHROUTE phase1_moe_ingress phase2_moe_request_egress\n"
      "ETHROUTE res_ingress phase3_res_join_egress\n"
      "ETHROUTE phase2_moe_result_ingress phase3_res_join_egress\n";

  // 两路落在同一处，下一跳就分不开了
  EXPECT_NE(ErrorAfterAppending(
                "same_sink.bachir",
                lane + eth + "PHASE1LANE L0 0 0 S0 S0 1024 1024 0 0 0 0 10\n")
                .find("同一处"),
            std::string::npos);

  // 要发 MoE 请求就得写明选中了哪几个专家
  EXPECT_NE(ErrorAfterAppending(
                "no_hit.bachir",
                lane + eth + "PHASE1LANE L0 0 0 S0 S1 1024 1024 0 0 0 0 10\n" +
                    "PHASE1USER L0 0\n")
                .find("PHASE1HIT"),
            std::string::npos);

  // 一条通道一个 user 也不产就没有意义
  EXPECT_NE(ErrorAfterAppending(
                "no_user.bachir",
                lane + eth + "PHASE1LANE L0 0 0 S0 S1 1024 1024 0 0 0 0 10\n" +
                    "PHASE1HIT L0 0 1\n")
                .find("PHASE1USER"),
            std::string::npos);

  const std::string whole = lane + eth +
                            "PHASE1LANE L0 0 0 S0 S1 1024 1024 0 0 0 0 10\n"
                            "PHASE1HIT L0 0 1\nPHASE1USER L0 0\n";

  // 注入要知道哪个 group 归谁
  EXPECT_NE(ErrorAfterAppending("no_bind.bachir", whole + "ETHMODE 1 1 1 0\n")
                .find("HOSTBIND"),
            std::string::npos);

  const std::string bound =
      whole + "HOSTBIND host 0\nHOSTGROUP host 0 0\n";

  // 完成权只能有一处：结果要交回交换节点，Phase1 边界那个判法就得关掉
  EXPECT_NE(ErrorAfterAppending("two_authorities.bachir",
                                bound + "ETHMODE 1 1 1 1\n")
                .find("完成权"),
            std::string::npos);

  // 结果要交回来，就得先有人把请求注入 Phase2
  EXPECT_NE(ErrorAfterAppending("no_ingress.bachir",
                                bound + "ETHMODE 0 0 0 1\n")
                .find("注入"),
            std::string::npos);
}

TEST(BachLoader, ChecksTheEthSwitchPortsAndRoutes) {
  EXPECT_NE(ErrorAfterAppending("bad_eth_port.bachir",
                                "ETHSW 200 128 6144 2 4 8\n"
                                "ETHPORT nowhere 60 0 64 64\n")
                .find("不认识的交换节点端口"),
            std::string::npos);

  // 五个口少一个都不行
  EXPECT_NE(ErrorAfterAppending("few_eth_ports.bachir",
                                "ETHSW 200 128 6144 2 4 8\n"
                                "ETHPORT res_ingress 60 0 64 64\n")
                .find("五个端口"),
            std::string::npos);

  // 没有 ETHSW 就不该有它的端口
  EXPECT_NE(ErrorAfterAppending("orphan_eth_port.bachir",
                                "ETHPORT res_ingress 60 0 64 64\n")
                .find("没有 ETHSW"),
            std::string::npos);
}
