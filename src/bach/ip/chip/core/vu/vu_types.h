#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_VU_TYPES_
#define _LATCH_BACH_IP_CHIP_CORE_VU_VU_TYPES_

// VU 的宏指令、静态配置与微指令。
//
// VU 与 VU-Core 之间的交互抽象是宏指令：一条宏指令一次配好整张计算图的通路。
// 一条由两半组成：8 组静态配置模板之一定各执行单元的连接与 op，12 个动态参数
// 寄存器定地址、索引、VL / 精度 / 舍入。写 macro_inst_trigger 就把当时的动态
// 参数锁成一份快照，与静态配置的指针打包压进 ISQ。
//
// 寄存器地址、位域与全部编码照《VU-DSA 寄存器整理》。六个 Block：动态参数
// 0x0000（12 个）、静态配置组 N×0x100 + 0x1000（8 组各 23 个）、全局静态
// 0x1F00（2 个）、DSA-RF 后门 0x2000（2 个）、状态 0x3000（12 个）、
// Profile 0x4000（65 个）。
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

// ── 六个 Block 的基址 ──
constexpr uint64_t kVuDynamicBase = 0x0000;
constexpr uint64_t kVuStaticBase = 0x1000;
constexpr uint64_t kVuStaticStride = 0x100;
constexpr uint64_t kVuGlobalStaticBase = 0x1F00;
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

// ── 全局静态配置：不属于任何一组，所有宏指令共享 ──
constexpr uint64_t kVuInfReplaceValue = kVuGlobalStaticBase + 0x00;
constexpr uint64_t kVuNanReplaceValue = kVuGlobalStaticBase + 0x04;

// ── DSA-RF 后门 ──
constexpr uint64_t kVuRegFileAddr = kVuRegfileBase + 0x00;
constexpr uint64_t kVuRegFileData = kVuRegfileBase + 0x04;

// reg_file_addr：RF_SEL 在 [17:16]，RF_ADDR 在 [15:0]，地址低 2 位被忽略。
constexpr uint64_t kVuRfSelShift = 16;
enum : uint64_t { kVuRfSelVrf = 0, kVuRfSelMrf = 1, kVuRfSelSrf = 2 };

// ── 状态区，12 个 ──
constexpr uint64_t kVuMacroInstLeft = kVuStatusBase + 0x00;
constexpr uint64_t kVuStatus = kVuStatusBase + 0x04;
constexpr uint64_t kVuErrorCode = kVuStatusBase + 0x08;
constexpr uint64_t kVuErrorInfo = kVuStatusBase + 0x0C;
constexpr uint64_t kVuSnapshotAddr = kVuStatusBase + 0x10;
constexpr uint64_t kVuSnapshotData = kVuStatusBase + 0x14;
constexpr uint64_t kVuVrfErrInfo = kVuStatusBase + 0x18;
constexpr uint64_t kVuMrfErrInfo = kVuStatusBase + 0x1C;
constexpr uint64_t kVuSrfErrInfo = kVuStatusBase + 0x20;
constexpr uint64_t kVuCmErrInfo = kVuStatusBase + 0x24;
constexpr uint64_t kVuCmErrAddr = kVuStatusBase + 0x28;
constexpr uint64_t kVuNanErrInfo = kVuStatusBase + 0x2C;
constexpr uint64_t kVuStatusEnd = kVuStatusBase + 0x30;

// ── Profile ──
//
// 控制寄存器 profile_ctrl 在 Block 最前（0x4000），0x4004 是未实现地址；其后是
// 32 个 64-bit 计数器，每个拆成 _lo 与 _hi 两个 32 位寄存器（0x4008～0x4104）。
constexpr uint64_t kVuProfileCtrl = kVuProfileBase + 0x00;
constexpr uint64_t kVuProfileCnt = kVuProfileBase + 0x08;
constexpr uint64_t kVuCounterNum = 32;

// status 位域。
constexpr uint64_t kVuStatusBusy = 1u << 0;
constexpr uint64_t kVuStatusIsqFull = 1u << 1;
constexpr uint64_t kVuStatusIsqEmpty = 1u << 2;
constexpr uint64_t kVuStatusErrorFlag = 1u << 3;

// error_code 位域。没有 ILLEGAL_OPCODE 这一位：`*_op.OPCODE` 的未分配编码按无操作
// 处理，不置任何异常。
constexpr uint64_t kVuErrRegAddr = 1u << 0;   // 配置总线访问了未实现的寄存器地址
constexpr uint64_t kVuErrCfg = 1u << 1;       // 非法配置，调度阶段拦截、本条不执行
constexpr uint64_t kVuErrRfIndex = 1u << 2;   // RF 访问跨越物理上界，回绕且数据不可信
constexpr uint64_t kVuErrCmAddr = 1u << 3;    // CM 地址未按访问格式对齐或超出实现范围
constexpr uint64_t kVuErrNan = 1u << 4;       // 归约输出 / SU 输入阶段出现 NaN
constexpr uint64_t kVuErrVrfEcc = 1u << 5;    // VRF 2-bit 不可纠 ECC
constexpr uint64_t kVuErrMrfEcc = 1u << 6;    // MRF 2-bit 不可纠 ECC
constexpr uint64_t kVuErrSrfEcc = 1u << 7;    // SRF 2-bit 不可纠 ECC
constexpr uint64_t kVuErrCmEcc = 1u << 8;     // CM 2-bit 不可纠 ECC

// error_info 的 ERR_UNIT 编码：上报该异常的单元。
enum : uint64_t {
  kVuErrUnitNone = 0x0,    // 配置总线，或派发前的静态配置合法性检查
  kVuErrUnitLu = 0x1,
  kVuErrUnitSu = 0x2,
  kVuErrUnitValu0 = 0x3,
  kVuErrUnitValu1 = 0x4,
  kVuErrUnitValu2 = 0x5,
  kVuErrUnitVsfu0 = 0x6,
  kVuErrUnitVsfu1 = 0x7,
  kVuErrUnitMexe = 0x8,
  kVuErrUnitSexe0 = 0x9,
  kVuErrUnitSexe1 = 0xA,
  kVuErrUnitSexe2 = 0xB,
  kVuErrUnitRfDebug = 0xC,  // DSA-RF 调试通路
  kVuErrUnitVrf = 0xD,
  kVuErrUnitMrf = 0xE,
  kVuErrUnitSrf = 0xF,
};

// error_info 位域：首个置位异常的宏指令配置上下文，sticky 锁存。
constexpr uint64_t kVuErrInfoUserShift = 0;     // [15:0]
constexpr uint64_t kVuErrInfoStreamShift = 16;  // [19:16]
constexpr uint64_t kVuErrInfoCfgIdxShift = 20;  // [22:20]
constexpr uint64_t kVuErrInfoUnitShift = 23;    // [26:23]
constexpr uint64_t kVuErrInfoFirstShift = 27;   // [30:27]
constexpr uint64_t kVuErrInfoValid = 1u << 31;

// snapshot_addr 位域：SNAP_IDX 选条目、SNAP_SEL 按年龄选宏指令（0x00 最老，
// 0xFF 选 sticky）。
constexpr uint64_t kVuSnapIdxShift = 0;   // [3:0]
constexpr uint64_t kVuSnapSelShift = 4;   // [11:4]
constexpr uint64_t kVuSnapSelSticky = 0xFF;
// SNAP_IDX 取 0x1～0xC 时对应动态参数寄存器地址 ÷ 4 + 1；0x0 是快照状态字。
constexpr uint64_t kVuSnapStatusWord = 0x0;

// 快照状态字的位域。
constexpr uint64_t kVuSnapValid = 1u << 0;
constexpr uint64_t kVuSnapDispatched = 1u << 1;
constexpr uint64_t kVuSnapTagShift = 8;   // [15:8]

// 错误上下文寄存器的公共位域：USER_ID [15:0]、RF_ERR_IDX [24:16]、VALID [31]。
constexpr uint64_t kVuErrCtxIdxShift = 16;
constexpr uint64_t kVuErrCtxValid = 1u << 31;
// cm_err_info 的 DIR 在 bit16：0 读、1 写。
constexpr uint64_t kVuCmErrInfoDir = 1u << 16;

// profile_ctrl 位域。
constexpr uint64_t kVuProfileRun = 1u << 0;
constexpr uint64_t kVuProfileClear = 1u << 1;

// ── macro_inst_trigger 位域 ──
constexpr uint64_t kVuTrigMaskShift = 0;      // [7:0] STATIC_DYNAMIC_MASK
constexpr uint64_t kVuTrigCfgIdxShift = 8;    // [10:8] CONFIG_IDX
constexpr uint64_t kVuTrigEventEn = 1u << 16;
constexpr uint64_t kVuTrigSidOverride = 1u << 17;
constexpr uint64_t kVuTrigSidShift = 18;      // [21:18] STREAM_ID
constexpr uint64_t kVuTrigFence = 1u << 24;   // MACRO_INST_FENCE
constexpr uint64_t kVuTrigCmFence = 1u << 25; // CM_FENCE

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
constexpr uint64_t kVuNanInfReplaceEn = 1u << 20;  // bit20

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

// 索引字段的有效位：高位的由硬件截断丢弃，起始索引本身因此不会越界。
constexpr uint64_t kVuRfIdxMask = 0x1FF;      // VRF / MRF 各 512 entry
constexpr uint64_t kVuSrfIdxMask = 0x3F;      // SRF 64 entry

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
  kSrcVsfu0 = 0x05,
  kSrcVsfu1 = 0x06,   // 仅 DATA_TYPE=FP32；BF16 下两个 VSFU 拼接，本编码非法
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
  kLdMask = 0x05,
  kLdSFp32 = 0x06,
};

enum class SuOp : uint32_t {
  kNop = 0x00,
  kStFp8e4m3 = 0x01,
  kStMxfp8 = 0x02,
  kStBf16 = 0x03,
  kStFp32 = 0x04,
  kStMask = 0x05,
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
  kSwap2 = 0x24, kSlide1Up = 0x25, kSlide1Down = 0x26,
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

// 归约与 Top-K 看整条，不分段；它们的结果还要在替换模式下过一遍 NaN / Inf 替换。
inline bool ValuIsReduce(ValuOp op) {
  return op >= ValuOp::kRedusum && op <= ValuOp::kRedmin;
}

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

// ── 每个单元支持哪些编码、每条指令用哪几路源 ──
//
// 「源用哪几路」是一位一路：bit0 src1、bit1 src2、bit2 src3。指令未使用的源选择
// 字段一律被忽略，不检查编码、不占端口、置任何值都不置 CFG_ERROR。
//
// 未分配的编码与本单元不支持的编码按无操作处理，该单元的全部字段一并被忽略。

inline bool ValuSupports(uint64_t which, ValuOp op) {
  switch (op) {
    case ValuOp::kFaddVv: case ValuOp::kFaddVf:
    case ValuOp::kFsubVv: case ValuOp::kFsubVf: case ValuOp::kFrsubVf:
    case ValuOp::kFmulVv: case ValuOp::kFmulVf:
    case ValuOp::kFminVv: case ValuOp::kFminVf:
    case ValuOp::kFmaxVv: case ValuOp::kFmaxVf:
    case ValuOp::kMvVf:
      return true;
    case ValuOp::kFdivVv:
    case ValuOp::kMvSf:
    case ValuOp::kMaccVv: case ValuOp::kMaccVf:
    case ValuOp::kNmaccVv: case ValuOp::kNmaccVf:
    case ValuOp::kMsacVv: case ValuOp::kMsacVf:
    case ValuOp::kNmsacVv: case ValuOp::kNmsacVf:
    case ValuOp::kSgnjVv: case ValuOp::kSgnjVf:
    case ValuOp::kSgnjnVv: case ValuOp::kSgnjnVf:
    case ValuOp::kSgnjxVv: case ValuOp::kSgnjxVf:
    case ValuOp::kEqVv: case ValuOp::kEqVf:
    case ValuOp::kNeVv: case ValuOp::kNeVf:
    case ValuOp::kLtVv: case ValuOp::kLtVf:
    case ValuOp::kLeVv: case ValuOp::kLeVf:
    case ValuOp::kGtVf: case ValuOp::kGeVf:
    case ValuOp::kClassMv: case ValuOp::kMergeVfm: case ValuOp::kMergeVvm:
      return which == 0;
    case ValuOp::kMvFs:
    case ValuOp::kRedusum: case ValuOp::kRedmax: case ValuOp::kRedmin:
    case ValuOp::kSortmax16: case ValuOp::kSortmin16:
      return which == 1;
    case ValuOp::kMvVv: case ValuOp::kSwap2:
    case ValuOp::kSlide1Up: case ValuOp::kSlide1Down:
      return which == 2;
    default:
      return false;
  }
}

inline uint64_t ValuSrcUsed(ValuOp op) {
  switch (op) {
    // 单源指令：向量源挂 src1，另一个字段被忽略或复用成立即数。
    case ValuOp::kMvVf: case ValuOp::kMvSf: case ValuOp::kMvFs:
    case ValuOp::kMvVv: case ValuOp::kSwap2: case ValuOp::kClassMv:
    case ValuOp::kSortmax16: case ValuOp::kSortmin16:
      return 0b001;
    // 两条 slide 的填充标量挂 src1、被搬的向量挂 src2。
    case ValuOp::kSlide1Up: case ValuOp::kSlide1Down:
      return 0b011;
    default:
      return op >= ValuOp::kMaccVv && op <= ValuOp::kNmsacVf ? 0b111 : 0b011;
  }
}

inline bool VsfuSupports(uint32_t opcode) { return opcode <= 0x0Cu; }

inline bool MexeSupports(uint32_t opcode) {
  return (opcode >= 0x01u && opcode <= 0x08u) ||
         (opcode >= 0x10u && opcode <= 0x16u);
}

inline uint64_t MexeSrcUsed(uint32_t opcode) {
  if (opcode >= 0x01u && opcode <= 0x08u) return 0b011;   // 双掩码
  if (opcode == 0x15u || opcode == 0x16u) return 0b011;   // 掩码 + 索引向量
  if (opcode >= 0x10u && opcode <= 0x14u) return 0b001;   // 单掩码
  return 0;
}

inline bool SexeSupports(uint32_t opcode) { return opcode >= 0x01u && opcode <= 0x07u; }

// 双操作数的那四条才用得到 src2，其余三条是单操作数。
inline bool SexeUsesTwoSrc(uint32_t opcode) {
  return opcode >= 0x01u && opcode <= 0x04u;
}

// 这个 VALU 的编码是不是「不支持掩码」的那一类：对应 MASK_SEL 被忽略。
inline bool ValuNoMask(ValuOp op) {
  return op >= ValuOp::kMvVf && op <= ValuOp::kSlide1Down;
}

// ── 执行单元 ──
//
// VSFU 有功能相同的两个：FP32 下两者独立工作，BF16 下拼接成一个逻辑单元。
enum class VuUnit : uint32_t {
  kLu = 0,
  kSu = 1,
  kValu0 = 2,
  kValu1 = 3,
  kValu2 = 4,
  kVsfu0 = 5,
  kVsfu1 = 6,
  kMexe = 7,
  kSexe = 8,
};
constexpr uint64_t kVuUnitNum = 9;

// ── 一个 op 寄存器 ──
//
// 四个字节：OPCODE 与三路源选择。SU 的 [16] 是 MXFP8_SCALE_ROUND，VALU0 的
// 几条指令把 SRC2_SEL 或 SRC1_SEL 低位复用成立即数（ELEM_IMM / CLASS_IMM），
// VSFU 的四个字节是 VSFU0 与 VSFU1 各一对，所以原始字保留在 raw 里。
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

  // 索引字段的高位由硬件截断丢弃，起始索引因此恒落在 RF 容量内；越界回绕是
  // 起始索引 + 占用 entry 数 > 上界才发生的事，在 pipe_ctrl 那一级查。
  uint64_t VrfRd(uint64_t port) const {
    uint64_t w = port == 0 ? (vrf_rd_index & 0xFFFFu) : (vrf_rd_index >> 16);
    return w & kVuRfIdxMask;
  }
  uint64_t VrfWt(uint64_t port) const {
    uint64_t w = port == 0 ? (vrf_wt_index & 0xFFFFu) : (vrf_wt_index >> 16);
    return w & kVuRfIdxMask;
  }
  uint64_t MrfRd(uint64_t port) const {
    uint64_t w = port == 0 ? (mrf_rd_index & 0xFFFFu) : (mrf_rd_index >> 16);
    return w & kVuRfIdxMask;
  }
  uint64_t MrfWt() const { return (mrf_wt_index & 0xFFFFu) & kVuRfIdxMask; }
  // SRF 索引各 8 位，p0～p3 在第一个寄存器，p4～p7 在第二个；有效位只有低 6 位。
  uint64_t SrfRd(uint64_t port) const {
    uint32_t w = port < 4 ? srf_rd_index_0 : srf_rd_index_1;
    return ((w >> (8 * (port % 4))) & 0xFFu) & kVuSrfIdxMask;
  }
  uint64_t SrfWt(uint64_t port) const {
    uint32_t w = port < 4 ? srf_wt_index_0 : srf_wt_index_1;
    return ((w >> (8 * (port % 4))) & 0xFFu) & kVuSrfIdxMask;
  }
};

// ── 一组静态配置模板：23 个寄存器 ──
struct VuStaticCfg {
  VuOpReg lu, su;
  std::array<VuOpReg, 3> valu;
  VuOpReg vsfu;           // 低 16 位配 VSFU0、高 16 位配 VSFU1
  VuOpReg mexe;
  std::array<VuOpReg, 3> sexe;
  // mask_op：VALU0 / VALU1 / VALU2 各占一个字节，取值是 src_sel 编码。
  uint32_t mask_op = 0;
  uint32_t prf_op = 0;    // VRF_WT_P0/P1_SRC、MRF_WT_SRC、SRF_WT_EN
  VuDynParam dup;         // 11 个静态副本

  // mask_op：0x00 不用掩码、0x01 LU 的 ld.mask bypass、0x40/0x41 MRF 读端口。
  // VSFU 没有掩码字段。
  uint64_t MaskSelOf(uint64_t which) const {
    return (mask_op >> (8 * which)) & 0xFFu;
  }
  // VSFU_op 的两个半字：which = 0 取 VSFU0、1 取 VSFU1。
  uint64_t VsfuOpcode(uint64_t which) const {
    return (vsfu.raw >> (16 * which)) & 0xFFu;
  }
  uint64_t VsfuSrc(uint64_t which) const {
    return ((vsfu.raw >> (16 * which)) >> 8) & 0xFFu;
  }
  bool VsfuActive(uint64_t which) const {
    return VsfuOpcode(which) != 0;
  }
  uint64_t VrfWtSrc(uint64_t port) const {
    return port == 0 ? (prf_op & 0xFFu) : ((prf_op >> 8) & 0xFFu);
  }
  uint64_t MrfWtSrc() const { return (prf_op >> 16) & 0xFFu; }
  uint64_t SrfWtEn() const { return (prf_op >> 24) & 0xFFu; }
};

// mask_op 的合法编码，取值是 src_sel。
constexpr uint64_t kVuMaskSelNone = 0x00;
constexpr uint64_t kVuMaskSelLu = 0x01;
constexpr uint64_t kVuMaskSelMrfP0 = 0x40;
constexpr uint64_t kVuMaskSelMrfP1 = 0x41;

// 各单元在这条宏指令里动不动：编码非 0、且本单元支持这个编码。
inline bool VuLuOn(VuStaticCfg const& c) {
  return c.lu.opcode >= 0x01u && c.lu.opcode <= 0x06u;
}
inline bool VuSuOn(VuStaticCfg const& c) {
  return c.su.opcode >= 0x01u && c.su.opcode <= 0x06u;
}
inline bool VuValuOn(VuStaticCfg const& c, uint64_t which) {
  return c.valu[which].Active() &&
         ValuSupports(which, ValuOp(c.valu[which].opcode));
}
inline bool VuVsfuOn(VuStaticCfg const& c, uint64_t which) {
  return c.VsfuActive(which) && VsfuSupports(uint32_t(c.VsfuOpcode(which)));
}

// 这一条有没有哪一路真的被用到的源指向这个编码。指令未使用的源选择字段一律被
// 忽略：既不检查编码，也不占 RF 端口。
inline bool VuUsesSrc(VuStaticCfg const& c, uint64_t sel) {
  for (uint64_t i = 0; i < 3; ++i) {
    if (!VuValuOn(c, i)) continue;
    uint64_t used = ValuSrcUsed(ValuOp(c.valu[i].opcode));
    uint64_t const f[3] = {c.valu[i].src1, c.valu[i].src2, c.valu[i].src3};
    for (uint64_t b = 0; b < 3; ++b) {
      if ((used & (1u << b)) && f[b] == sel) return true;
    }
  }
  for (uint64_t which = 0; which < 2; ++which) {
    if (VuVsfuOn(c, which) && c.VsfuSrc(which) == sel) return true;
  }
  if (MexeSupports(c.mexe.opcode)) {
    uint64_t used = MexeSrcUsed(c.mexe.opcode);
    uint64_t const f[2] = {c.mexe.src1, c.mexe.src2};
    for (uint64_t b = 0; b < 2; ++b) {
      if ((used & (1u << b)) && f[b] == sel) return true;
    }
  }
  for (uint64_t k = 0; k < 3; ++k) {
    if (!SexeSupports(c.sexe[k].opcode)) continue;
    if (c.sexe[k].src1 == sel) return true;
    if (SexeUsesTwoSrc(c.sexe[k].opcode) && c.sexe[k].src2 == sel) return true;
  }
  return VuSuOn(c) && c.su.src1 == sel;
}

// MRF 读口的占用方有三类：掩码（mask_op 里对应的字段）、MEXE 的两个操作数字段、
// 以及 st.mask 的数据源。掩码数据不能广播，一个读端口只服务一个消费者。
inline bool VuUsesMrfPort(VuStaticCfg const& c, uint64_t port) {
  uint64_t want = port == 0 ? kVuMaskSelMrfP0 : kVuMaskSelMrfP1;
  for (uint64_t i = 0; i < 3; ++i) {
    if (!VuValuOn(c, i)) continue;
    if (ValuNoMask(ValuOp(c.valu[i].opcode))) continue;
    if (c.MaskSelOf(i) == want) return true;
  }
  if (MexeSupports(c.mexe.opcode)) {
    uint64_t used = MexeSrcUsed(c.mexe.opcode);
    if ((used & 0b01) && c.mexe.src1 == want) return true;
    if ((used & 0b10) && c.mexe.src2 == want) return true;
  }
  return VuSuOn(c) && uint32_t(SuOp(c.su.opcode)) == 0x05u &&
         c.su.src1 == want;
}

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
  bool fence = false;     // MACRO_INST_FENCE：等此前全部宏指令完成才派发
  bool cm_fence = false;  // CM_FENCE：等前序宏指令的 CM 访问完成才派发
  uint64_t stream_id = 0;
  uint64_t task_id = 0;
  // 用户号与 stream/task 一样，写 trigger 那一拍从 VU-Core 的身份直连线上采。
  uint64_t user_id = 0;
  uint64_t seq = 0;
  // 入队时按顺序分配的标签，随这条指令携带到退休，超过 0xFF 回绕。快照窗口
  // 用它辨别两次读取是不是同一条宏指令。
  uint64_t tag = 0;

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
  // bit20：0 = NaN 上报（归约输出与 SU 输入阶段）、Inf 原样透传；1 = 按
  // INF_REPLACE_VALUE / NAN_REPLACE_VALUE 替换。
  bool NanInfReplaceEn() const {
    return (TypeVl() & kVuNanInfReplaceEn) != 0;
  }
  uint64_t ElemBytes() const { return Bf16() ? 2 : 4; }
  // 一条向量占几个 RF entry：FP32 每 entry 32 个 element，BF16 每 entry 64 个。
  uint64_t Entries() const {
    uint64_t per = Bf16() ? 64 : 32;
    return (Vl() + per - 1) / per;
  }
  // 一条向量走几拍：向量位宽固定 128 B。
  uint64_t Beats() const { return Entries(); }

  // 快照窗口按 SNAP_IDX 取一个动态参数寄存器：0x1 是 macro_inst_trigger、
  // 0x2～0xC 依次是 0x0004～0x002C。给出的是硬件实际使用的那一份——Mask 对应位
  // 为 0 的参数取的是静态副本的值。
  uint64_t SnapParam(uint64_t idx) const {
    switch (idx) {
      case 0x1: return dyn.trigger;
      case 0x2: return TypeVl();
      case 0x3: return LdAddr();
      case 0x4: return StAddr();
      case 0x5: return (mask & kVuMaskVrfRd) ? dyn.vrf_rd_index : dup.vrf_rd_index;
      case 0x6: return (mask & kVuMaskVrfWt) ? dyn.vrf_wt_index : dup.vrf_wt_index;
      case 0x7: return (mask & kVuMaskMrf) ? dyn.mrf_rd_index : dup.mrf_rd_index;
      case 0x8: return (mask & kVuMaskMrf) ? dyn.mrf_wt_index : dup.mrf_wt_index;
      case 0x9:
        return (mask & kVuMaskSrf) ? dyn.srf_rd_index_0 : dup.srf_rd_index_0;
      case 0xA:
        return (mask & kVuMaskSrf) ? dyn.srf_rd_index_1 : dup.srf_rd_index_1;
      case 0xB:
        return (mask & kVuMaskSrf) ? dyn.srf_wt_index_0 : dup.srf_wt_index_0;
      case 0xC:
        return (mask & kVuMaskSrf) ? dyn.srf_wt_index_1 : dup.srf_wt_index_1;
      default: return 0;
    }
  }
};

// ── 一条宏指令展开成的微指令 ──
//
// units 是这一条要用哪几个执行单元，一位一个（位号见 VuUnit）；展开时未分配与
// 本单元不支持的 opcode 已经归成不动。两个全局静态替换值在展开这一拍取一份
// 随微指令往下走：它们是上电写一次就不再改的常量。
struct VuUops {
  VuStaticCfg cfg;
  VuMacroInst inst;
  uint64_t units = 0;
  uint32_t inf_replace = 0;   // 0x1F00
  uint32_t nan_replace = 0;   // 0x1F04
};

// 替换值按当前精度取：FP32 用全 32 位，BF16 取低 16 位当 BF16 位型、直接截断。
inline float VuReplaceValue(uint32_t raw, bool bf16) {
  if (!bf16) return numeric::FloatOf(raw);
  return numeric::FromBf16(uint16_t(raw & 0xFFFFu));
}

// 逐元素算、且元素 i 的结果只看第 i 个输入的 VALU 编码才能拆段。
//
// 归约（0x70～0x72）与 Top-16（0x73/0x74）看整条；两条 slide（0x25/0x26）在段
// 边界上要取相邻段的元素；`vfmv.s.f` / `vfmv.f.s` 只碰第一个 entry 内的那个
// element（ELEM_IMM 高位被截断），拆了就算错。
inline bool ValuSegmentable(uint64_t which, ValuOp op) {
  if (!ValuSupports(which, op)) return true;   // 无操作，拆不拆都一样
  switch (op) {
    case ValuOp::kFaddVv: case ValuOp::kFaddVf:
    case ValuOp::kFsubVv: case ValuOp::kFsubVf: case ValuOp::kFrsubVf:
    case ValuOp::kFmulVv: case ValuOp::kFmulVf: case ValuOp::kFdivVv:
    case ValuOp::kFminVv: case ValuOp::kFminVf:
    case ValuOp::kFmaxVv: case ValuOp::kFmaxVf:
    case ValuOp::kMvVf: case ValuOp::kMvVv: case ValuOp::kSwap2:
    case ValuOp::kMaccVv: case ValuOp::kMaccVf:
    case ValuOp::kNmaccVv: case ValuOp::kNmaccVf:
    case ValuOp::kMsacVv: case ValuOp::kMsacVf:
    case ValuOp::kNmsacVv: case ValuOp::kNmsacVf:
    case ValuOp::kSgnjVv: case ValuOp::kSgnjVf:
    case ValuOp::kSgnjnVv: case ValuOp::kSgnjnVf:
    case ValuOp::kSgnjxVv: case ValuOp::kSgnjxVf:
    case ValuOp::kEqVv: case ValuOp::kEqVf:
    case ValuOp::kNeVv: case ValuOp::kNeVf:
    case ValuOp::kLtVv: case ValuOp::kLtVf:
    case ValuOp::kLeVv: case ValuOp::kLeVf:
    case ValuOp::kGtVf: case ValuOp::kGeVf:
    case ValuOp::kClassMv: case ValuOp::kMergeVfm: case ValuOp::kMergeVvm:
      return true;
    default:
      return false;
  }
}

// 这一条宏指令能不能拆成段逐段流过。
//
// 拆的收益在 Load 与 Store 重叠上，所以两头都得是向量、且本条用到的每个单元都
// 逐元素算。拆不了的整条走一份。
inline bool VuCanSegment(VuUops const& u) {
  LuOp lu = LuOp(u.cfg.lu.opcode);
  SuOp su = SuOp(u.cfg.su.opcode);
  if (!u.cfg.lu.Active() || !u.cfg.su.Active()) return false;
  if (lu == LuOp::kLdMask || lu == LuOp::kLdSFp32) return false;
  if (su == SuOp::kStMask || su == SuOp::kStSFp32) return false;
  // MXFP8 的块 scale 按整条定阶，拆段之后每段自己定阶就不是同一个数了。
  if (lu == LuOp::kLdMxfp8 || su == SuOp::kStMxfp8) return false;
  if (u.cfg.mexe.Active()) return false;
  for (VuOpReg const& r : u.cfg.sexe) {
    if (r.Active()) return false;
  }
  for (uint64_t i = 0; i < 3; ++i) {
    VuOpReg const& r = u.cfg.valu[i];
    if (r.Active() && !ValuSegmentable(i, ValuOp(r.opcode))) return false;
  }
  // 写 SRF 的那几路也是一条宏指令一次的事。
  if (u.cfg.SrfWtEn() != 0) return false;
  return u.inst.Entries() > 1;
}

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
