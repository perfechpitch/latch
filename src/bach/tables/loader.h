#ifndef _LATCH_BACH_TABLES_LOADER_
#define _LATCH_BACH_TABLES_LOADER_

// 中间文件的读入。格式定义见 doc/bach/intermediate_format.md。
//
// 解析分两遍：第一遍逐行读进各张表，第二遍做引用完整性检查。记录之间顺序无关，
// 所以像 TASK 行里指向某个外部节点的标识符这种前向引用，要等第二遍才能落成下标。
//
// 遇到不认识的记录标签、不认识的 PARAM 名字、不认识的 TMETA 键、不认识的枚举取值，
// 一律当错误停下。只有 META 的 key 例外，它是纯来源信息，不影响模型。静默跳过一条
// 不认识的记录，等于两边跑的不是同一份配置，那正是这份文件要杜绝的事。
//
// 两个入口：TryLoadBachIr 把错误写进字符串返回 false，供测试覆盖各条拒绝路径；
// LoadBachIr 直接在错误上停机，供装配路径用。

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/log.h"
#include "bach/common/packet.h"
#include "bach/common/params.h"
#include "bach/tables/credit_table.h"
#include "bach/tables/task_meta.h"
#include "bach/tables/task_table.h"

namespace latch {
namespace bach {

// ---------------------------------------------------------------- 枚举

enum class ExtKind : uint32_t {
  kHost = 0,
  kOut = 1,
  kPhase1Lane = 2,
  kPhase1Sink = 3,
};

enum class Direction : uint32_t {
  kLeft = 0,
  kRight = 1,
  kTop = 2,
  kBottom = 3,
};

enum class SrcKind : uint32_t {
  kHost = 0,
  kDispatcher = 1,
  kPhase1Lane = 2,
};

enum class PcieEndKind : uint32_t {
  kSwitch = 0,
  kCore = 1,
  kHost = 2,
  kOut = 3,
};

// ---------------------------------------------------------------- 拓扑尺寸

// 六个尺寸字段，以及由它们唯一确定的坐标换算。核阵列内的一维 core id 与二维全局
// 坐标一一对应，外部节点用不落在阵列内的坐标表示。
struct Dim {
  uint32_t chip_rows = 0;
  uint32_t chip_cols = 0;
  uint32_t core_rows_per_chip = 0;
  uint32_t core_cols_per_chip = 0;
  uint32_t node_chip_rows = 0;
  uint32_t node_chip_cols = 0;

  uint32_t GlobalRows() const { return chip_rows * core_rows_per_chip; }
  uint32_t GlobalCols() const { return chip_cols * core_cols_per_chip; }
  uint64_t CoreNum() const {
    return static_cast<uint64_t>(GlobalRows()) * GlobalCols();
  }

  bool Inside(Coord c) const {
    return c.row >= 0 && c.col >= 0 &&
           static_cast<uint32_t>(c.row) < GlobalRows() &&
           static_cast<uint32_t>(c.col) < GlobalCols();
  }

  Coord CoordOf(uint64_t core_id) const {
    Coord c;
    c.row = static_cast<int32_t>(core_id / GlobalCols());
    c.col = static_cast<int32_t>(core_id % GlobalCols());
    return c;
  }

  // 不在核阵列内返回 -1。
  int64_t CoreIdAt(Coord c) const {
    if (!Inside(c)) return -1;
    return static_cast<int64_t>(c.row) * GlobalCols() + c.col;
  }

  Coord ChipOfCoord(Coord g) const {
    Coord chip;
    chip.row = g.row / static_cast<int32_t>(core_rows_per_chip);
    chip.col = g.col / static_cast<int32_t>(core_cols_per_chip);
    return chip;
  }

  Coord ChipOf(uint64_t core_id) const { return ChipOfCoord(CoordOf(core_id)); }

  // 片内一维 core id 换成全局坐标。片内按行优先编号，与网关规则里的写法一致。
  Coord GlobalOf(uint32_t chip_id, uint32_t local_core) const {
    Coord c;
    const uint32_t chip_row = chip_id / chip_cols;
    const uint32_t chip_col = chip_id % chip_cols;
    c.row = static_cast<int32_t>(chip_row * core_rows_per_chip +
                                 local_core / core_cols_per_chip);
    c.col = static_cast<int32_t>(chip_col * core_cols_per_chip +
                                 local_core % core_cols_per_chip);
    return c;
  }

  Coord NodeOfChip(Coord chip) const {
    Coord node;
    node.row = chip.row / static_cast<int32_t>(node_chip_rows);
    node.col = chip.col / static_cast<int32_t>(node_chip_cols);
    return node;
  }

  uint32_t ChipIdOf(Coord chip) const {
    return static_cast<uint32_t>(chip.row) * chip_cols +
           static_cast<uint32_t>(chip.col);
  }
};

// ---------------------------------------------------------------- 记录

struct CoreInfo {
  int32_t group_id = -1;
  CoreType type = CoreType::kNormal;
};

struct ExtNode {
  std::string name;
  ExtKind kind = ExtKind::kHost;
  Coord coord;
  uint64_t target_core = 0;
  uint64_t volume = 0;
  Port port = Port::kUnknown;
  uint64_t pcie_bandwidth = 0;  // 0 表示用默认
  uint64_t pcie_delay = 0;      // 0 表示用默认
};

struct Gateway {
  uint32_t chip_id = 0;
  Direction dir = Direction::kLeft;
  std::vector<uint32_t> local_cores;
};

struct DirAttr {
  uint32_t chip_id = 0;
  Direction dir = Direction::kLeft;
  uint64_t bandwidth = 0;
  uint64_t delay = 0;
};

// 软件 credit 图的一条边：src 向 dst 申请与归还额度，额度上限看 dst 的类型。
//
// 它跟物理连线、跟任务表都是两回事：有物理线不代表要额度，有验资任务也只说明"这一条
// 任务要验哪几个下游"，不说明开了哪些户。所以这张图必须单独给，不能从别处推。
struct CreditEdge {
  uint64_t src_core = 0;
  uint64_t dst_core = 0;
  CoreType dst_type = CoreType::kNormal;
};

// 一条片间链路的一个方向。带宽与延迟是发送侧那个端口上的取值。
struct ChipLink {
  uint64_t src_core = 0;
  Port src_port = Port::kUnknown;
  uint64_t dst_core = 0;
  Port dst_port = Port::kUnknown;
  uint64_t bandwidth = 0;  // 0 表示用默认
  uint64_t delay = 0;
};

struct PcieSwitchInfo {
  std::string id;
  int32_t chip = -1;
  Coord coord;
};

// 一条 PCIe 链路的一端。交换节点那一端的端口名是 Map 自己起的标识符，不是十二个
// 方向之一：交换节点的端口有几个、叫什么，由拓扑决定，本来就没有方向可言。
struct PcieEnd {
  PcieEndKind kind = PcieEndKind::kCore;
  std::string ref;
  Port port = Port::kUnknown;  // 核与外部节点那一端用
  std::string port_name;       // 交换节点那一端用
};

struct PcieLink {
  std::string id;
  PcieEnd a;
  PcieEnd b;
  uint64_t bandwidth = 0;
  uint64_t delay = 0;
};

struct PcieRoute {
  std::string sw_id;
  Coord dst;
  std::string out_port;
};

struct PcieInRoute {
  std::string sw_id;
  std::string in_port;
  Coord dst;
  std::string out_port;
};

struct HostGroupTarget {
  std::string host_name;
  uint32_t group_id = 0;
  uint64_t target_core = 0;
};

// Phase1 一条通道的行为参数。它自己的坐标、网关、端口与带宽走 EXTNODE，这里只补
// 它的分段延迟、两个落点与本次选中的那几个专家。
struct Phase1LaneInfo {
  std::string name;
  uint64_t layer_id = 0;
  int64_t group_id = -1;
  std::string bypass_sink;  // 空表示这一路不发
  std::string moe_sink;
  uint64_t bypass_volume = 0;
  uint64_t moe_volume = 0;
  uint64_t fc0_delay = 0;
  uint64_t res_delay = 0;
  uint64_t norm_delay = 0;
  uint64_t router_delay = 0;
  uint64_t push_delay = 0;
  std::vector<uint32_t> hit_map;  // 专家 id，交换节点按它换算成 EPGroup
  std::vector<uint64_t> uids;
};

// 落点的角色，取 GENERIC、MOE、BYPASS 之一。它决定这个落点把收齐的包交到以太网
// 交换节点的哪个入口，GENERIC 不往下交。
struct Phase1SinkInfo {
  std::string name;
  std::string role;
};

struct EthPortInfo {
  std::string port_id;
  uint64_t bandwidth = 0;
  uint64_t propagation = 0;
  uint64_t queue_capacity = 0;
  uint64_t pending_capacity = 0;
};

struct EthRouteInfo {
  std::string in_port;
  std::string out_port;
};

// 一个专家落在哪几个 EPGroup 上。不给这张表时按组大小整除，那只是没有对应表时的
// 兜底：热点专家复制之后一个专家会落在多个 group 上，那种 Map 必须写出来。
struct ExpertGroupEntry {
  uint32_t expert_id = 0;
  std::vector<uint32_t> groups;
};

struct EthConfig {
  bool present = false;
  uint64_t processing_delay = 200;
  uint64_t header_bytes = 128;
  uint64_t payload_numel = 6144;
  uint64_t payload_bytes_per_elem = 2;
  uint32_t expert_group_size = 16;
  uint32_t expert_count = 0;  // 0 表示不限

  // 四个开关。完成权只能有一处，所以第四个开着时第一个必须关着。
  bool complete_phase1_boundary = true;
  bool phase2_ingress = false;
  bool inject_host = false;
  bool result_bridge = false;

  std::vector<EthPortInfo> ports;
  std::vector<EthRouteInfo> routes;
  std::vector<ExpertGroupEntry> expert_groups;
};

struct EpGroup {
  uint32_t group_id = 0;
  int64_t shared_ep_id = -1;
  uint32_t routed_ep_num = 0;
};

struct HostBind {
  std::string host_name;
  uint32_t group_id = 0;
};

struct UserTarget {
  uint32_t host_ext = kNoExtNode;
  uint64_t target_core = 0;
  uint64_t tag = 0;
};

struct UserInfo {
  uint64_t uid = 0;
  SrcKind src_kind = SrcKind::kHost;
  uint32_t src_ext = kNoExtNode;
  std::vector<UserTarget> targets;
  std::vector<uint32_t> hit_map;
  std::vector<std::pair<uint32_t, uint32_t>> moe_bitmap;
  uint64_t expected_fragments = 1;
};

// VectorCore 在 router_softmax_topk 完成那一刻要写出去的 HitMap，内容预先算好。
struct DynHitMap {
  uint64_t uid = 0;
  uint64_t layer_id = 0;
  uint64_t core_id = 0;
  std::vector<uint32_t> groups;
};

// ---------------------------------------------------------------- 汇总

struct BachIr {
  std::map<std::string, std::string> meta;
  Params params;
  Dim dim;

  std::unordered_map<uint64_t, CoreInfo> cores;
  std::unordered_map<uint64_t, CoreTaskTable> tasks;
  TaskMetaTable metas;
  SkipSourceTable skip_sources;
  CreditTable credits;

  std::vector<ExtNode> ext_nodes;
  std::unordered_map<uint64_t, uint32_t> ext_by_coord;  // EncodeCoord 到下标
  std::unordered_map<std::string, uint32_t> ext_by_name;

  std::vector<CreditEdge> credit_edges;
  std::vector<Gateway> gateways;
  std::vector<DirAttr> dir_attrs;
  std::vector<ChipLink> chip_links;

  std::vector<PcieSwitchInfo> pcie_switches;
  std::vector<PcieLink> pcie_links;
  std::vector<PcieRoute> pcie_routes;
  std::vector<PcieInRoute> pcie_in_routes;
  std::vector<HostGroupTarget> host_group_targets;

  std::vector<Phase1LaneInfo> phase1_lanes;
  std::vector<Phase1SinkInfo> phase1_sinks;
  EthConfig eth;

  std::vector<EpGroup> ep_groups;
  std::vector<HostBind> host_binds;

  uint64_t total_users = 0;
  // 一次 run 里各汇聚点一共会收到几包。它与完成判据是两回事：一个 user 的结果可以
  // 落在几个汇聚点上，收到第一个就算它完成了，但时钟要等余下那些也到了才停。
  uint64_t total_packets = 0;
  std::vector<UserInfo> users;
  std::unordered_map<uint64_t, uint32_t> user_by_uid;
  std::vector<DynHitMap> dyn_hit_maps;

  CoreTaskTable const* Tasks(uint64_t core_id) const {
    auto it = tasks.find(core_id);
    return it == tasks.end() ? nullptr : &it->second;
  }

  ExtNode const* ExtAt(Coord c) const {
    auto it = ext_by_coord.find(EncodeCoord(c));
    return it == ext_by_coord.end() ? nullptr : &ext_nodes[it->second];
  }

  UserInfo const* User(uint64_t uid) const {
    auto it = user_by_uid.find(uid);
    return it == user_by_uid.end() ? nullptr : &users[it->second];
  }
};

// ---------------------------------------------------------------- 解析

namespace detail {

inline bool ParseI64(std::string const& s, int64_t* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  long long v = std::strtoll(s.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return false;
  *out = static_cast<int64_t>(v);
  return true;
}

inline bool ParseU64(std::string const& s, uint64_t* out) {
  int64_t v = 0;
  if (!ParseI64(s, &v) || v < 0) return false;
  *out = static_cast<uint64_t>(v);
  return true;
}

inline bool LooksNumeric(std::string const& s) {
  if (s.empty()) return false;
  size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  if (i >= s.size()) return false;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

inline bool ParseUnitType(std::string const& s, UnitType* out) {
  if (s == "DTE") *out = UnitType::kDte;
  else if (s == "MC") *out = UnitType::kMc;
  else if (s == "VC") *out = UnitType::kVc;
  else if (s == "CU") *out = UnitType::kCu;
  else if (s == "SKIP") *out = UnitType::kSkip;
  else return false;
  return true;
}

inline bool ParseOpcode(std::string const& s, Opcode* out) {
  if (s == "USER_INIT") *out = Opcode::kUserInit;
  else if (s == "MOVE") *out = Opcode::kMove;
  else if (s == "REDUCE") *out = Opcode::kReduce;
  else if (s == "REDUCTION") *out = Opcode::kReduction;
  else if (s == "CONCAT") *out = Opcode::kConcat;
  else if (s == "RETIRE") *out = Opcode::kRetire;
  else if (s == "FIFO_IN") *out = Opcode::kFifoIn;
  else if (s == "FIFO_OUT") *out = Opcode::kFifoOut;
  else if (s == "RES") *out = Opcode::kRes;
  else if (s == "BYPASS") *out = Opcode::kBypass;
  else if (s == "DONTCARE") *out = Opcode::kDontCare;
  else return false;
  return true;
}

inline bool ParsePort(std::string const& s, Port* out) {
  if (s == "LOCAL") *out = Port::kLocal;
  else if (s == "NORTH") *out = Port::kNorth;
  else if (s == "EAST") *out = Port::kEast;
  else if (s == "SOUTH") *out = Port::kSouth;
  else if (s == "WEST") *out = Port::kWest;
  else if (s == "PCIE_UP") *out = Port::kPcieUp;
  else if (s == "PCIE_DOWN") *out = Port::kPcieDown;
  else if (s == "PCIE_NORTH") *out = Port::kPcieNorth;
  else if (s == "PCIE_SOUTH") *out = Port::kPcieSouth;
  else if (s == "PCIE_EAST") *out = Port::kPcieEast;
  else if (s == "PCIE_WEST") *out = Port::kPcieWest;
  else if (s == "UNKNOWN") *out = Port::kUnknown;
  else return false;
  return true;
}

inline bool ParseCoreType(std::string const& s, CoreType* out) {
  if (s == "NORMAL") *out = CoreType::kNormal;
  else if (s == "BROADCAST") *out = CoreType::kBroadcast;
  else if (s == "REDUCTION") *out = CoreType::kReduction;
  else return false;
  return true;
}

inline bool ParseExtKind(std::string const& s, ExtKind* out) {
  if (s == "HOST") *out = ExtKind::kHost;
  else if (s == "OUT") *out = ExtKind::kOut;
  else if (s == "PHASE1_LANE") *out = ExtKind::kPhase1Lane;
  else if (s == "PHASE1_SINK") *out = ExtKind::kPhase1Sink;
  else return false;
  return true;
}

inline bool ParseDirection(std::string const& s, Direction* out) {
  if (s == "LEFT") *out = Direction::kLeft;
  else if (s == "RIGHT") *out = Direction::kRight;
  else if (s == "TOP") *out = Direction::kTop;
  else if (s == "BOTTOM") *out = Direction::kBottom;
  else return false;
  return true;
}

inline bool ParseSrcKind(std::string const& s, SrcKind* out) {
  if (s == "HOST") *out = SrcKind::kHost;
  else if (s == "DISPATCHER") *out = SrcKind::kDispatcher;
  else if (s == "PHASE1_LANE") *out = SrcKind::kPhase1Lane;
  else return false;
  return true;
}

inline bool ParsePcieEndKind(std::string const& s, PcieEndKind* out) {
  if (s == "SW") *out = PcieEndKind::kSwitch;
  else if (s == "CORE") *out = PcieEndKind::kCore;
  else if (s == "HOST") *out = PcieEndKind::kHost;
  else if (s == "OUT") *out = PcieEndKind::kOut;
  else return false;
  return true;
}

inline bool ParseSemanticOp(std::string const& s, SemanticOp* out) {
  if (s == "moe_send") *out = SemanticOp::kMoeSend;
  else if (s == "router_softmax_topk") *out = SemanticOp::kRouterSoftmaxTopk;
  else if (s == "concat_router_logits") *out = SemanticOp::kConcatRouterLogits;
  else return false;
  return true;
}

inline bool ParseDsaRoute(std::string const& s, DsaRoute* out) {
  if (s == "R2M") *out = DsaRoute::kR2M;
  else if (s == "R2C") *out = DsaRoute::kR2C;
  else if (s == "M2R") *out = DsaRoute::kM2R;
  else if (s == "C2R") *out = DsaRoute::kC2R;
  else if (s == "M2C") *out = DsaRoute::kM2C;
  else return false;
  return true;
}

inline bool ParseEndpoint(std::string const& s, Endpoint* out) {
  if (s == "M") *out = Endpoint::kMatrix;
  else if (s == "C") *out = Endpoint::kCore;
  else return false;
  return true;
}

// PARAM 名字到字段的对应。两张表分开是因为字段宽度不同。
inline std::unordered_map<std::string, uint64_t Params::*> const& U64ParamMap() {
  static const std::unordered_map<std::string, uint64_t Params::*> kMap = {
      {"ts_logic_time", &Params::ts_logic_time},
      {"dte_setup_time", &Params::dte_setup_time},
      {"mu_setup_time", &Params::mu_setup_time},
      {"vu_setup_time", &Params::vu_setup_time},
      {"cm_arb_delay", &Params::cm_arb_delay},
      {"mm_arb_delay", &Params::mm_arb_delay},
      {"noc_router_delay", &Params::noc_router_delay},
      {"noc_wire_delay", &Params::noc_wire_delay},
      {"noc_access_delay", &Params::noc_access_delay},
      {"cross_chip_delay", &Params::cross_chip_delay},
      {"cross_node_delay", &Params::cross_node_delay},
      {"host_push_delay", &Params::host_push_delay},
      {"bitmap_access_time", &Params::bitmap_access_time},
      {"credit_check_time", &Params::credit_check_time},
      {"dte_reduce_time", &Params::dte_reduce_time},
      {"noc_bandwidth", &Params::noc_bandwidth},
      {"pcie_bandwidth", &Params::pcie_bandwidth},
      {"dte_bandwidth", &Params::dte_bandwidth},
      {"if_dte_cm", &Params::if_dte_cm},
      {"if_dte_mm", &Params::if_dte_mm},
      {"if_vu_cm", &Params::if_vu_cm},
      {"if_mu_cm", &Params::if_mu_cm},
      {"if_mu_mm", &Params::if_mu_mm},
      {"matrix_fifo_credit", &Params::matrix_fifo_credit},
      {"num_users", &Params::num_users},
      {"router_queue_warn", &Params::router_queue_warn},
      {"link_fifo_depth", &Params::link_fifo_depth},
      {"watchdog_lifespan", &Params::watchdog_lifespan},
      {"host_credit_timeout", &Params::host_credit_timeout},
      {"hop_count_warn", &Params::hop_count_warn},
      {"hop_count_err", &Params::hop_count_err},
  };
  return kMap;
}

inline std::unordered_map<std::string, uint32_t Params::*> const& U32ParamMap() {
  static const std::unordered_map<std::string, uint32_t Params::*> kMap = {
      {"setup_ahead_depth", &Params::setup_ahead_depth},
      {"stream_count", &Params::stream_count},
  };
  return kMap;
}

class Parser {
 public:
  Parser(std::string const& file_path, BachIr* result, std::string* error)
      : path(file_path), ir(result), err(error) {}

  bool Run() {
    std::ifstream in(path);
    if (!in.is_open()) {
      *err = path + ": 打不开文件";
      return false;
    }
    std::string line;
    while (std::getline(in, line)) {
      ++line_no;
      if (!Line(line)) return false;
    }
    if (!saw_version) return Fail("缺版本行，首行必须是 BACHIR <version>");
    return Validate();
  }

 private:
  // ---- 基础

  bool Fail(std::string const& msg) {
    *err = path + ":" + std::to_string(line_no) + ": " + msg;
    return false;
  }

  static void Tokenize(std::string const& line, std::vector<std::string>* out) {
    out->clear();
    size_t i = 0;
    while (i < line.size()) {
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t' ||
                                 line[i] == '\r')) ++i;
      if (i >= line.size()) break;
      size_t start = i;
      while (i < line.size() && line[i] != ' ' && line[i] != '\t' &&
             line[i] != '\r') ++i;
      out->emplace_back(line, start, i - start);
    }
  }

  bool Need(size_t n) {
    if (tok.size() < n) {
      return Fail(tok[0] + " 至少要 " + std::to_string(n - 1) + " 个字段，实际 " +
                  std::to_string(tok.size() - 1) + " 个");
    }
    return true;
  }

  bool NeedExact(size_t n) {
    if (tok.size() != n) {
      return Fail(tok[0] + " 要 " + std::to_string(n - 1) + " 个字段，实际 " +
                  std::to_string(tok.size() - 1) + " 个");
    }
    return true;
  }

  bool U64(size_t idx, uint64_t* out) {
    if (!ParseU64(tok[idx], out)) return Fail("第 " + std::to_string(idx) +
                                              " 个字段不是非负整数：" + tok[idx]);
    return true;
  }

  bool I64(size_t idx, int64_t* out) {
    if (!ParseI64(tok[idx], out)) return Fail("第 " + std::to_string(idx) +
                                              " 个字段不是整数：" + tok[idx]);
    return true;
  }

  bool U32(size_t idx, uint32_t* out) {
    uint64_t v = 0;
    if (!U64(idx, &v)) return false;
    *out = static_cast<uint32_t>(v);
    return true;
  }

  // ---- 逐行

  bool Line(std::string const& raw) {
    if (raw.empty() || raw[0] == '#') return true;
    Tokenize(raw, &tok);
    if (tok.empty()) return true;

    std::string const& tag = tok[0];
    if (!saw_version && tag != "BACHIR") {
      return Fail("首行必须是 BACHIR <version>，实际是 " + tag);
    }

    if (tag == "BACHIR") return Version();
    if (tag == "META") return Meta(raw);
    if (tag == "PARAM") return Param();
    if (tag == "MODE") return Mode();
    if (tag == "DIM") return DimRec();
    if (tag == "CORE") return CoreRec();
    if (tag == "TASK") return TaskRec();
    if (tag == "TMETA") return TmetaRec();
    if (tag == "CREDIT") return CreditRec();
    if (tag == "CREDITEDGE") return CreditEdgeRec();
    if (tag == "SKIPSRC") return SkipSrcRec();
    if (tag == "EXTNODE") return ExtNodeRec();
    if (tag == "GATEWAY") return GatewayRec();
    if (tag == "DIRATTR") return DirAttrRec();
    if (tag == "CHIPLINK") return ChipLinkRec();
    if (tag == "PCIESW") return PcieSwRec();
    if (tag == "PCIELINK") return PcieLinkRec();
    if (tag == "PCIEROUTE") return PcieRouteRec();
    if (tag == "PCIEIROUTE") return PcieInRouteRec();
    if (tag == "HOSTGROUP") return HostGroupRec();
    if (tag == "PHASE1LANE") return Phase1LaneRec();
    if (tag == "PHASE1HIT") return Phase1HitRec();
    if (tag == "PHASE1USER") return Phase1UserRec();
    if (tag == "PHASE1SINK") return Phase1SinkRec();
    if (tag == "ETHSW") return EthSwRec();
    if (tag == "ETHPORT") return EthPortRec();
    if (tag == "ETHROUTE") return EthRouteRec();
    if (tag == "ETHMODE") return EthModeRec();
    if (tag == "EXPGROUP") return ExpGroupRec();
    if (tag == "EPGROUP") return EpGroupRec();
    if (tag == "HOSTBIND") return HostBindRec();
    if (tag == "TOTALUSERS") return TotalUsersRec();
    if (tag == "TOTALPACKETS") return TotalPacketsRec();
    if (tag == "USER") return UserRec();
    if (tag == "USERTARGET") return UserTargetRec();
    if (tag == "HITMAP") return HitMapRec();
    if (tag == "BITMAP") return BitMapRec();
    if (tag == "OUTFRAG") return OutFragRec();
    if (tag == "DHITMAP") return DynHitMapRec();
    return Fail("不认识的记录标签 " + tag);
  }

  bool Version() {
    if (!NeedExact(2)) return false;
    uint64_t v = 0;
    if (!U64(1, &v)) return false;
    if (v != kFormatVersion) {
      return Fail("格式版本 " + tok[1] + " 不是本解析器认识的 " +
                  std::to_string(kFormatVersion));
    }
    saw_version = true;
    return true;
  }

  bool Meta(std::string const& raw) {
    if (!Need(3)) return false;
    // 值吃到行尾，所以要回到原始行上按位置取，不能用切好的 token 拼，也不能靠
    // 查找 key 的文本，那样 key 是标签的子串时会找错位置
    size_t pos = raw.find_first_not_of(" \t");          // 标签起点
    pos = raw.find_first_of(" \t", pos);                // 标签末尾
    pos = raw.find_first_not_of(" \t", pos);            // key 起点
    pos = raw.find_first_of(" \t", pos);                // key 末尾
    pos = raw.find_first_not_of(" \t", pos);            // 值起点
    std::string value = raw.substr(pos);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r')) value.pop_back();
    ir->meta[tok[1]] = value;
    return true;
  }

  bool Param() {
    if (!NeedExact(3)) return false;
    uint64_t value = 0;
    if (!U64(2, &value)) return false;
    auto const& u64map = U64ParamMap();
    auto it64 = u64map.find(tok[1]);
    if (it64 != u64map.end()) {
      ir->params.*(it64->second) = value;
      return true;
    }
    auto const& u32map = U32ParamMap();
    auto it32 = u32map.find(tok[1]);
    if (it32 != u32map.end()) {
      ir->params.*(it32->second) = static_cast<uint32_t>(value);
      return true;
    }
    return Fail("不认识的参数名 " + tok[1]);
  }

  bool Mode() {
    if (!NeedExact(3)) return false;
    if (tok[1] == "dte_execution_mode") {
      if (tok[2] == "split") ir->params.dte_execution_mode = DteExecutionMode::kSplit;
      else if (tok[2] == "shared") ir->params.dte_execution_mode = DteExecutionMode::kShared;
      else return Fail("dte_execution_mode 取值只能是 split 或 shared，实际 " + tok[2]);
      return true;
    }
    if (tok[1] == "dte_dsa_mode") {
      if (tok[2] == "off") ir->params.dte_dsa_mode = DteDsaMode::kOff;
      else if (tok[2] == "five_route") ir->params.dte_dsa_mode = DteDsaMode::kFiveRoute;
      else return Fail("dte_dsa_mode 取值只能是 off 或 five_route，实际 " + tok[2]);
      return true;
    }
    return Fail("不认识的模式名 " + tok[1]);
  }

  bool DimRec() {
    if (!NeedExact(7)) return false;
    if (saw_dim) return Fail("DIM 只能有一条");
    Dim d;
    if (!U32(1, &d.chip_rows) || !U32(2, &d.chip_cols) ||
        !U32(3, &d.core_rows_per_chip) || !U32(4, &d.core_cols_per_chip) ||
        !U32(5, &d.node_chip_rows) || !U32(6, &d.node_chip_cols)) return false;
    if (d.chip_rows == 0 || d.chip_cols == 0 || d.core_rows_per_chip == 0 ||
        d.core_cols_per_chip == 0 || d.node_chip_rows == 0 ||
        d.node_chip_cols == 0) {
      return Fail("DIM 六个值都必须是正整数");
    }
    if (d.chip_rows % d.node_chip_rows != 0) return Fail("node_chip_rows 必须整除 chip_rows");
    if (d.chip_cols % d.node_chip_cols != 0) return Fail("node_chip_cols 必须整除 chip_cols");
    ir->dim = d;
    saw_dim = true;
    return true;
  }

  bool CoreRec() {
    if (!NeedExact(4)) return false;
    uint64_t core_id = 0;
    int64_t group_id = 0;
    CoreInfo info;
    if (!U64(1, &core_id) || !I64(2, &group_id)) return false;
    if (!ParseCoreType(tok[3], &info.type)) return Fail("不认识的 core 类型 " + tok[3]);
    if (ir->cores.count(core_id)) return Fail("core " + tok[1] + " 出现两条 CORE 记录");
    info.group_id = static_cast<int32_t>(group_id);
    ir->cores[core_id] = info;
    return true;
  }

  bool TaskRec() {
    if (!NeedExact(11)) return false;
    uint64_t core_id = 0, task_id = 0;
    TaskEntry e;
    int64_t dst_row = 0, dst_col = 0;
    if (!U64(1, &core_id) || !U64(2, &task_id)) return false;
    if (!ParseUnitType(tok[3], &e.unit)) return Fail("不认识的 unit " + tok[3]);
    if (!ParseOpcode(tok[4], &e.opcode)) return Fail("不认识的 opcode " + tok[4]);
    if (!U64(5, &e.tag) || !I64(6, &e.up_cid)) return false;

    if (LooksNumeric(tok[7])) {
      if (!I64(7, &e.down_cid)) return false;
    } else {
      // 外部节点的名字，等第二遍才知道它对应哪个 EXTNODE
      e.down_cid = -1;
      pending_ext.push_back({core_id, task_id, tok[7], line_no});
    }
    if (!I64(8, &dst_row) || !I64(9, &dst_col) || !U64(10, &e.time_or_vol)) return false;
    e.dst.row = static_cast<int32_t>(dst_row);
    e.dst.col = static_cast<int32_t>(dst_col);

    CoreTaskTable& table = ir->tasks[core_id];
    if (task_id != table.Size()) {
      return Fail("core " + tok[1] + " 的 task id 必须从 0 连续递增，期望 " +
                  std::to_string(table.Size()) + "，实际 " + tok[2]);
    }
    table.Append(e);
    return true;
  }

  bool TmetaRec() {
    if (!NeedExact(5)) return false;
    uint64_t core_id = 0, task_id = 0;
    if (!U64(1, &core_id) || !U64(2, &task_id)) return false;
    meta_refs.push_back({core_id, task_id, line_no});

    TaskMeta* m = ir->metas.Find(core_id, task_id);
    if (m == nullptr) {
      ir->metas.Set(core_id, task_id, TaskMeta());
      m = ir->metas.Find(core_id, task_id);
    }
    std::string const& key = tok[3];
    std::string const& value = tok[4];

    if (key == "recv_init") return Flag(value, &m->recv_init);
    if (key == "no_credit_return") return Flag(value, &m->no_credit_return);
    if (key == "local") return Flag(value, &m->local);
    if (key == "local_compute") return Flag(value, &m->local_compute);
    if (key == "res_sum_local") return Flag(value, &m->res_sum_local);
    if (key == "require_dynamic_hitmap") return Flag(value, &m->require_dynamic_hitmap);
    if (key == "dynamic_hitmap") return Flag(value, &m->dynamic_hitmap);
    if (key == "dsa_local_transfer") return Flag(value, &m->dsa_local_transfer);
    if (key == "semantic_op") {
      if (!ParseSemanticOp(value, &m->semantic_op)) return Fail("不认识的 semantic_op " + value);
      return true;
    }
    if (key == "dsa_route") {
      if (!ParseDsaRoute(value, &m->dsa_route)) return Fail("不认识的 dsa_route " + value);
      return true;
    }
    if (key == "source_endpoint") {
      if (!ParseEndpoint(value, &m->source_endpoint)) return Fail("端点只能是 M 或 C，实际 " + value);
      return true;
    }
    if (key == "destination_endpoint") {
      if (!ParseEndpoint(value, &m->destination_endpoint)) return Fail("端点只能是 M 或 C，实际 " + value);
      return true;
    }
    if (key == "hitmap_source") { m->hitmap_source = value; return true; }
    if (key == "payload_role") { m->payload_role = value; return true; }
    if (key == "wire_tag") {
      uint64_t v = 0;
      if (!U64(4, &v)) return false;
      m->wire_tag = v;
      return true;
    }
    return Fail("不认识的任务元数据键 " + key);
  }

  bool Flag(std::string const& value, bool* out) {
    if (value == "1") { *out = true; return true; }
    if (value == "0") { *out = false; return true; }
    return Fail("开关型元数据的取值只能是 0 或 1，实际 " + value);
  }

  bool CreditRec() {
    if (!Need(4)) return false;
    uint64_t core_id = 0, task_id = 0;
    if (!U64(1, &core_id) || !U64(2, &task_id)) return false;
    std::vector<int64_t> targets;
    for (size_t i = 3; i < tok.size(); ++i) {
      int64_t t = 0;
      if (!I64(i, &t)) return false;
      targets.push_back(t);
    }
    credit_refs.push_back({core_id, task_id, line_no});
    ir->credits.Set(core_id, task_id, std::move(targets));
    return true;
  }

  bool CreditEdgeRec() {
    if (!NeedExact(4)) return false;
    CreditEdge e;
    if (!U64(1, &e.src_core) || !U64(2, &e.dst_core)) return false;
    if (!ParseCoreType(tok[3], &e.dst_type)) {
      return Fail("不认识的 core 类型 " + tok[3]);
    }
    ir->credit_edges.push_back(e);
    return true;
  }

  bool SkipSrcRec() {
    if (!Need(6)) return false;
    uint64_t core_id = 0, task_id = 0;
    SkipSource s;
    if (!U64(1, &core_id) || !U64(2, &task_id) || !I64(3, &s.sender)) return false;
    s.phase_type = tok[4];
    if (!U32(5, &s.phase_idx)) return false;
    for (size_t i = 6; i < tok.size(); ++i) {
      uint32_t g = 0;
      if (!U32(i, &g)) return false;
      s.sender_group_ids.push_back(g);
    }
    skip_refs.push_back({core_id, task_id, line_no});
    ir->skip_sources.Set(core_id, task_id, s);
    return true;
  }

  bool ExtNodeRec() {
    if (!NeedExact(10)) return false;
    ExtNode n;
    n.name = tok[1];
    if (!ParseExtKind(tok[2], &n.kind)) return Fail("不认识的外部节点类型 " + tok[2]);
    int64_t row = 0, col = 0;
    if (!I64(3, &row) || !I64(4, &col) || !U64(5, &n.target_core) ||
        !U64(6, &n.volume)) return false;
    if (!ParsePort(tok[7], &n.port)) return Fail("不认识的端口名 " + tok[7]);
    if (!U64(8, &n.pcie_bandwidth) || !U64(9, &n.pcie_delay)) return false;
    n.coord.row = static_cast<int32_t>(row);
    n.coord.col = static_cast<int32_t>(col);

    if (ir->ext_by_name.count(n.name)) return Fail("外部节点名字 " + n.name + " 重复");
    uint64_t key = EncodeCoord(n.coord);
    if (ir->ext_by_coord.count(key)) return Fail("外部节点 " + n.name + " 的坐标与另一个节点重合");
    uint32_t idx = static_cast<uint32_t>(ir->ext_nodes.size());
    ir->ext_nodes.push_back(n);
    ir->ext_by_name[n.name] = idx;
    ir->ext_by_coord[key] = idx;
    return true;
  }

  bool GatewayRec() {
    if (!Need(4)) return false;
    Gateway g;
    if (!U32(1, &g.chip_id)) return false;
    if (!ParseDirection(tok[2], &g.dir)) return Fail("不认识的方向 " + tok[2]);
    for (size_t i = 3; i < tok.size(); ++i) {
      uint32_t c = 0;
      if (!U32(i, &c)) return false;
      g.local_cores.push_back(c);
    }
    ir->gateways.push_back(std::move(g));
    return true;
  }

  bool DirAttrRec() {
    if (!NeedExact(5)) return false;
    DirAttr a;
    if (!U32(1, &a.chip_id)) return false;
    if (!ParseDirection(tok[2], &a.dir)) return Fail("不认识的方向 " + tok[2]);
    if (!U64(3, &a.bandwidth) || !U64(4, &a.delay)) return false;
    ir->dir_attrs.push_back(a);
    return true;
  }

  bool ChipLinkRec() {
    if (!NeedExact(7)) return false;
    ChipLink l;
    if (!U64(1, &l.src_core)) return false;
    if (!ParsePort(tok[2], &l.src_port)) return Fail("不认识的端口名 " + tok[2]);
    if (!U64(3, &l.dst_core)) return false;
    if (!ParsePort(tok[4], &l.dst_port)) return Fail("不认识的端口名 " + tok[4]);
    if (!U64(5, &l.bandwidth) || !U64(6, &l.delay)) return false;
    ir->chip_links.push_back(l);
    return true;
  }

  bool PcieSwRec() {
    if (!NeedExact(5)) return false;
    PcieSwitchInfo s;
    s.id = tok[1];
    int64_t chip = 0, row = 0, col = 0;
    if (!I64(2, &chip) || !I64(3, &row) || !I64(4, &col)) return false;
    s.chip = static_cast<int32_t>(chip);
    s.coord.row = static_cast<int32_t>(row);
    s.coord.col = static_cast<int32_t>(col);
    ir->pcie_switches.push_back(s);
    return true;
  }

  bool PcieEndAt(size_t idx, PcieEnd* out) {
    if (!ParsePcieEndKind(tok[idx], &out->kind)) return Fail("不认识的 PCIe 端点类型 " + tok[idx]);
    out->ref = tok[idx + 1];
    if (out->kind == PcieEndKind::kSwitch) {
      if (tok[idx + 2] == "UNKNOWN") return Fail("交换节点那一端必须写端口名");
      out->port_name = tok[idx + 2];
      return true;
    }
    if (!ParsePort(tok[idx + 2], &out->port)) return Fail("不认识的端口名 " + tok[idx + 2]);
    return true;
  }

  bool PcieLinkRec() {
    if (!NeedExact(10)) return false;
    PcieLink l;
    l.id = tok[1];
    if (!PcieEndAt(2, &l.a) || !PcieEndAt(5, &l.b)) return false;
    if (!U64(8, &l.bandwidth) || !U64(9, &l.delay)) return false;
    ir->pcie_links.push_back(l);
    return true;
  }

  bool PcieRouteRec() {
    if (!NeedExact(5)) return false;
    PcieRoute r;
    r.sw_id = tok[1];
    int64_t row = 0, col = 0;
    if (!I64(2, &row) || !I64(3, &col)) return false;
    r.out_port = tok[4];
    r.dst.row = static_cast<int32_t>(row);
    r.dst.col = static_cast<int32_t>(col);
    ir->pcie_routes.push_back(r);
    return true;
  }

  bool PcieInRouteRec() {
    if (!NeedExact(6)) return false;
    PcieInRoute r;
    r.sw_id = tok[1];
    r.in_port = tok[2];
    int64_t row = 0, col = 0;
    if (!I64(3, &row) || !I64(4, &col)) return false;
    r.out_port = tok[5];
    r.dst.row = static_cast<int32_t>(row);
    r.dst.col = static_cast<int32_t>(col);
    ir->pcie_in_routes.push_back(r);
    return true;
  }

  bool HostGroupRec() {
    if (!NeedExact(4)) return false;
    HostGroupTarget h;
    h.host_name = tok[1];
    if (!U32(2, &h.group_id) || !U64(3, &h.target_core)) return false;
    host_name_refs.push_back({h.host_name, line_no});
    ir->host_group_targets.push_back(h);
    return true;
  }

  // ---- Phase1 与以太网交换节点

  bool Phase1LaneRec() {
    if (!NeedExact(13)) return false;
    Phase1LaneInfo l;
    l.name = tok[1];
    int64_t group = 0;
    if (!U64(2, &l.layer_id) || !I64(3, &group)) return false;
    l.group_id = group;
    // 一路不发时写一个减号，不是空字符串：空字符串在按空白切分的行里留不下来
    l.bypass_sink = tok[4] == "-" ? "" : tok[4];
    l.moe_sink = tok[5] == "-" ? "" : tok[5];
    if (!U64(6, &l.bypass_volume) || !U64(7, &l.moe_volume)) return false;
    if (!U64(8, &l.fc0_delay) || !U64(9, &l.res_delay)) return false;
    if (!U64(10, &l.norm_delay) || !U64(11, &l.router_delay)) return false;
    if (!U64(12, &l.push_delay)) return false;
    if (l.bypass_sink.empty() && l.moe_sink.empty()) {
      return Fail("PHASE1LANE " + l.name + " 两路都没有落点，它什么也不发");
    }
    if (!l.bypass_sink.empty() && l.bypass_sink == l.moe_sink) {
      return Fail("PHASE1LANE " + l.name + " 的两路落在同一处，下一跳就分不开了");
    }
    for (auto const& x : ir->phase1_lanes) {
      if (x.name == l.name) return Fail("PHASE1LANE " + l.name + " 出现两次");
    }
    ir->phase1_lanes.push_back(std::move(l));
    return true;
  }

  Phase1LaneInfo* MutableLane(std::string const& name) {
    for (auto& l : ir->phase1_lanes) {
      if (l.name == name) return &l;
    }
    return nullptr;
  }

  bool Phase1HitRec() {
    if (!Need(3)) return false;
    lane_refs.push_back({tok[1], line_no});
    Phase1LaneInfo* l = MutableLane(tok[1]);
    if (l == nullptr) return Fail("PHASE1HIT 的通道 " + tok[1] + " 还没有 PHASE1LANE");
    for (size_t i = 2; i < tok.size(); ++i) {
      uint32_t v = 0;
      if (!U32(i, &v)) return false;
      l->hit_map.push_back(v);
    }
    return true;
  }

  bool Phase1UserRec() {
    if (!Need(3)) return false;
    lane_refs.push_back({tok[1], line_no});
    Phase1LaneInfo* l = MutableLane(tok[1]);
    if (l == nullptr) return Fail("PHASE1USER 的通道 " + tok[1] + " 还没有 PHASE1LANE");
    for (size_t i = 2; i < tok.size(); ++i) {
      uint64_t v = 0;
      if (!U64(i, &v)) return false;
      l->uids.push_back(v);
    }
    return true;
  }

  bool Phase1SinkRec() {
    if (!NeedExact(3)) return false;
    Phase1SinkInfo s;
    s.name = tok[1];
    s.role = tok[2];
    if (s.role != "GENERIC" && s.role != "MOE" && s.role != "BYPASS") {
      return Fail("不认识的落点角色 " + s.role);
    }
    for (auto const& x : ir->phase1_sinks) {
      if (x.name == s.name) return Fail("PHASE1SINK " + s.name + " 出现两次");
    }
    ir->phase1_sinks.push_back(std::move(s));
    return true;
  }

  bool EthSwRec() {
    if (!NeedExact(7)) return false;
    if (ir->eth.present) return Fail("ETHSW 只能有一条");
    EthConfig& e = ir->eth;
    if (!U64(1, &e.processing_delay) || !U64(2, &e.header_bytes)) return false;
    if (!U64(3, &e.payload_numel) || !U64(4, &e.payload_bytes_per_elem)) {
      return false;
    }
    if (!U32(5, &e.expert_group_size) || !U32(6, &e.expert_count)) return false;
    if (e.expert_group_size == 0) return Fail("ETHSW 的组大小必须为正");
    if (e.payload_bytes_per_elem == 0) return Fail("ETHSW 的元素字节数必须为正");
    e.present = true;
    return true;
  }

  static bool KnownEthPort(std::string const& id) {
    return id == "res_ingress" || id == "phase1_moe_ingress" ||
           id == "phase2_moe_result_ingress" ||
           id == "phase2_moe_request_egress" || id == "phase3_res_join_egress";
  }

  bool EthPortRec() {
    if (!NeedExact(6)) return false;
    EthPortInfo p;
    p.port_id = tok[1];
    if (!KnownEthPort(p.port_id)) return Fail("不认识的交换节点端口 " + p.port_id);
    if (!U64(2, &p.bandwidth) || !U64(3, &p.propagation)) return false;
    if (!U64(4, &p.queue_capacity) || !U64(5, &p.pending_capacity)) return false;
    if (p.bandwidth == 0) return Fail("ETHPORT " + p.port_id + " 的带宽必须为正");
    for (auto const& x : ir->eth.ports) {
      if (x.port_id == p.port_id) return Fail("ETHPORT " + p.port_id + " 出现两次");
    }
    ir->eth.ports.push_back(std::move(p));
    return true;
  }

  bool EthRouteRec() {
    if (!NeedExact(3)) return false;
    EthRouteInfo r;
    r.in_port = tok[1];
    r.out_port = tok[2];
    if (!KnownEthPort(r.in_port)) return Fail("不认识的交换节点端口 " + r.in_port);
    if (!KnownEthPort(r.out_port)) return Fail("不认识的交换节点端口 " + r.out_port);
    for (auto const& x : ir->eth.routes) {
      if (x.in_port == r.in_port) {
        return Fail("ETHROUTE 的入口 " + r.in_port + " 有两条出路");
      }
    }
    ir->eth.routes.push_back(std::move(r));
    return true;
  }

  bool EthModeRec() {
    if (!NeedExact(5)) return false;
    uint64_t a = 0, b = 0, c = 0, d = 0;
    if (!U64(1, &a) || !U64(2, &b) || !U64(3, &c) || !U64(4, &d)) return false;
    if (a > 1 || b > 1 || c > 1 || d > 1) return Fail("ETHMODE 的开关只能是 0 或 1");
    ir->eth.complete_phase1_boundary = a != 0;
    ir->eth.phase2_ingress = b != 0;
    ir->eth.inject_host = c != 0;
    ir->eth.result_bridge = d != 0;
    return true;
  }

  bool ExpGroupRec() {
    if (!Need(3)) return false;
    ExpertGroupEntry e;
    if (!U32(1, &e.expert_id)) return false;
    for (size_t i = 2; i < tok.size(); ++i) {
      uint32_t v = 0;
      if (!U32(i, &v)) return false;
      for (uint32_t g : e.groups) {
        if (g == v) return Fail("EXPGROUP 里同一个 group 写了两次");
      }
      e.groups.push_back(v);
    }
    for (auto const& x : ir->eth.expert_groups) {
      if (x.expert_id == e.expert_id) {
        return Fail("EXPGROUP 的专家 " + tok[1] + " 出现两次");
      }
    }
    ir->eth.expert_groups.push_back(std::move(e));
    return true;
  }

  bool EpGroupRec() {
    if (!NeedExact(4)) return false;
    EpGroup g;
    if (!U32(1, &g.group_id) || !I64(2, &g.shared_ep_id) ||
        !U32(3, &g.routed_ep_num)) return false;
    ir->ep_groups.push_back(g);
    return true;
  }

  bool HostBindRec() {
    if (!NeedExact(3)) return false;
    HostBind b;
    b.host_name = tok[1];
    if (!U32(2, &b.group_id)) return false;
    host_name_refs.push_back({b.host_name, line_no});
    ir->host_binds.push_back(b);
    return true;
  }

  bool TotalUsersRec() {
    if (!NeedExact(2)) return false;
    if (saw_total_users) return Fail("TOTALUSERS 只能有一条");
    if (!U64(1, &ir->total_users)) return false;
    saw_total_users = true;
    return true;
  }

  bool TotalPacketsRec() {
    if (!NeedExact(2)) return false;
    if (saw_total_packets) return Fail("TOTALPACKETS 只能有一条");
    if (!U64(1, &ir->total_packets)) return false;
    saw_total_packets = true;
    return true;
  }

  bool UserRec() {
    if (!NeedExact(4)) return false;
    UserInfo u;
    if (!U64(1, &u.uid)) return false;
    if (!ParseSrcKind(tok[2], &u.src_kind)) return Fail("不认识的注入源类型 " + tok[2]);
    if (ir->user_by_uid.count(u.uid)) return Fail("uid " + tok[1] + " 出现两条 USER 记录");
    user_src_refs.push_back({u.uid, tok[3], line_no});
    uint32_t idx = static_cast<uint32_t>(ir->users.size());
    ir->users.push_back(u);
    ir->user_by_uid[u.uid] = idx;
    return true;
  }

  // uid 的记录可能出现在 USER 之前，所以这些先攒着，第二遍再落到 UserInfo 上。
  bool UserTargetRec() {
    if (!NeedExact(5)) return false;
    PendingTarget t;
    if (!U64(1, &t.uid)) return false;
    t.host_name = tok[2];
    if (!U64(3, &t.target_core) || !U64(4, &t.tag)) return false;
    t.line = line_no;
    host_name_refs.push_back({t.host_name, line_no});
    pending_targets.push_back(t);
    return true;
  }

  bool HitMapRec() {
    if (!Need(2)) return false;
    PendingGroups g;
    if (!U64(1, &g.uid)) return false;
    for (size_t i = 2; i < tok.size(); ++i) {
      uint32_t v = 0;
      if (!U32(i, &v)) return false;
      g.groups.push_back(v);
    }
    g.line = line_no;
    pending_hitmaps.push_back(std::move(g));
    return true;
  }

  bool BitMapRec() {
    if (!NeedExact(4)) return false;
    PendingBitmap b;
    if (!U64(1, &b.uid) || !U32(2, &b.group_id) || !U32(3, &b.count)) return false;
    b.line = line_no;
    pending_bitmaps.push_back(b);
    return true;
  }

  bool OutFragRec() {
    if (!NeedExact(3)) return false;
    PendingFrag f;
    if (!U64(1, &f.uid) || !U64(2, &f.fragments)) return false;
    f.line = line_no;
    pending_frags.push_back(f);
    return true;
  }

  bool DynHitMapRec() {
    if (!Need(4)) return false;
    DynHitMap d;
    if (!U64(1, &d.uid) || !U64(2, &d.layer_id) || !U64(3, &d.core_id)) return false;
    for (size_t i = 4; i < tok.size(); ++i) {
      uint32_t v = 0;
      if (!U32(i, &v)) return false;
      d.groups.push_back(v);
    }
    dyn_refs.push_back({d.uid, line_no});
    ir->dyn_hit_maps.push_back(std::move(d));
    return true;
  }

  // ---- 第二遍

  bool AtLine(uint64_t ln, std::string const& msg) {
    line_no = ln;
    return Fail(msg);
  }

  bool TaskAt(uint64_t core_id, uint64_t task_id, TaskEntry const** out) {
    auto it = ir->tasks.find(core_id);
    if (it == ir->tasks.end() || !it->second.Has(task_id)) return false;
    *out = &it->second.At(task_id);
    return true;
  }

  bool Validate() {
    line_no = 0;
    if (!saw_dim) return Fail("缺 DIM 记录");
    if (!saw_total_users) return Fail("缺 TOTALUSERS 记录");
    if (!saw_total_packets) return Fail("缺 TOTALPACKETS 记录");
    if (ir->cores.empty()) return Fail("至少要有一条 CORE 记录");
    if (ir->ext_nodes.empty()) return Fail("至少要有一条 EXTNODE 记录");
    if (ir->users.empty()) return Fail("至少要有一条 USER 记录");

    for (auto const& kv : ir->tasks) {
      if (!ir->cores.count(kv.first)) {
        return Fail("core " + std::to_string(kv.first) + " 有任务表但没有 CORE 记录");
      }
    }

    // TASK 里指向外部节点的标识符
    for (auto const& p : pending_ext) {
      auto it = ir->ext_by_name.find(p.name);
      if (it == ir->ext_by_name.end()) {
        return AtLine(p.line, "任务目标 " + p.name + " 没有对应的 EXTNODE");
      }
      auto tit = ir->tasks.find(p.core_id);
      if (tit == ir->tasks.end() || !tit->second.Has(p.task_id)) {
        return AtLine(p.line, "内部错误：任务行丢失");
      }
      tit->second.Mutable(p.task_id).down_ext = it->second;
    }

    for (auto const& r : meta_refs) {
      TaskEntry const* e = nullptr;
      if (!TaskAt(r.core_id, r.task_id, &e)) {
        return AtLine(r.line, "TMETA 指向的任务行不存在");
      }
    }

    for (auto const& r : credit_refs) {
      TaskEntry const* e = nullptr;
      if (!TaskAt(r.core_id, r.task_id, &e)) {
        return AtLine(r.line, "CREDIT 指向的任务行不存在");
      }
      if (e->unit != UnitType::kCu) {
        return AtLine(r.line, "CREDIT 只能挂在 CU 行上");
      }
    }

    for (auto const& r : skip_refs) {
      TaskEntry const* e = nullptr;
      if (!TaskAt(r.core_id, r.task_id, &e)) {
        return AtLine(r.line, "SKIPSRC 指向的任务行不存在");
      }
      if (e->unit != UnitType::kSkip) {
        return AtLine(r.line, "SKIPSRC 只能挂在 SKIP 行上");
      }
    }

    // DTE 行落在核阵列外的目标必须是某个外部节点或某个 PCIe 交换节点
    std::unordered_set<uint64_t> anchor;
    for (auto const& s : ir->pcie_switches) anchor.insert(EncodeCoord(s.coord));
    for (auto const& kv : ir->tasks) {
      for (uint64_t t = 0; t < kv.second.Size(); ++t) {
        TaskEntry const& e = kv.second.At(t);
        if (e.unit != UnitType::kDte) continue;
        if (ir->dim.Inside(e.dst)) continue;
        uint64_t key = EncodeCoord(e.dst);
        if (ir->ext_by_coord.count(key) || anchor.count(key)) continue;
        return Fail("core " + std::to_string(kv.first) + " 的第 " +
                    std::to_string(t) + " 行目标坐标落在核阵列外，"
                    "却没有对应的 EXTNODE 或 PCIESW");
      }
    }

    for (auto const& r : host_name_refs) {
      auto it = ir->ext_by_name.find(r.name);
      if (it == ir->ext_by_name.end()) {
        return AtLine(r.line, "找不到名为 " + r.name + " 的 EXTNODE");
      }
      if (ir->ext_nodes[it->second].kind != ExtKind::kHost) {
        return AtLine(r.line, r.name + " 不是 HOST");
      }
    }

    // 片间链路两端都要在核阵列内，且两个方向成对
    std::unordered_set<std::string> link_keys;
    for (auto const& l : ir->chip_links) {
      if (l.src_core >= ir->dim.CoreNum() || l.dst_core >= ir->dim.CoreNum()) {
        return Fail("CHIPLINK 的 core id 超出核阵列范围");
      }
      link_keys.insert(LinkKey(l.src_core, l.src_port, l.dst_core, l.dst_port));
    }
    for (auto const& l : ir->chip_links) {
      if (!link_keys.count(LinkKey(l.dst_core, l.dst_port, l.src_core, l.src_port))) {
        return Fail("CHIPLINK " + std::to_string(l.src_core) + " 到 " +
                    std::to_string(l.dst_core) + " 缺反方向的那一条");
      }
    }

    if (!ValidatePcie()) return false;
    if (!ValidatePhase()) return false;

    // 按 uid 归位
    for (auto const& p : pending_targets) {
      UserInfo* u = MutableUser(p.uid);
      if (u == nullptr) return AtLine(p.line, "USERTARGET 的 uid 没有 USER 记录");
      UserTarget t;
      t.host_ext = ir->ext_by_name[p.host_name];
      t.target_core = p.target_core;
      t.tag = p.tag;
      u->targets.push_back(t);
    }
    for (auto const& p : pending_hitmaps) {
      UserInfo* u = MutableUser(p.uid);
      if (u == nullptr) return AtLine(p.line, "HITMAP 的 uid 没有 USER 记录");
      u->hit_map = p.groups;
    }
    for (auto const& p : pending_bitmaps) {
      UserInfo* u = MutableUser(p.uid);
      if (u == nullptr) return AtLine(p.line, "BITMAP 的 uid 没有 USER 记录");
      u->moe_bitmap.emplace_back(p.group_id, p.count);
    }
    for (auto const& p : pending_frags) {
      UserInfo* u = MutableUser(p.uid);
      if (u == nullptr) return AtLine(p.line, "OUTFRAG 的 uid 没有 USER 记录");
      u->expected_fragments = p.fragments;
    }
    for (auto const& r : dyn_refs) {
      if (MutableUser(r.uid) == nullptr) {
        return AtLine(r.line, "DHITMAP 的 uid 没有 USER 记录");
      }
    }
    for (auto const& r : user_src_refs) {
      auto it = ir->ext_by_name.find(r.name);
      if (it == ir->ext_by_name.end()) {
        return AtLine(r.line, "USER 的注入源 " + r.name + " 没有对应的 EXTNODE");
      }
      MutableUser(r.uid)->src_ext = it->second;
    }

    for (auto const& u : ir->users) {
      // 从 Phase1 通道进来的 user 不写注入目标：它进哪个核要等交换节点把专家换算
      // 成 EPGroup 之后才知道，编译期定不下来。
      if (u.src_kind == SrcKind::kPhase1Lane) continue;
      if (u.targets.empty()) {
        return Fail("uid " + std::to_string(u.uid) + " 没有任何 USERTARGET");
      }
    }
    return true;
  }

  // Phase1 与以太网交换节点的引用完整性，以及那四个开关之间的前置条件。
  bool ValidatePhase() {
    ExtNode const* node = nullptr;
    auto ext_of = [&](std::string const& name, ExtKind kind) {
      auto it = ir->ext_by_name.find(name);
      if (it == ir->ext_by_name.end()) return false;
      node = &ir->ext_nodes[it->second];
      return node->kind == kind;
    };

    std::unordered_set<std::string> sink_names;
    for (auto const& s : ir->phase1_sinks) {
      if (!ext_of(s.name, ExtKind::kPhase1Sink)) {
        return Fail("PHASE1SINK " + s.name + " 没有对应的 PHASE1_SINK 外部节点");
      }
      sink_names.insert(s.name);
      if (s.role == "GENERIC") continue;
      if (!ir->eth.present) {
        return Fail("落点 " + s.name + " 要往交换节点交包，却没有 ETHSW");
      }
    }

    for (auto const& l : ir->phase1_lanes) {
      if (!ext_of(l.name, ExtKind::kPhase1Lane)) {
        return Fail("PHASE1LANE " + l.name + " 没有对应的 PHASE1_LANE 外部节点");
      }
      for (std::string const* s : {&l.bypass_sink, &l.moe_sink}) {
        if (s->empty()) continue;
        if (!sink_names.count(*s)) {
          return Fail("PHASE1LANE " + l.name + " 的落点 " + *s +
                      " 没有 PHASE1SINK 记录");
        }
      }
      if (!l.moe_sink.empty() && l.hit_map.empty()) {
        return Fail("PHASE1LANE " + l.name + " 要发 MoE 请求，却没有 PHASE1HIT");
      }
      if (l.uids.empty()) {
        return Fail("PHASE1LANE " + l.name + " 没有 PHASE1USER，它一个 user 也不产");
      }
    }

    for (auto const& r : lane_refs) {
      bool found = false;
      for (auto const& l : ir->phase1_lanes) {
        if (l.name == r.name) { found = true; break; }
      }
      if (!found) return AtLine(r.line, "找不到通道 " + r.name);
    }

    if (!ir->eth.present) {
      if (!ir->eth.ports.empty() || !ir->eth.routes.empty() ||
          !ir->eth.expert_groups.empty()) {
        return Fail("有 ETHPORT、ETHROUTE 或 EXPGROUP，却没有 ETHSW");
      }
      return true;
    }

    if (ir->eth.ports.size() != 5) {
      return Fail("ETHSW 的五个端口要各写一条 ETHPORT，实际 " +
                  std::to_string(ir->eth.ports.size()) + " 条");
    }
    if (ir->eth.routes.empty()) return Fail("ETHSW 没有一条 ETHROUTE");
    for (auto const& r : ir->eth.routes) {
      const bool in_ok = r.in_port == "res_ingress" ||
                         r.in_port == "phase1_moe_ingress" ||
                         r.in_port == "phase2_moe_result_ingress";
      if (!in_ok) return Fail("ETHROUTE 的起点 " + r.in_port + " 不是入口");
      const bool out_ok = r.out_port == "phase2_moe_request_egress" ||
                          r.out_port == "phase3_res_join_egress";
      if (!out_ok) return Fail("ETHROUTE 的终点 " + r.out_port + " 不是出口");
    }
    for (auto const& e : ir->eth.expert_groups) {
      if (e.groups.empty()) {
        return Fail("EXPGROUP 的专家至少要落在一个 group 上");
      }
      if (ir->eth.expert_count != 0 && e.expert_id >= ir->eth.expert_count) {
        return Fail("EXPGROUP 的专家号超出 ETHSW 声明的专家数");
      }
    }

    // 四个开关之间的前置条件。少一条，一次工作会被记成两次完成，或者根本没有结果。
    if (ir->eth.phase2_ingress) {
      if (ir->host_binds.empty()) {
        return Fail("要把 MoE 请求注入 Host，就得有 HOSTBIND 说明哪个 group 归谁");
      }
      if (ir->host_group_targets.empty()) {
        return Fail("要把 MoE 请求注入 Host，就得有 HOSTGROUP 说明推到哪个核");
      }
    }
    if (!ir->eth.result_bridge) return true;

    if (!ir->eth.phase2_ingress) {
      return Fail("结果要交回交换节点，就得先有人把请求注入 Phase2");
    }
    if (!ir->eth.inject_host) {
      return Fail("结果要交回交换节点，注入就必须真的推给 Host，否则 Phase2 算不出"
                  "结果");
    }
    if (ir->eth.complete_phase1_boundary) {
      return Fail("结果要交回交换节点，Phase1 边界那个判法就必须关掉：完成权只能"
                  "有一处");
    }
    uint64_t out_num = 0;
    for (auto const& n : ir->ext_nodes) {
      if (n.kind == ExtKind::kOut) ++out_num;
    }
    if (out_num != 1) {
      return Fail("结果要交回交换节点时只支持一个 OUT，实际 " +
                  std::to_string(out_num) + " 个");
    }
    return true;
  }

  // PCIe 交换拓扑的引用完整性。端口有没有真接过线由装配层查，这里只查名字指得到人。
  bool ValidatePcie() {
    std::unordered_set<std::string> sw_ids;
    for (auto const& s : ir->pcie_switches) {
      if (!sw_ids.insert(s.id).second) {
        return Fail("PCIESW 的 id " + s.id + " 出现两次");
      }
    }

    auto check_end = [&](PcieEnd const& e, std::string const& link_id) {
      switch (e.kind) {
        case PcieEndKind::kSwitch:
          if (!sw_ids.count(e.ref)) {
            return Fail("PCIELINK " + link_id + " 指向不存在的 PCIESW " + e.ref);
          }
          return true;
        case PcieEndKind::kCore: {
          uint64_t id = 0;
          if (!ParseU64(e.ref, &id) || id >= ir->dim.CoreNum()) {
            return Fail("PCIELINK " + link_id + " 的 core id " + e.ref +
                        " 超出核阵列范围");
          }
          if (e.port == Port::kUnknown) {
            return Fail("PCIELINK " + link_id + " 的核那一端必须写端口");
          }
          return true;
        }
        default: {
          auto it = ir->ext_by_name.find(e.ref);
          if (it == ir->ext_by_name.end()) {
            return Fail("PCIELINK " + link_id + " 指向不存在的 EXTNODE " + e.ref);
          }
          const ExtKind want =
              e.kind == PcieEndKind::kHost ? ExtKind::kHost : ExtKind::kOut;
          if (ir->ext_nodes[it->second].kind != want) {
            return Fail("PCIELINK " + link_id + " 的 " + e.ref + " 类型对不上");
          }
          return true;
        }
      }
    };

    for (auto const& l : ir->pcie_links) {
      if (l.a.kind != PcieEndKind::kSwitch && l.b.kind != PcieEndKind::kSwitch) {
        return Fail("PCIELINK " + l.id + " 至少有一端要是 PCIESW");
      }
      if (!check_end(l.a, l.id) || !check_end(l.b, l.id)) return false;
    }

    for (auto const& r : ir->pcie_routes) {
      if (!sw_ids.count(r.sw_id)) {
        return Fail("PCIEROUTE 指向不存在的 PCIESW " + r.sw_id);
      }
    }
    for (auto const& r : ir->pcie_in_routes) {
      if (!sw_ids.count(r.sw_id)) {
        return Fail("PCIEIROUTE 指向不存在的 PCIESW " + r.sw_id);
      }
    }
    if (!ir->pcie_links.empty() && ir->pcie_routes.empty() &&
        ir->pcie_in_routes.empty()) {
      return Fail("有 PCIELINK 就必须有 PCIEROUTE 或 PCIEIROUTE，交换节点没有默认路由");
    }
    return true;
  }

  static std::string LinkKey(uint64_t a, Port ap, uint64_t b, Port bp) {
    return std::to_string(a) + "/" + std::to_string(static_cast<uint32_t>(ap)) +
           "-" + std::to_string(b) + "/" +
           std::to_string(static_cast<uint32_t>(bp));
  }

  UserInfo* MutableUser(uint64_t uid) {
    auto it = ir->user_by_uid.find(uid);
    return it == ir->user_by_uid.end() ? nullptr : &ir->users[it->second];
  }

  // ---- 状态

  struct ExtRef { uint64_t core_id; uint64_t task_id; std::string name; uint64_t line; };
  struct TaskRef { uint64_t core_id; uint64_t task_id; uint64_t line; };
  struct NameRef { std::string name; uint64_t line; };
  struct UserSrcRef { uint64_t uid; std::string name; uint64_t line; };
  struct UidRef { uint64_t uid; uint64_t line; };
  struct PendingTarget { uint64_t uid; std::string host_name; uint64_t target_core;
                         uint64_t tag; uint64_t line; };
  struct PendingGroups { uint64_t uid; std::vector<uint32_t> groups; uint64_t line; };
  struct PendingBitmap { uint64_t uid; uint32_t group_id; uint32_t count; uint64_t line; };
  struct PendingFrag { uint64_t uid; uint64_t fragments; uint64_t line; };

  static constexpr uint64_t kFormatVersion = 5;

  std::string path;
  BachIr* ir;
  std::string* err;
  std::vector<std::string> tok;
  uint64_t line_no = 0;
  bool saw_version = false;
  bool saw_dim = false;
  bool saw_total_users = false;
  bool saw_total_packets = false;

  std::vector<ExtRef> pending_ext;
  std::vector<TaskRef> meta_refs, credit_refs, skip_refs;
  std::vector<NameRef> host_name_refs;
  std::vector<NameRef> lane_refs;
  std::vector<UserSrcRef> user_src_refs;
  std::vector<UidRef> dyn_refs;
  std::vector<PendingTarget> pending_targets;
  std::vector<PendingGroups> pending_hitmaps;
  std::vector<PendingBitmap> pending_bitmaps;
  std::vector<PendingFrag> pending_frags;
};

}

// 出错时把原因写进 err 并返回 false，out 的内容不保证完整。
inline bool TryLoadBachIr(std::string const& path, BachIr* out, std::string* err) {
  LOGCHECK(out != nullptr && err != nullptr, "TryLoadBachIr: null output.");
  *out = BachIr();
  err->clear();
  detail::Parser parser(path, out, err);
  return parser.Run();
}

// 装配路径用这个，出错就停机。
inline BachIr LoadBachIr(std::string const& path) {
  BachIr ir;
  std::string err;
  if (!TryLoadBachIr(path, &ir, &err)) {
    spdlog::error("读中间文件失败：{}", err);
    LOGCHECK(false, "LoadBachIr failed, see the error above.");
  }
  ir.params.Validate();
  return ir;
}

}
}

#endif
