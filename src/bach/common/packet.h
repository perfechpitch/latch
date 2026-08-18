#ifndef _LATCH_BACH_COMMON_PACKET_
#define _LATCH_BACH_COMMON_PACKET_

// Bach 的封包、身份与枚举。
//
// 模型里没有全局唯一的包 id。每个身份字段各自在一定范围内唯一，谁分配它也各不
// 相同，所以任何地方都不能拿单个字段当主键：
//
//   uid          一次 run 内全局唯一，由注入源分配
//   tid          只在单个 Core 的任务表内唯一，Core 之间自由复用
//   tag          随 opcode 变化：RETIRE 与 REDUCE 族携带发送核 id，USER_INIT
//                携带 MoEBitMap 值，RES 携带任务元数据里的 wire_tag
//   layer_id     Phase1 层号，非 Phase1 流量恒为 0
//   beat_id      在一个包内唯一，取值 0 到 total_fragments-1，发送侧逐拍递增
//   fragment_id  一拍被某条链路切分后的一组片内唯一，切它的 Router 分配
//
// 由此得到两条组合键：
//
//   收包去重与收齐判定  BeatKey 加 beat_id
//   转发前的整拍重组    BeatKey 加 beat_id 加 xfer_id
//
// 最后那一维在 Bach 里是承载该拍的 Python 对象身份 id(payload)，C++ 侧没有对应
// 物，改由发送侧给每一拍显式分配一个传输实例号 xfer_id。少了它，两个 uid、tid、
// tag、opcode 全同的包在同一跳会被误当成同一拍的两个片。
//
// 链路上流动的是 RoutedPkt，一拍的数据被切成若干片，各片共享同一个 CommInst：
// CommInst 挂在 LogicPtr 下，Push 只搬 shared_ptr，不深拷 HitMap 那些变长字段。

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "base/logic.h"
#include "base/time_stamp.h"

namespace latch {
namespace bach {

// ---------------------------------------------------------------- 枚举

// 任务表第 0 位，决定 TaskScheduler 把这一行派给哪个单元。
// SKIP 行不派发任何单元，它是接收侧的占位：走到该行时挂起，等 DTE 收到对端封包
// 之后 ack 它。
enum class UnitType : uint32_t {
  kDte = 0,
  kMc = 1,
  kVc = 2,
  kCu = 3,
  kSkip = 4,
};

// 任务表第 1 位，同一个 opcode 在发送侧与接收侧的行为不同。
// kReduce 与 kReduction 是两种任务语义，不能合并：前者直接做加法，后者的首包要
// 先写 MatrixMem 登记该 user 再走加法。
enum class Opcode : uint32_t {
  kUserInit = 0,
  kMove = 1,
  kReduce = 2,
  kReduction = 3,
  kConcat = 4,
  kRetire = 5,
  kFifoIn = 6,
  kFifoOut = 7,
  kRes = 8,
  kBypass = 9,
  kDontCare = 10,
};

// Router 的 12 个端口位。端口名直接对应物理方向与 PCIe 方向。
enum class Port : uint32_t {
  kLocal = 0,
  kNorth = 1,
  kEast = 2,
  kSouth = 3,
  kWest = 4,
  kPcieUp = 5,
  kPcieDown = 6,
  kPcieNorth = 7,
  kPcieSouth = 8,
  kPcieEast = 9,
  kPcieWest = 10,
  kUnknown = 11,
};

constexpr uint32_t kPortNum = 12;

// 下游收包时的入端口取本次出端口的反向。
inline Port OppositePort(Port p) {
  switch (p) {
    case Port::kNorth: return Port::kSouth;
    case Port::kSouth: return Port::kNorth;
    case Port::kEast: return Port::kWest;
    case Port::kWest: return Port::kEast;
    case Port::kPcieUp: return Port::kPcieDown;
    case Port::kPcieDown: return Port::kPcieUp;
    case Port::kPcieNorth: return Port::kPcieSouth;
    case Port::kPcieSouth: return Port::kPcieNorth;
    case Port::kPcieEast: return Port::kPcieWest;
    case Port::kPcieWest: return Port::kPcieEast;
    case Port::kLocal: return Port::kLocal;
    default: return Port::kUnknown;
  }
}

inline bool IsPciePort(Port p) {
  return p == Port::kPcieUp || p == Port::kPcieDown || p == Port::kPcieNorth ||
         p == Port::kPcieSouth || p == Port::kPcieEast || p == Port::kPcieWest;
}

// 由 Map 的入边关系判定，决定 credit 开户额度与 Host 的 credit 容量。
enum class CoreType : uint32_t {
  kNormal = 0,
  kBroadcast = 1,
  kReduction = 2,
};

// 一个 Core 的 DTE 在首个用户到达时定下存储模式，之后不允许混用。
enum class StorageMode : uint32_t {
  kUnset = 0,
  kCore = 1,
  kFifo = 2,
  kReduction = 3,
};

// 屏障 SKIP 行：走到它时挂起等对端数据，不像其它 SKIP 行那样本地立即完成。
inline bool IsBarrierOpcode(Opcode op) {
  return op == Opcode::kReduce || op == Opcode::kReduction ||
         op == Opcode::kConcat || op == Opcode::kRes;
}

inline const char* OpcodeName(Opcode op) {
  switch (op) {
    case Opcode::kUserInit: return "USER_INIT";
    case Opcode::kMove: return "MOVE";
    case Opcode::kReduce: return "REDUCE";
    case Opcode::kReduction: return "REDUCTION";
    case Opcode::kConcat: return "CONCAT";
    case Opcode::kRetire: return "RETIRE";
    case Opcode::kFifoIn: return "FIFO_IN";
    case Opcode::kFifoOut: return "FIFO_OUT";
    case Opcode::kRes: return "RES";
    case Opcode::kBypass: return "BYPASS";
    default: return "DONTCARE";
  }
}

inline const char* PortName(Port p) {
  switch (p) {
    case Port::kLocal: return "LOCAL";
    case Port::kNorth: return "NORTH";
    case Port::kEast: return "EAST";
    case Port::kSouth: return "SOUTH";
    case Port::kWest: return "WEST";
    case Port::kPcieUp: return "PCIE_UP";
    case Port::kPcieDown: return "PCIE_DOWN";
    case Port::kPcieNorth: return "PCIE_NORTH";
    case Port::kPcieSouth: return "PCIE_SOUTH";
    case Port::kPcieEast: return "PCIE_EAST";
    case Port::kPcieWest: return "PCIE_WEST";
    default: return "UNKNOWN";
  }
}

// ---------------------------------------------------------------- 坐标

// 全局二维坐标。核阵列内的坐标非负；外部节点与 PCIe 交换节点用不落在阵列内的
// 坐标表示，可以为负，所以这里是有符号的。
struct Coord {
  int32_t row = 0;
  int32_t col = 0;

  bool operator==(Coord const& r) const { return row == r.row && col == r.col; }
  bool operator!=(Coord const& r) const { return !(*this == r); }
  bool operator<(Coord const& r) const {
    return row != r.row ? row < r.row : col < r.col;
  }
};

// Logic64 只装 uint64_t，坐标进封包前按位打包，取出时还原。
inline uint64_t EncodeCoord(Coord c) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(c.row)) << 32) |
         static_cast<uint32_t>(c.col);
}

inline Coord DecodeCoord(uint64_t v) {
  Coord c;
  c.row = static_cast<int32_t>(static_cast<uint32_t>(v >> 32));
  c.col = static_cast<int32_t>(static_cast<uint32_t>(v & 0xffff'ffffull));
  return c;
}

struct CoordHash {
  size_t operator()(Coord const& c) const {
    return std::hash<uint64_t>()(EncodeCoord(c));
  }
};

// ---------------------------------------------------------------- 载荷

// 一拍数据的完整描述，被这一拍切出来的所有链路片共享。它不携带张量，vol 只作为
// 字节数参与拍数与延迟换算。
struct CommInst {
  uint64_t uid = 0;
  uint64_t tid = 0;
  Opcode opcode = Opcode::kDontCare;
  uint64_t tag = 0;
  uint64_t layer_id = 0;

  uint64_t beat_id = 0;          // 本拍序号，0 到 total_fragments-1
  uint64_t total_fragments = 1;  // 本包总拍数，发送侧写入
  uint64_t vol = 0;              // 本拍字节数
  uint64_t xfer_id = 0;          // 传输实例号，替代 Bach 的 id(payload)

  // 本次激活的 EPGroup id。RES 与 BYPASS 包强制为空，不继承 DTE 缓存的值。
  std::vector<uint32_t> hit_map;
  // (group_id, 该 group 上命中的专家数)，决定接收核的计算倍率。
  std::vector<std::pair<uint32_t, uint32_t>> moe_bitmap;

  // Phase1 身份，非 Phase1 流量为 -1。同一个 uid 出现互相矛盾的值即报错。
  int64_t phase1_lane_id = -1;
  int64_t phase1_group_id = -1;

  bool HitsGroup(uint32_t group_id) const {
    for (uint32_t g : hit_map) {
      if (g == group_id) return true;
    }
    return false;
  }
};

using CommInstPtr = std::shared_ptr<CommInst>;

// ---------------------------------------------------------------- 组合键

// 收包去重与收齐判定的键。同一个 user 在同一个 Core 上并发的两个任务，如果
// tid、tag、opcode 全同，会被判为同一个包的重复拍并报错，这是协议验证能力的
// 来源，不要为了绕开它去放宽键。
struct BeatKey {
  uint64_t uid = 0;
  uint64_t layer_id = 0;
  uint64_t tid = 0;
  uint64_t tag = 0;
  Opcode opcode = Opcode::kDontCare;

  bool operator==(BeatKey const& r) const {
    return uid == r.uid && layer_id == r.layer_id && tid == r.tid &&
           tag == r.tag && opcode == r.opcode;
  }
};

inline BeatKey MakeBeatKey(CommInst const& inst) {
  BeatKey k;
  k.uid = inst.uid;
  k.layer_id = inst.layer_id;
  k.tid = inst.tid;
  k.tag = inst.tag;
  k.opcode = inst.opcode;
  return k;
}

struct BeatKeyHash {
  size_t operator()(BeatKey const& k) const {
    size_t h = std::hash<uint64_t>()(k.uid);
    auto mix = [&h](uint64_t v) {
      h ^= std::hash<uint64_t>()(v) + 0x9e37'79b9'7f4a'7c15ull + (h << 6) +
           (h >> 2);
    };
    mix(k.layer_id);
    mix(k.tid);
    mix(k.tag);
    mix(static_cast<uint64_t>(k.opcode));
    return h;
  }
};

// 转发前的整拍重组键：同一拍的各片要按这个键累加字节数，收齐 payload_size 才
// 继续往下一跳走。
struct FragKey {
  BeatKey beat;
  uint64_t beat_id = 0;
  uint64_t xfer_id = 0;

  bool operator==(FragKey const& r) const {
    return beat == r.beat && beat_id == r.beat_id && xfer_id == r.xfer_id;
  }
};

struct FragKeyHash {
  size_t operator()(FragKey const& k) const {
    size_t h = BeatKeyHash()(k.beat);
    auto mix = [&h](uint64_t v) {
      h ^= std::hash<uint64_t>()(v) + 0x9e37'79b9'7f4a'7c15ull + (h << 6) +
           (h >> 2);
    };
    mix(k.beat_id);
    mix(k.xfer_id);
    return h;
  }
};

// ---------------------------------------------------------------- 链路封包

// 跨 Core 的唯一载体。每条链路每个方向一个 Fifo<RoutedPkt>，满足 Fifo 的单
// producer 单 consumer 约束。
//
// arrive_cycle 是发送侧按链路模型算出的到达拍，接收侧要到点才取。Fifo 本身固有
// 的一拍延迟由此被吸收，时序 owner 仍然唯一是发送侧，不额外扣时。
class RoutedPkt : public Logic {
 public:
  Logic64 dst;          // 目的坐标，EncodeCoord 编码
  Logic64 src;          // 源坐标，诊断与 PCIe 延迟判定用
  Logic64 size;         // 本片字节数
  Logic64 payload_size; // 整拍字节数，重组判据
  Logic64 byte_offset;  // 本片在整拍里的起始字节
  Logic64 fragment_id;
  Logic64 total_frag;   // 本拍被切成几片
  Logic64 is_tail;
  Logic64 arrive_cycle; // 发送侧算出的到达拍
  Logic64 hop_count;
  Logic64 in_port;      // 入端口，取上一跳出端口的反向
  LogicPtr<CommInst> payload;

  explicit RoutedPkt(ClockPtr c)
      : dst(c), src(c), size(c), payload_size(c), byte_offset(c),
        fragment_id(c), total_frag(c), is_tail(c), arrive_cycle(c),
        hop_count(c), in_port(c), payload(c) {
    Fields(dst, src, size, payload_size, byte_offset);
    Fields(fragment_id, total_frag, is_tail, arrive_cycle, hop_count, in_port);
    Fields(payload);
  }
};

}
}

#endif
