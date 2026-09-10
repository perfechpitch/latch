#ifndef _LATCH_BACH_COMMON_PARAMS_
#define _LATCH_BACH_COMMON_PARAMS_

// Bach 的参数总表与拍数换算。
//
// 这里是全部参数默认值的唯一出处，别处只按名字引用。取值与 Bach 的
// _SimulationConfig.__init__ 一致，中间文件里带来的值在装配期覆盖它们。
//
// 每个值都标了出处等级，这决定重建时怎么对待它：
//
//   一级  声称来自硬件规格，照抄
//   二级  结构性取值，语义明确、数值可调，照抄但需要标定后才能作绝对论断
//   三级  代码里自标为未定或临时的取值，是已知不确定项，注释原样保留
//
// 时间单位是 ns，时钟周期取 1 ns，所以一拍就是一 ns，下面的数值直接就是拍数。
// 带宽单位是 Byte 每拍。

#include <cstdint>

#include "base/log.h"
#include "base/time_stamp.h"

namespace latch {
namespace bach {

// DTE 的两条执行通道是分开还是合一。split 时出入方向各一条，shared 时同一条。
enum class DteExecutionMode : uint32_t {
  kSplit = 0,
  kShared = 1,
};

// 五路径仲裁。开启时要求 split，否则取不到 IN 加 OUT 的双向占用。
enum class DteDsaMode : uint32_t {
  kOff = 0,
  kFiveRoute = 1,
};

struct Params {
  // ------------------------------------------------------------ 时间参数

  uint64_t ts_logic_time = 16;   // 一级。TaskScheduler 每轮取指的固定逻辑时间
  uint64_t dte_setup_time = 85;  // 一级。DTE 出方向与入方向各一次
  uint64_t mu_setup_time = 40;   // 一级。MatrixCore setup
  uint64_t vu_setup_time = 40;   // 一级。VectorCore setup
  uint32_t setup_ahead_depth = 1;  // 一级。准入窗容量为该值加 1

  uint64_t cm_arb_delay = 15;  // 二级。CoreMem 每次访存的基础延迟
  uint64_t mm_arb_delay = 50;  // 二级。MatrixMem 每次访存的基础延迟
  uint64_t noc_router_delay = 1;   // 二级。Router 每跳最小服务时间
  uint64_t noc_wire_delay = 40;    // 二级。非 LOCAL 相关跳的线延迟
  uint64_t noc_access_delay = 10;  // 二级。出口或入口为 LOCAL 的线延迟
  uint64_t cross_chip_delay = 400;    // 二级。同 node 内跨 chip 的 PCIe 延迟
  uint64_t cross_node_delay = 5000;   // 二级。跨 node 的 PCIe 延迟
  uint64_t host_push_delay = 100;     // 二级。Host 每个 user 的推包间隔
  uint64_t bitmap_access_time = 1;    // 二级。MoEBitMap 一次读或写

  // 三级。Bach 原注释：WARNING 这里的时间可能不对
  uint64_t credit_check_time = 4;
  // 三级。Bach 原注释：FIXME 会与 Router Delay 产生 Overlap，为了弥补这个问题，
  // 默认为 BASIC_ROUTER_DELAY 的一半。另一条 FIXME 说本该收到第一拍就能开始算，
  // 因此这里的整包串行有一段重叠没有建模。
  uint64_t dte_reduce_time = 32;

  // ------------------------------------------------------------ 带宽

  uint64_t noc_bandwidth = 128;   // 二级。NoC 链路，同时是 Router buffer 大小
  uint64_t pcie_bandwidth = 128;  // 二级。PCIe 链路，同时是 PCIe buffer 大小
  uint64_t dte_bandwidth = 512;   // 二级。DTE 出口每拍字节数

  // 三级。IF_DTE_MM 的 Bach 原注释：WARNING Just a random number I came up
  // with. Fix it if u need. 其余四项的注释只写了连接关系，未声明出处。
  uint64_t if_dte_cm = 128;   // DTE 到 CoreMem
  uint64_t if_dte_mm = 128;   // DTE 到 MatrixMem
  uint64_t if_vu_cm = 64;     // VectorCore 到 CoreMem
  uint64_t if_mu_cm = 256;    // MatrixCore 到 CoreMem
  uint64_t if_mu_mm = 8192;   // MatrixCore 到 MatrixMem

  // ------------------------------------------------------------ 容量

  uint32_t stream_count = 8;  // 二级。stream 槽位数，同时是普通下游的 credit 额度
  uint64_t matrix_fifo_credit = 4096;  // 二级。Matrix FIFO 深度，同时是
                                       // BROADCAST 与 REDUCTION 下游的额度
  uint64_t num_users = 17;             // 每个 Host 或 dispatcher 生成的 user 数

  // 二级。Router 与交换节点的队列告警阈值，0 表示不告警。它只是告警，不阻塞、
  // 不丢包、不产生反压，上游唯一的反压来源是 credit 与 stream 槽位。
  uint64_t router_queue_warn = 0;

  // 本方案独有：链路 Fifo 的深度。Bach 的入端口队列无上限，这里是容量保护，
  // 深度不足是实现错误而不是静默丢包，所以要给足。
  uint64_t link_fifo_depth = 1024;

  // ------------------------------------------------------------ 看门狗

  uint64_t watchdog_lifespan = 10'000'000;  // 二级。收包与全局看门狗时限
  uint64_t host_credit_timeout = 5'000'000; // 二级。单个 uid 等 credit 的上限
  uint64_t hop_count_warn = 1024;           // 二级
  uint64_t hop_count_err = 12800;           // 二级

  // ------------------------------------------------------------ 模式

  DteExecutionMode dte_execution_mode = DteExecutionMode::kSplit;
  DteDsaMode dte_dsa_mode = DteDsaMode::kOff;

  // 装配前调用。只查参数之间的互斥与前置条件，不查 Map。
  void Validate() const {
    LOGCHECK(stream_count > 0, "Params: stream_count must be positive.");
    LOGCHECK(noc_bandwidth > 0 && pcie_bandwidth > 0 && dte_bandwidth > 0,
             "Params: bandwidth must be positive.");
    LOGCHECK(matrix_fifo_credit > 0,
             "Params: matrix_fifo_credit must be positive.");
    LOGCHECK(link_fifo_depth > 0, "Params: link_fifo_depth must be positive.");
    LOGCHECK(hop_count_warn <= hop_count_err,
             "Params: hop_count_warn must not exceed hop_count_err.");
    LOGCHECK(dte_dsa_mode == DteDsaMode::kOff ||
                 dte_execution_mode == DteExecutionMode::kSplit,
             "Params: five_route requires split execution lanes.");
  }
};

// ============================================================================
// 逐拍 cycle 模型的参数表
//
// 上面那个 Params 是 Python 版模拟器移植过来的，服务的是旧模型，保留不动。
// 下面这一组是《latch 建模计划》定的新模型参数，出处逐条标在注释里：
//
//   MAS      原始设计文档给的
//   规格书   性能需求规格说明书给的
//   待定     设计未给值，建模计划填的默认值，向设计方要到值后只改这里
//
// 时间单位 1 T = 1 ns，带宽单位 B/T。GB/s 换算成 B/T 就是除以 1（1 GB/s ≈ 1 B/ns），
// 除不尽的向下取整，偏保守。
// ============================================================================

struct LinkParams {
  uint64_t bandwidth = 256;   // B/T
  uint64_t latency = 40;      // T
  uint64_t queue_depth = 64;  // 在途队列深度，按 latency 每拍 1 flit 的上限取
};

// chip 内相邻 core 的 Router 之间。规格书：256 B/T、40T
inline LinkParams LinkR2R() { return {256, 40, 64}; }

// chip 之间的 C2C，Router 到 Router。规格书：400T
inline LinkParams LinkC2C() { return {256, 400, 448}; }

// PCIe C2C：64 GB/s、300 ns
inline LinkParams LinkPcieC2C() { return {64, 300, 352}; }

// tray 间纵向链路：120 GB/s（试算值，待定）
inline LinkParams LinkTrayVertical() { return {120, 400, 448}; }

// PCIe 到 Router：128 B/T，左右 10T + 25T
inline LinkParams LinkPcieRouterLr() { return {128, 35, 64}; }

// PCIe 到 Router：上下 10T + 50T
inline LinkParams LinkPcieRouterUd() { return {128, 60, 96}; }

// ETH 注入：50 GB/s 每口，3 μs
inline LinkParams LinkEthIn() { return {50, 3000, 3072}; }

// PCIe 入口 x16 54.4 GB/s，向下取整
inline LinkParams LinkPcieIn() { return {54, 300, 352}; }

// PCIe 出口 x32 108.8 GB/s，向下取整
inline LinkParams LinkPcieOut() { return {108, 300, 352}; }

// 拍数换算。全模型统一走这一个函数：size 不大于 0 时仍算一拍，其余向上取整。
inline uint64_t CalcCycles(int64_t size, uint64_t bandwidth) {
  LOGCHECK(bandwidth > 0, "CalcCycles: bandwidth must be positive.");
  if (size <= 0) return 1;
  return (static_cast<uint64_t>(size) + bandwidth - 1) / bandwidth;
}

}
}

#endif
