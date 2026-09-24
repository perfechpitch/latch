/* VU RV core 的 kernel。向量计算那一档。
 *
 * 寄存器名与位域照《VU-DSA 寄存器整理》。一条宏指令写 macro_inst_trigger
 * 就发射：CONFIG_IDX 选第几组静态配置，STATIC_DYNAMIC_MASK 每一位决定对应
 * 执行单元取静态配置还是取动态参数。
 *
 * VU-DSA 不追踪 Core Mem 访存的数据冲突，有冲突的宏指令之间要由软件置
 * MACRO_INST_FENCE，让这一条等此前全部宏指令做完才派发。
 */

#include "bach.h"

#define TASK __attribute__((section(".text.task"), noinline, used))

/* 地址随 stream 变，走动态副本；其余参数取静态组里的那一份 */
#define ADDR_DYNAMIC (VU_MASK_LD_ADDR | VU_MASK_ST_ADDR)

static void vu_launch(u32 config_idx, u32 ld, u32 st, u32 fence) {
  u32 sid = stream_id();
  dsa_write(VU_LD_ADDR, ld);
  dsa_write(VU_ST_ADDR, st);
  dsa_write(VU_MACRO_INST_TRIGGER,
            (ADDR_DYNAMIC << VU_STATIC_DYNAMIC_MASK_SHIFT)
                | (config_idx << VU_CONFIG_IDX_SHIFT)
                | VU_STREAM_ID_OVERRIDE | (sid << VU_STREAM_ID_SHIFT)
                | VU_EVENT_EN
                | (fence ? VU_MACRO_INST_FENCE : 0u));
}

/* 交还之前等本 task 最后那条 trigger 被 VU 的配置通路收下。
 *
 * VU 在收下 trigger 的那一拍才采 stream_id / task_id，而 RV 发出 dsaw 不等它被
 * 收下：ISQ 满时 trigger 压在通路上，这时交还，下一笔 task 一起来，身份就换成
 * 了下一笔的，dsa_done 报给了别人。DSA 读写同走一条通路、按序收，所以读一次
 * 并用掉读回的值（mv zero 读这个寄存器，值没回来就停在这条上），就说明前面的
 * trigger 都已收下。 */
static inline __attribute__((always_inline)) void vu_wait_trigger_taken(void) {
  u32 v = dsa_read(VU_MACRO_INST_LEFT);
  __asm__ volatile("mv zero, %0" : : "r"(v));
}

/* 单 core 用例的 VU 那一步：MU 算出来的 BF16 逐元素算一遍，仍按 BF16 写回。
 * 算什么由第 0 组静态配置定。置 EVENT_EN，VU 退休才把 dsa_done 打给 TS。 */
TASK void task_vu_compute(void) {
  u32 base = CMEM_STREAM_BASE + stream_id() * CMEM_STREAM_STRIDE;
  vu_launch(0, base + CMEM_FC1_OFF, base + CMEM_ACT_OFF, 0);
  vu_wait_trigger_taken();
  task_done(1);
}

/* ===== dot core：silu·dot·量化 =====
 *
 * 三条宏指令，经 VRF 中转：VU 的执行链是 VALU 排在 VSFU 前面，一条里做不完
 * 「先取 sigmoid 再乘回去」；一条宏指令也只有一路 LU，fc1 与 fc3 两个向量进
 * 不来。
 *
 *   组 1  LU 读 fc1（BF16）→ VSFU 出 sigmoid → 写 VRF 第 0 项
 *   组 2  LU 读 fc1（BF16）→ VALU0 乘 VRF 第 0 项 → 写 VRF 第 8 项，这就是 silu
 *   组 3  LU 读 fc3（BF16）→ VALU0 乘 VRF 第 8 项 → SU 量化成 MXFP8 写回
 *
 * 组 0 留给 task_vu_compute。
 *
 * 读的是 chip 内归约出来的两份部分和，按 FP32 算，写回的是 FC2 输入，scale 随
 * 数据写进 scale 旁带。第 8 项不是第 1 项：一条 VL=256 的 FP32 向量占 8 个 entry，
 * VRF 索引是 entry 号。三条之间是 VRF 上的 RAW / WAW，记分板会串起来，
 * 不必再置 MACRO_INST_FENCE。 */

#define VRF_SIG  0u
#define VRF_GATE 8u

static void vu_static(u32 group, u32 off, u32 data) {
  dsa_write(vu_static_group(group) + off, data);
}

static u32 op_word(u32 opcode, u32 src1, u32 src2) {
  return opcode | (src1 << VU_SRC1_SHIFT) | (src2 << VU_SRC2_SHIFT);
}

/* 三组静态配置，几个专家共用：地址每条走动态副本。firmware 不跑，每次
 * task_vu_gate 开头写一遍。 */
static void gate_setup(void) {
  u32 type_vl = MOE_SEG_INTER;   /* FP32、RNE 都是 0 */

  vu_static(1, VU_LU_OP, op_word(VU_LU_LD_BF16, 0, 0));
  vu_static(1, VU_VSFU_OP, op_word(VU_VSFU_SIGMOID, VU_SRC_LU, 0));
  vu_static(1, VU_SU_OP, op_word(VU_SU_NOP, 0, 0));
  vu_static(1, VU_PRF_OP, VU_SRC_VSFU);
  vu_static(1, VU_STATIC_DUP + VU_VRF_WT_INDEX, VRF_SIG);
  vu_static(1, VU_STATIC_DUP + VU_TYPE_VL, type_vl);

  vu_static(2, VU_LU_OP, op_word(VU_LU_LD_BF16, 0, 0));
  vu_static(2, VU_VALU0_OP,
            op_word(VU_VALU_FMUL_VV, VU_SRC_LU, VU_SRC_VRF_P0));
  vu_static(2, VU_SU_OP, op_word(VU_SU_NOP, 0, 0));
  vu_static(2, VU_PRF_OP, VU_SRC_VALU0);
  vu_static(2, VU_STATIC_DUP + VU_VRF_RD_INDEX, VRF_SIG);
  vu_static(2, VU_STATIC_DUP + VU_VRF_WT_INDEX, VRF_GATE);
  vu_static(2, VU_STATIC_DUP + VU_TYPE_VL, type_vl);

  vu_static(3, VU_LU_OP, op_word(VU_LU_LD_BF16, 0, 0));
  vu_static(3, VU_VALU0_OP,
            op_word(VU_VALU_FMUL_VV, VU_SRC_LU, VU_SRC_VRF_P0));
  vu_static(3, VU_SU_OP, op_word(VU_SU_ST_MXFP8, VU_SRC_VALU0, 0));
  vu_static(3, VU_PRF_OP, 0);
  vu_static(3, VU_STATIC_DUP + VU_VRF_RD_INDEX, VRF_GATE);
  vu_static(3, VU_STATIC_DUP + VU_TYPE_VL, type_vl);
}

/* 发一条宏指令：地址走动态副本。event_en 只给本 task 最后一条，前面的条不报
 * TS，避免一 task 多条 dsa_done 把链推过头。fence 等此前全部宏做完再派发。 */
static void vu_fire(u32 group, u32 ld, u32 st, u32 event_en, u32 fence) {
  dsa_write(VU_LD_ADDR, ld);
  dsa_write(VU_ST_ADDR, st);
  dsa_write(VU_MACRO_INST_TRIGGER,
            VU_MASK_LD_ADDR | VU_MASK_ST_ADDR
                | (group << VU_CONFIG_IDX_SHIFT)
                | (event_en ? VU_EVENT_EN : 0u)
                | (fence ? VU_MACRO_INST_FENCE : 0u));
}

/* 每个专家一份：silu(FC1)·FC3，量化成 MXFP8 作为 FC2 输入。
 *
 * 只在最后一条置 EVENT_EN，TASK_RECV_UNIT 配 DSA：最后一条被收下后 RV 就
 * task_done(1)，TS 等最后一条退休的 dsa_done。执行链按序退休，最后一条退休时
 * 前面的都已退休；一个 task 内各条写的 Core Mem 不重叠，fence 传 0。 */
TASK void task_vu_gate(void) {
  u32 base = CMEM_STREAM_BASE + stream_id() * CMEM_STREAM_STRIDE;
  u32 red = base + MOE_RED_OFF + MOE_SW_HEAD_BYTES;
  u32 e;
  gate_setup();
  for (e = 0; e < MOE_EXPERTS; ++e) {
    u32 fc1 = red + e * MOE_PART_STRIDE;
    u32 fc3 = red + (MOE_EXPERTS + e) * MOE_PART_STRIDE;
    u32 act = base + MOE_ACT_OFF + e * MOE_ACT_STRIDE;
    u32 last = (e + 1u == MOE_EXPERTS);
    vu_fire(1, fc1, act, 0, 0);
    vu_fire(2, fc1, act, 0, 0);
    vu_fire(3, fc3, act, last, 0);
  }
  vu_wait_trigger_taken();
  task_done(1);
}

/* ===== R core：两半求和 =====
 *
 * 两条宏指令，同样经 VRF 中转：一条宏指令只有一路 LU，两个向量进不来。
 *
 *   组 4  LU 读前一半（BF16）→ 写 VRF
 *   组 5  LU 读后一半（BF16）→ VALU0 与 VRF 相加 → SU 按 BF16 写回 Core Mem
 *
 * 向量长度是 MOE_EMBED，与门控那三条的 MOE_SEG_INTER 不同，所以另占两组静态配置。
 * firmware 不跑，每次 task_vu_add 开头写一遍。 */
static void add_setup(void) {
  /* 读写与中间一律 BF16，RNE。向量通路的一拍吃多少个 element 由 DATA_TYPE 定：
     BF16 一拍 64 个，正好是 CM 一拍 128 B 装的个数，一个块一段流过去；配成 FP32
     的话通路一拍只吃 32 个，一个块要拆成两段，通路就成了瓶颈，一条 VL = 6144 的
     向量在通路上要 192 拍而不是 96 拍 */
  u32 type_vl = MOE_EMBED | (1u << VU_DATA_TYPE_SHIFT);

  vu_static(4, VU_LU_OP, op_word(VU_LU_LD_BF16, 0, 0));
  vu_static(4, VU_SU_OP, op_word(VU_SU_NOP, 0, 0));
  vu_static(4, VU_PRF_OP, VU_SRC_LU);
  vu_static(4, VU_STATIC_DUP + VU_VRF_WT_INDEX, RC_VRF);
  vu_static(4, VU_STATIC_DUP + VU_TYPE_VL, type_vl);

  vu_static(5, VU_LU_OP, op_word(VU_LU_LD_BF16, 0, 0));
  vu_static(5, VU_VALU0_OP,
            op_word(VU_VALU_FADD_VV, VU_SRC_LU, VU_SRC_VRF_P0));
  vu_static(5, VU_SU_OP, op_word(VU_SU_ST_BF16, VU_SRC_VALU0, 0));
  vu_static(5, VU_PRF_OP, 0);
  vu_static(5, VU_STATIC_DUP + VU_VRF_RD_INDEX, RC_VRF);
  vu_static(5, VU_STATIC_DUP + VU_TYPE_VL, type_vl);
}

/* R core 链二的求和那一步：本行结果与上一行送来的那一份逐元素相加。
 *
 * 两半开头那 16 B 是软件辅助信息，跳过；结果写回前一半的同一处。与门控同一档：
 * 只在最后一条置 EVENT_EN，TASK_RECV_UNIT 配 DSA。组 5 读组 4 的 VRF，fence 传 0。 */
TASK void task_vu_add(void) {
  u32 base = CMEM_STREAM_BASE + stream_id() * CMEM_STREAM_STRIDE;
  u32 at = MOE_SW_HEAD_BYTES;
  add_setup();
  vu_fire(4, base + RC_A_OFF + at, base + RC_SUM_OFF + at, 0, 0);
  vu_fire(5, base + RC_B_OFF + at, base + RC_SUM_OFF + at, 1, 0);
  vu_wait_trigger_taken();
  task_done(1);
}

void kernel_init(void) {
  /* firmware 不跑。组 1/2/3 由 task_vu_gate 写，组 4/5 由 task_vu_add 写。 */
}
