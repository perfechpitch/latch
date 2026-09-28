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

/* 单 core 用例的 VU 那一步：MU 算出来的 BF16 逐元素算一遍，仍按 BF16 写回。
 * 算什么由第 0 组静态配置定。置 EVENT_EN，VU 退休才把 dsa_done 打给 TS。 */
TASK void task_vu_compute(void) {
  u32 base = CMEM_STREAM_BASE + stream_id() * CMEM_STREAM_STRIDE;
  vu_launch(0, base + CMEM_FC1_OFF, base + CMEM_ACT_OFF, 0);
  task_done(1);
}

/* ===== dot core：silu·dot·量化 =====
 *
 * 收成两条宏指令。组 0 留给
 * task_vu_compute，这里用组 1 / 组 2。VL = MOE_SEG_INTER（256）个 FP32，
 * 一个 VRF entry 装 32 个，x 占 entry 0，sigmoid(x) 占 entry 8。
 *
 *   组 1  LU 读 fc1（x）→ 写入 VRF_WT_P0，同时 bypass 进 VSFU 出 sigmoid
 *         → 写入 VRF_WT_P1。一条里 LU 同时供两处，模型没有 DATA_BROADCAST 位，
 *         旁路本身就分得开。
 *   组 2  VALU0 = sigmoid(x) * x（两源都读 VRF）；
 *         LU 读 fc3（y）bypass 进 VALU1，VALU1 = silu(x) * y；
 *         SU 从 VALU1 量化成 MXFP8 写回，不落 VRF。
 *
 * MXFP8_SCALE_ROUND 保持 0（向下取整），与参考向量一致。参考那份把自己的
 * 黄金比对配成向上取整，这里不对齐那一位。
 * 两条之间是 VRF 上的 RAW，记分板会串起来，不必再置 MACRO_INST_FENCE。 */

#if 0
/* 组 1/2 改由 bundle VUSTATIC 在装载时写，不再每个 token 重配。 */
#define VRF_X    0u
#define VRF_SIG  8u   /* ⌈VL / 32⌉，VL = MOE_SEG_INTER */

static void vu_static(u32 group, u32 off, u32 data) {
  dsa_write(vu_static_group(group) + off, data);
}

static u32 op_word(u32 opcode, u32 src1, u32 src2) {
  return opcode | (src1 << VU_SRC1_SHIFT) | (src2 << VU_SRC2_SHIFT);
}

static void gate_setup(void) {
  u32 type_vl = MOE_SEG_INTER;   /* FP32、RNE 都是 0 */
  u32 vrf_wt = (VRF_SIG << 16) | VRF_X;
  u32 vrf_rd = (VRF_X << 16) | VRF_SIG;

  vu_static(1, VU_LU_OP, op_word(VU_LU_LD_BF16, 0, 0));
  vu_static(1, VU_VSFU_OP, op_word(VU_VSFU_SIGMOID, VU_SRC_LU, 0));
  vu_static(1, VU_PRF_OP, VU_SRC_LU | (VU_SRC_VSFU << VU_SRC1_SHIFT));
  vu_static(1, VU_STATIC_DUP + VU_VRF_WT_INDEX, vrf_wt);
  vu_static(1, VU_STATIC_DUP + VU_VRF_RD_INDEX, vrf_rd);
  vu_static(1, VU_STATIC_DUP + VU_TYPE_VL, type_vl);

  vu_static(2, VU_LU_OP, op_word(VU_LU_LD_BF16, 0, 0));
  vu_static(2, VU_VALU0_OP,
            op_word(VU_VALU_FMUL_VV, VU_SRC_VRF_P0, VU_SRC_VRF_P1));
  vu_static(2, VU_VALU1_OP,
            op_word(VU_VALU_FMUL_VV, VU_SRC_VALU0, VU_SRC_LU));
  vu_static(2, VU_SU_OP, op_word(VU_SU_ST_MXFP8, VU_SRC_VALU1, 0));
  vu_static(2, VU_PRF_OP, 0);
  vu_static(2, VU_STATIC_DUP + VU_VRF_WT_INDEX, vrf_wt);
  vu_static(2, VU_STATIC_DUP + VU_VRF_RD_INDEX, vrf_rd);
  vu_static(2, VU_STATIC_DUP + VU_TYPE_VL, type_vl);
}
#endif

/* 发一条宏指令。mask 里置位的地址走动态副本。event_en 只给本 task 最后一条。
 * fence 等此前全部宏做完再派发。 */
static void vu_fire(u32 group, u32 ld, u32 st, u32 mask, u32 event_en,
                    u32 fence) {
  if (mask & VU_MASK_LD_ADDR) dsa_write(VU_LD_ADDR, ld);
  if (mask & VU_MASK_ST_ADDR) dsa_write(VU_ST_ADDR, st);
  dsa_write(VU_MACRO_INST_TRIGGER,
            mask | (group << VU_CONFIG_IDX_SHIFT)
                | (event_en ? VU_EVENT_EN : 0u)
                | (fence ? VU_MACRO_INST_FENCE : 0u));
}

/* 每个专家一份：silu(fc1)·fc3，量化成 MXFP8 作为 FC2 输入。
 *
 * 组 1 只动态化 LD（fc1）；组 2 动态化 LD（fc3）与 ST（act）。只在本 task
 * 最后一个专家的组 2 置 EVENT_EN。TASK_RECV_UNIT 配 DSA，所以仍 task_done(1)：
 * TS 要收齐 rv_done 与这一笔 dsa_done。 */
TASK void task_vu_gate(void) {
  u32 base = CMEM_STREAM_BASE + stream_id() * CMEM_STREAM_STRIDE;
  u32 red = base + MOE_RED_OFF + MOE_SW_HEAD_BYTES;
  u32 e;
  /* gate_setup(); 静态组由 bundle VUSTATIC / PreloadVuGate 写 */
  for (e = 0; e < MOE_EXPERTS; ++e) {
    u32 fc1 = red + e * MOE_PART_STRIDE;
    u32 fc3 = red + (MOE_EXPERTS + e) * MOE_PART_STRIDE;
    u32 act = base + MOE_ACT_OFF + e * MOE_ACT_STRIDE;
    u32 last = (e + 1u == MOE_EXPERTS);
    vu_fire(1, fc1, 0, VU_MASK_LD_ADDR, 0, 0);
    vu_fire(2, fc3, act, VU_MASK_LD_ADDR | VU_MASK_ST_ADDR, last, 0);
  }
  task_done(1);
}

/* ===== R core：两半求和 =====
 *
 * 两条宏指令，同样经 VRF 中转：一条宏指令只有一路 LU，两个向量进不来。
 *
 *   组 4  LU 读前一半（BF16）→ 写 VRF
 *   组 5  LU 读后一半（BF16）→ VALU0 与 VRF 相加 → SU 按 BF16 写回 Core Mem
 *
 * 向量长度是 MOE_EMBED，与门控那两条的 MOE_SEG_INTER 不同，所以另占两组静态配置。
 * 组 4/5 由 bundle VUSTATIC / PreloadVuAdd 在装载时写。 */
#if 0
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
#endif

/* R core 链二的求和那一步：本行结果与上一行送来的那一份逐元素相加。
 *
 * 两半开头那 16 B 是软件辅助信息，跳过；结果写回前一半的同一处。与门控同一档：
 * 只在最后一条置 EVENT_EN，TASK_RECV_UNIT 配 DSA。组 5 读组 4 的 VRF，fence 传 0。 */
TASK void task_vu_add(void) {
  u32 base = CMEM_STREAM_BASE + stream_id() * CMEM_STREAM_STRIDE;
  u32 at = MOE_SW_HEAD_BYTES;
  /* add_setup(); 静态组由 bundle VUSTATIC / PreloadVuAdd 写 */
  vu_fire(4, base + RC_A_OFF + at, base + RC_SUM_OFF + at,
          VU_MASK_LD_ADDR | VU_MASK_ST_ADDR, 0, 0);
  vu_fire(5, base + RC_B_OFF + at, base + RC_SUM_OFF + at,
          VU_MASK_LD_ADDR | VU_MASK_ST_ADDR, 1, 0);
  task_done(1);
}

void kernel_init(void) {
  /* firmware 不跑。组 1/2、4/5 由 bundle VUSTATIC 在装载时写。 */
}
