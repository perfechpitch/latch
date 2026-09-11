#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_VU_TYPES_
#define _LATCH_BACH_IP_CHIP_CORE_VU_VU_TYPES_

// VU 的宏指令、静态配置与微指令。
//
// VU 与 VU-Core 之间的交互抽象是宏指令：一条宏指令一次配好整张计算图的通路。
// 一条由两半组成：8 组静态配置模板之一定各执行单元的连接与 op，12 个动态参数
// 寄存器定地址、索引、VL / 精度 / 舍入。写 macro_inst_trigger 就把当时的动态
// 参数锁成一份快照，与静态配置的指针打包压进 ISQ。
//
// 寄存器地址、位域与全部编码照《VU-DSA 寄存器整理》。五个 Block：动态参数
// 0x0000（12 个）、静态配置组 N×0x100 + 0x1000（8 组各 23 个）、DSA-RF 后门
// 0x2000（2 个）、状态 0x3000（3 个）、Profile 0x4000（59 个）。
//
// 静态配置组内前 12 个是没有动态副本的 *_op / mask_op / PRF_op，后 11 个是动态
// 参数寄存器的静态副本、与动态版本逐位相同，组内偏移 = 对应动态寄存器地址 +
// 0x2C。取哪一份由 trigger 的 STATIC_DYNAMIC_MASK 逐位决定。

#include <array>
#include <cstdint>
#include <vector>

#include "bach/common/numeric/mx.h"
#include "bach/common/numeric/round.h"

namespace latch {
namespace bach {

// ── 五个 Block 的基址 ──
constexpr uint64_t kVuDynamicBase = 0x0000;
constexpr uint64_t kVuStaticBase = 0x1000;
constexpr uint64_t kVuStaticStride = 0x100;
constexpr uint64_t kVuRegfileBase = 0x2000;
constexpr uint64_t kVuStatusBase = 0x3000;
constexpr uint64_t kVuProfileBase = 0x4000;
constexpr uint64_t kVuCfgGroups = 8;
// 静态副本的组内偏移 = 对应动态寄存器地址 + 0x2C。
constexpr uint64_t kVuStaticDupOffset = 0x2C;

// ── 动态参数区，12 个 ──
constexpr uint64_t kVuMacroInstTrigger = 0x00;
constexpr uint64_t kVuTypeVl = 0x04;
constexpr uint64_t kVuLdAddr = 0x08;
constexpr uint64_t kVuStAddr = 0x0C;
constexpr uint64_t kVuVrfRdIndex = 0x10;   // rd_p0 [15:0]、rd_p1 [31:16]
constexpr uint64_t kVuVrfWtIndex = 0x14;   // wt_p0 [15:0]、wt_p1 [31:16]
constexpr uint64_t kVuMrfRdIndex = 0x18;   // rd_p0 [15:0]、rd_p1 [31:16]
constexpr uint64_t kVuMrfWtIndex = 0x1C;   // wt [15:0]
constexpr uint64_t kVuSrfRdIndex0 = 0x20;  // rd_p0～p3，各 8 位
constexpr uint64_t kVuSrfRdIndex1 = 0x24;  // rd_p4～p7
constexpr uint64_t kVuSrfWtIndex0 = 0x28;  // wt_p0～p3
constexpr uint64_t kVuSrfWtIndex1 = 0x2C;  // wt_p4～p5，[31:16] 保留
constexpr uint64_t kVuDynamicEnd = 0x30;

// ── 静态配置组内的 12 个 op 寄存器 ──
constexpr uint64_t kVuLuOp = 0x00;
constexpr uint64_t kVuSuOp = 0x04;
constexpr uint64_t kVuValu0Op = 0x08;
constexpr uint64_t kVuValu1Op = 0x0C;
constexpr uint64_t kVuValu2Op = 0x10;
constexpr uint64_t kVuVsfuOp = 0x14;
constexpr uint64_t kVuMexeOp = 0x18;
constexpr uint64_t kVuSexe0Op = 0x1C;
constexpr uint64_t kVuSexe1Op = 0x20;
constexpr uint64_t kVuSexe2Op = 0x24;
constexpr uint64_t kVuMaskOp = 0x28;
constexpr uint64_t kVuPrfOp = 0x2C;

// ── DSA-RF 后门、状态与 Profile ──
constexpr uint64_t kVuRegFileAddr = kVuRegfileBase + 0x00;
constexpr uint64_t kVuRegFileData = kVuRegfileBase + 0x04;
constexpr uint64_t kVuMacroInstLeft = kVuStatusBase + 0x00;
constexpr uint64_t kVuStatus = kVuStatusBase + 0x04;
constexpr uint64_t kVuErrorCode = kVuStatusBase + 0x08;
constexpr uint64_t kVuProfileCtrl = kVuProfileBase + 0x00;
constexpr uint64_t kVuProfileCnt = kVuProfileBase + 0x08;

// reg_file_addr：RF_SEL 在 [17:16]，RF_ADDR 在 [15:0]，地址低 2 位被忽略。
constexpr uint64_t kVuRfSelShift = 16;
enum : uint64_t { kVuRfSelVrf = 0, kVuRfSelMrf = 1, kVuRfSelSrf = 2 };

// status 位域。
constexpr uint64_t kVuStatusBusy = 1u << 0;
constexpr uint64_t kVuStatusIsqFull = 1u << 1;
constexpr uint64_t kVuStatusIsqEmpty = 1u << 2;
constexpr uint64_t kVuStatusErrorFlag = 1u << 3;

// error_code 位域。没有 ILLEGAL_OPCODE 这一位：未分配与不支持的编码按无操作
// 处理，不置任何异常。
constexpr uint64_t kVuErrUnimplReg = 1u << 0;   // 访问了未实现的寄存器地址
constexpr uint64_t kVuErrCfg = 1u << 2;         // 静态配置非法
constexpr uint64_t kVuErrRfIndex = 1u << 3;     // RF 索引超出物理范围
constexpr uint64_t kVuErrCmAddr = 1u << 4;      // CM 地址未按访问格式对齐
constexpr uint64_t kVuErrCmResp = 1u << 5;      // CM 返回错误响应
constexpr uint64_t kVuErrDataCvt = 1u << 6;     // 高转低结果为 NaN

// profile_ctrl 位域。
constexpr uint64_t kVuProfileRun = 1u << 0;
constexpr uint64_t kVuProfileClear = 1u << 1;

// ── macro_inst_trigger 位域 ──
constexpr uint64_t kVuTrigMaskShift = 0;      // [7:0] STATIC_DYNAMIC_MASK
constexpr uint64_t kVuTrigCfgIdxShift = 8;    // [10:8] CONFIG_IDX
constexpr uint64_t kVuTrigEventEn = 1u << 16;
constexpr uint64_t kVuTrigSidOverride = 1u << 17;
constexpr uint64_t kVuTrigSidShift = 18;      // [21:18] STREAM_ID
constexpr uint64_t kVuTrigFence = 1u << 24;
constexpr uint64_t kVuTrigBroadcast = 1u << 25;

// STATIC_DYNAMIC_MASK 逐位控制哪个参数取动态版本。0 = 静态，1 = 动态。
// bit[5] 同时控制 MRF 读写两个寄存器，bit[6] 同时控制 SRF 四个。
enum : uint64_t {
  kVuMaskTypeVl = 1u << 0,
  kVuMaskLdAddr = 1u << 1,
  kVuMaskStAddr = 1u << 2,
  kVuMaskVrfRd = 1u << 3,
  kVuMaskVrfWt = 1u << 4,
  kVuMaskMrf = 1u << 5,
  kVuMaskSrf = 1u << 6,
};

// ── TYPE_VL 位域 ──
constexpr uint64_t kVuVlMask = 0xFFFF;        // [15:0]
constexpr uint64_t kVuDataTypeShift = 16;     // bit16：0 = FP32，1 = BF16
constexpr uint64_t kVuRoundModeShift = 17;    // [19:17]

// ── 尺寸 ──
constexpr uint64_t kVuVw = 128;               // 向量位宽，128 B/cycle
constexpr uint64_t kVuVlMax = 16384;
constexpr uint64_t kVuVrfEntry = 512;
constexpr uint64_t kVuVrfEntryBytes = 128;    // 1024 bit
constexpr uint64_t kVuVrfBytes = kVuVrfEntry * kVuVrfEntryBytes;  // 64 KB
constexpr uint64_t kVuMrfEntryBytes = 8;      // 64 bit：FP32 用低 32 位，BF16 用满
constexpr uint64_t kVuMrfBytes = 4096;
constexpr uint64_t kVuMrfEntry = kVuMrfBytes / kVuMrfEntryBytes;
constexpr uint64_t kVuSrfEntry = 64;
constexpr uint64_t kVuSrfBytes = kVuSrfEntry * 4;                 // 256 B
constexpr uint64_t kVuSrfRdPorts = 8;
constexpr uint64_t kVuSrfWtPorts = 6;
constexpr uint64_t kVuIsqDepth = 8;
constexpr uint64_t kVuOverlap = 2;            // 最多两条相邻宏指令重叠
constexpr uint64_t kVuCmLatency = 14;         // CM 访问延迟，VU 侧 14T
constexpr uint64_t kVuExeStages = 4;          // 各执行单元的级数，设计未给，取 4
constexpr uint64_t kVuTopK = 16;
constexpr uint64_t kVuLanes = 32;             // 一拍 32 个 FP32，归约的 LANES
constexpr uint64_t kVuCmAlign = 32;           // 向量与掩码按 32 B 对齐
constexpr uint64_t kVuScalarAlign = 4;        // 标量按 4 B 对齐

// ── 全局 src_sel 编码表，全部执行单元共用 ──
//
// 高 4 位是来源类别（0 执行单元 bypass、1 MEXE、2 SEXE 迭代、3 VRF、4 MRF、
// 5 SRF），低 4 位是类别内序号。
enum : uint64_t {
  kSrcNone = 0x00,
  kSrcLu = 0x01,
  kSrcValu0 = 0x02,
  kSrcValu1 = 0x03,
  kSrcValu2 = 0x04,
  kSrcVsfu = 0x05,
  kSrcMexe = 0x10,
  kSrcSexe0 = 0x20,
  kSrcSexe1 = 0x21,
  kSrcSexe2 = 0x22,
  kSrcVrfP0 = 0x30,
  kSrcVrfP1 = 0x31,
  kSrcMrfP0 = 0x40,
  kSrcMrfP1 = 0x41,
  kSrcSrfP0 = 0x50,   // 0x50～0x57，低 3 位即端口号
};

inline bool SrcIsVrf(uint64_t s) { return s == kSrcVrfP0 || s == kSrcVrfP1; }
inline bool SrcIsMrf(uint64_t s) { return s == kSrcMrfP0 || s == kSrcMrfP1; }
inline bool SrcIsSrf(uint64_t s) { return s >= 0x50 && s <= 0x57; }
inline uint64_t SrfPortOf(uint64_t s) { return s & 0x7u; }

// ── 各执行单元的 OPCODE ──
//
// 未分配的编码与本单元不支持的编码一律按无操作处理，与 0x00 等效，不置异常。
enum class LuOp : uint32_t {
  kNop = 0x00,
  kLdFp8e4m3 = 0x01,
  kLdMxfp8 = 0x02,
  kLdBf16 = 0x03,
  kLdFp32 = 0x04,
  kLdVmMask = 0x05,
  kLdSFp32 = 0x06,
};

enum class SuOp : uint32_t {
  kNop = 0x00,
  kStFp8e4m3 = 0x01,
  kStMxfp8 = 0x02,
  kStBf16 = 0x03,
  kStFp32 = 0x04,
  kStVmMask = 0x05,
  kStSFp32 = 0x06,
};

// SU_op 的 MXFP8_SCALE_ROUND 在 bit16：0 向下取整、1 向上取整。element 数值的
// 舍入另由 TYPE_VL.ROUND_MODE 决定，两个旋钮各管各的。
constexpr uint64_t kVuSuScaleRoundUp = 1u << 16;

enum class ValuOp : uint32_t {
  kNop = 0x00,
  kFaddVv = 0x01, kFaddVf = 0x02, kFsubVv = 0x03, kFsubVf = 0x04,
  kFrsubVf = 0x05, kFmulVv = 0x06, kFmulVf = 0x07, kFdivVv = 0x08,
  kFminVv = 0x10, kFminVf = 0x11, kFmaxVv = 0x12, kFmaxVf = 0x13,
  kMvVf = 0x20, kMvSf = 0x21, kMvFs = 0x22, kMvVv = 0x23,
  kMaccVv = 0x30, kMaccVf = 0x31, kNmaccVv = 0x32, kNmaccVf = 0x33,
  kMsacVv = 0x34, kMsacVf = 0x35, kNmsacVv = 0x36, kNmsacVf = 0x37,
  kSgnjVv = 0x40, kSgnjVf = 0x41, kSgnjnVv = 0x42, kSgnjnVf = 0x43,
  kSgnjxVv = 0x44, kSgnjxVf = 0x45,
  kEqVv = 0x50, kEqVf = 0x51, kNeVv = 0x52, kNeVf = 0x53,
  kLtVv = 0x54, kLtVf = 0x55, kLeVv = 0x56, kLeVf = 0x57,
  kGtVf = 0x58, kGeVf = 0x59,
  kClassMv = 0x60, kMergeVfm = 0x61, kMergeVvm = 0x62,
  kRedusum = 0x70, kRedmax = 0x71, kRedmin = 0x72,
  kSortmax16 = 0x73, kSortmin16 = 0x74,
};

enum class VsfuOp : uint32_t {
  kNop = 0x00,
  kSin = 0x01, kCos = 0x02, kTanh = 0x03, kSigmoid = 0x04,
  kExp = 0x05, kExp2 = 0x06, kLn = 0x07, kLog2 = 0x08,
  kSqrt = 0x09, kRcp = 0x0A, kRsqrt = 0x0B, kCustom = 0x0C,
};

enum class MexeOp : uint32_t {
  kNop = 0x00,
  kAnd = 0x01, kNand = 0x02, kAndn = 0x03, kXor = 0x04,
  kOr = 0x05, kNor = 0x06, kOrn = 0x07, kXnor = 0x08,
  kCpop = 0x10, kFirst = 0x11,
  kSbf = 0x12, kSif = 0x13, kSof = 0x14,
  kIuset = 0x15, kIset = 0x16,
};

enum class SexeOp : uint32_t {
  kNop = 0x00,
  kFadd = 0x01, kFsub = 0x02, kFmul = 0x03, kFdiv = 0x04,
  kFsqrt = 0x05, kFrsqrt = 0x06, kFrcp = 0x07,
};

// ── 执行单元 ──
enum class VuUnit : uint32_t {
  kLu = 0,
  kSu = 1,
  kValu0 = 2,
  kValu1 = 3,
  kValu2 = 4,
  kVsfu = 5,
  kMexe = 6,
  kSexe = 7,
};
constexpr uint64_t kVuUnitNum = 8;

// ── 一个 op 寄存器 ──
//
// 四个字节：OPCODE 与三路源选择。SU 的 [16] 是 MXFP8_SCALE_ROUND，VALU0 的
// 几条指令把 SRC2_SEL 或 SRC1_SEL 低位复用成立即数（ELEM_IMM / CLASS_IMM），
// 所以原始字保留在 raw 里。
struct VuOpReg {
  uint32_t opcode = 0;
  uint32_t src1 = 0, src2 = 0, src3 = 0;
  uint32_t raw = 0;

  void Set(uint64_t v) {
    raw = uint32_t(v);
    opcode = uint32_t(v & 0xFFu);
    src1 = uint32_t((v >> 8) & 0xFFu);
    src2 = uint32_t((v >> 16) & 0xFFu);
    src3 = uint32_t((v >> 24) & 0xFFu);
  }
  bool Active() const { return opcode != 0; }
};

// ── 动态参数的一份快照 ──
//
// 静态副本用的是同一个结构：组内偏移 = 动态地址 + 0x2C，逐位相同。
struct VuDynParam {
  uint32_t trigger = 0;
  uint32_t type_vl = 0;
  uint32_t ld_addr = 0;
  uint32_t st_addr = 0;
  uint32_t vrf_rd_index = 0;
  uint32_t vrf_wt_index = 0;
  uint32_t mrf_rd_index = 0;
  uint32_t mrf_wt_index = 0;
  uint32_t srf_rd_index_0 = 0;
  uint32_t srf_rd_index_1 = 0;
  uint32_t srf_wt_index_0 = 0;
  uint32_t srf_wt_index_1 = 0;

  uint64_t VrfRd(uint64_t port) const {
    return port == 0 ? (vrf_rd_index & 0xFFFFu) : (vrf_rd_index >> 16);
  }
  uint64_t VrfWt(uint64_t port) const {
    return port == 0 ? (vrf_wt_index & 0xFFFFu) : (vrf_wt_index >> 16);
  }
  uint64_t MrfRd(uint64_t port) const {
    return port == 0 ? (mrf_rd_index & 0xFFFFu) : (mrf_rd_index >> 16);
  }
  uint64_t MrfWt() const { return mrf_wt_index & 0xFFFFu; }
  // SRF 索引各 8 位，p0～p3 在第一个寄存器，p4～p7 在第二个。
  uint64_t SrfRd(uint64_t port) const {
    uint32_t w = port < 4 ? srf_rd_index_0 : srf_rd_index_1;
    return (w >> (8 * (port % 4))) & 0xFFu;
  }
  uint64_t SrfWt(uint64_t port) const {
    uint32_t w = port < 4 ? srf_wt_index_0 : srf_wt_index_1;
    return (w >> (8 * (port % 4))) & 0xFFu;
  }
};

// ── 一组静态配置模板：23 个寄存器 ──
struct VuStaticCfg {
  VuOpReg lu, su;
  std::array<VuOpReg, 3> valu;
  VuOpReg vsfu;
  VuOpReg mexe;
  std::array<VuOpReg, 3> sexe;
  uint32_t mask_op = 0;   // 每个 VEXE 2 位：00 不用、01 MRF_rd_p0、10 MRF_rd_p1
  uint32_t prf_op = 0;    // VRF_WT_P0/P1_SRC、MRF_WT_SRC、SRF_WT_EN
  VuDynParam dup;         // 11 个静态副本

  // mask_op：VALU0、VALU1、VALU2、VSFU 各占 2 位。
  uint64_t MaskSelOf(uint64_t vexe) const {
    return (mask_op >> (2 * vexe)) & 0x3u;
  }
  uint64_t VrfWtSrc(uint64_t port) const {
    return port == 0 ? (prf_op & 0xFFu) : ((prf_op >> 8) & 0xFFu);
  }
  uint64_t MrfWtSrc() const { return (prf_op >> 16) & 0xFFu; }
  uint64_t SrfWtEn() const { return (prf_op >> 24) & 0xFFu; }
};

// mask_op 的 2 位编码。
enum : uint64_t { kVuMaskNone = 0, kVuMaskP0 = 1, kVuMaskP1 = 2 };

// ── 一条宏指令 ──
//
// 参数分两份：动态快照与所属静态组的副本。取哪一份逐参数由 mask 决定，
// Param() 把这件事收在一处。
struct VuMacroInst {
  VuDynParam dyn;
  VuDynParam dup;         // 所属静态组的副本，压 ISQ 时一并锁下
  uint64_t cfg_idx = 0;
  uint64_t mask = 0;      // STATIC_DYNAMIC_MASK
  bool event_en = false;
  bool sid_override = false;
  bool fence = false;
  bool broadcast = false;
  uint64_t stream_id = 0;
  uint64_t task_id = 0;
  uint64_t seq = 0;

  uint64_t TypeVl() const {
    return (mask & kVuMaskTypeVl) ? dyn.type_vl : dup.type_vl;
  }
  uint64_t LdAddr() const {
    return (mask & kVuMaskLdAddr) ? dyn.ld_addr : dup.ld_addr;
  }
  uint64_t StAddr() const {
    return (mask & kVuMaskStAddr) ? dyn.st_addr : dup.st_addr;
  }
  uint64_t VrfRd(uint64_t port) const {
    return (mask & kVuMaskVrfRd) ? dyn.VrfRd(port) : dup.VrfRd(port);
  }
  uint64_t VrfWt(uint64_t port) const {
    return (mask & kVuMaskVrfWt) ? dyn.VrfWt(port) : dup.VrfWt(port);
  }
  uint64_t MrfRd(uint64_t port) const {
    return (mask & kVuMaskMrf) ? dyn.MrfRd(port) : dup.MrfRd(port);
  }
  uint64_t MrfWt() const {
    return (mask & kVuMaskMrf) ? dyn.MrfWt() : dup.MrfWt();
  }
  uint64_t SrfRd(uint64_t port) const {
    return (mask & kVuMaskSrf) ? dyn.SrfRd(port) : dup.SrfRd(port);
  }
  uint64_t SrfWt(uint64_t port) const {
    return (mask & kVuMaskSrf) ? dyn.SrfWt(port) : dup.SrfWt(port);
  }

  // VL 为 0 等效 1，大于 16384 等效 16384，不报错。
  uint64_t Vl() const {
    uint64_t v = TypeVl() & kVuVlMask;
    if (v == 0) return 1;
    return v > kVuVlMax ? kVuVlMax : v;
  }
  // bit16：0 = FP32，1 = BF16。只作用于向量通路，标量只有 FP32。
  bool Bf16() const { return ((TypeVl() >> kVuDataTypeShift) & 1u) != 0; }
  numeric::RoundMode Round() const {
    return numeric::RoundModeOf((TypeVl() >> kVuRoundModeShift) & 0x7u);
  }
  uint64_t ElemBytes() const { return Bf16() ? 2 : 4; }
  // 一条向量占几个 RF entry：FP32 每 entry 32 个 element，BF16 每 entry 64 个。
  uint64_t Entries() const {
    uint64_t per = Bf16() ? 64 : 32;
    return (Vl() + per - 1) / per;
  }
  // 一条向量走几拍：向量位宽固定 128 B。
  uint64_t Beats() const { return Entries(); }
};

// ── 一条宏指令展开成的微指令 ──
struct VuUops {
  VuStaticCfg cfg;
  VuMacroInst inst;
  // 各单元本条要不要动。未分配与不支持的 opcode 在这里已经归成不动。
  std::array<bool, kVuUnitNum> active{};
};

// ── 微指令在通路上的中间结果 ──
struct VuOperand {
  std::vector<float> vec;      // 向量通路，元素一律按 FP32 存
  std::vector<bool> mask;      // Mask 通路
  float scalar = 0.0f;         // 标量通路，只有 FP32
  std::vector<uint16_t> index; // Top-K 的 16 个 INT16 索引
  bool has_scalar = false;
};

}  // namespace bach
}  // namespace latch

#endif
