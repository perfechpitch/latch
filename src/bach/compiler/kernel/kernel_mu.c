/* MU RV core 的 kernel。矩阵计算那一档。
 *
 * 寄存器名与位域照《Matrix Unit DSA》§Register Map Overview。MU 没有身份
 * 寄存器：给哪个 user 算由地址决定，Core Mem 按 stream 均分，kernel 读
 * CSR 拿 stream_id 再算出本 stream 那一段的基址。
 */

#include "bach.h"

#define TASK __attribute__((section(".text.task"), noinline, used))

/* 权重落 Matrix Mem 的起点。Core Mem 那一侧的分区在 bach.h 里 */
#define MMEM_WEIGHT_BASE 0x00000u

static u32 stream_base(void) {
  return CMEM_STREAM_BASE + stream_id() * CMEM_STREAM_STRIDE;
}

/* TASK_CFG 与 TASK_BLOCK 配完再写 SYS_CTRL.TASK_START，启动位必须最后写。
 * 身份先写：MU 没有从 RV core 直连过来的身份信号，dsa_done 填的就是这三个
 * 寄存器里的值，不写的话 TS 收到的完成对不上任何一个 stream */
static void mu_launch(u32 token, u32 weight, u32 out, u32 cfg,
                      u32 kblock, u32 nblock) {
  mmio_write(MU_IO_BASE, MU_STREAM_ID, stream_id());
  mmio_write(MU_IO_BASE, MU_TASK_ID, task_id());
  mmio_write(MU_IO_BASE, MU_USER_ID, user_id());
  mmio_write(MU_IO_BASE, MU_TASK_CFG, cfg);
  mmio_write(MU_IO_BASE, MU_TASK_BLOCK, kblock | (nblock << MU_NBLOCK_SHIFT));
  mmio_write(MU_IO_BASE, MU_ADDR_TOKEN, token);
  mmio_write(MU_IO_BASE, MU_ADDR_WEIGHT, weight);
  mmio_write(MU_IO_BASE, MU_ADDR_OUT, out);
  mmio_write(MU_IO_BASE, MU_SYS_CTRL, MU_TASK_START);
}

/* 一笔 gemv：从 Core Mem 读 token、Matrix Mem 读权重，结果写回 Core Mem。
 * 大 K 小 N，vlane=1，BF16 进 FP32 出 */
TASK void task_mu_compute(void) {
  u32 base = stream_base();
  mu_launch(base + CMEM_TOKEN_OFF, MMEM_WEIGHT_BASE, base + CMEM_FC1_OFF,
            MU_PRIM_TYPE_K256_N32 | (0u << MU_VLANE_SHIFT)
                | (0u << MU_DTYPE_AB_SHIFT),
            1, 1);
  task_done();
}

/* ===== 一层 MoE 那三笔 =====
 *
 * 三笔的形状与摆放照 bach.h 里的 MOE_*。差别只在三处：读哪个矩阵、结果写哪、
 * 几个专家各出一份还是合并成一份。 */

static void mu_moe(u32 token_off, u32 weight_off, u32 out_off, u32 kblock,
                   u32 nblock, u32 ep_reduce) {
  u32 base = stream_base();
  mmio_write(MU_IO_BASE, MU_STREAM_ID, stream_id());
  mmio_write(MU_IO_BASE, MU_TASK_ID, task_id());
  mmio_write(MU_IO_BASE, MU_USER_ID, user_id());
  mmio_write(MU_IO_BASE, MU_TASK_CFG,
             MU_PRIM_TYPE_K256_N32 | (0u << MU_VLANE_SHIFT)
                 | (0u << MU_DTYPE_AB_SHIFT));
  mmio_write(MU_IO_BASE, MU_TASK_BLOCK, kblock | (nblock << MU_NBLOCK_SHIFT));
  mmio_write(MU_IO_BASE, MU_ADDR_TOKEN, base + token_off);
  mmio_write(MU_IO_BASE, MU_ADDR_WEIGHT, weight_off);
  mmio_write(MU_IO_BASE, MU_ADDR_OUT, base + out_off);
  mmio_write(MU_IO_BASE, MU_AC_EXPERT_STRIDE, MOE_EXPERT_STRIDE);
  mmio_write(MU_IO_BASE, MU_B_EXPERT_STRIDE, MMEM_EXPERT_STRIDE);
  mmio_write(MU_IO_BASE, MU_EP_CTRL,
             MOE_EXPERTS | (ep_reduce ? MU_EP_REDUCE_EN : 0u));
  mmio_write(MU_IO_BASE, MU_TOPK_ADDR, MOE_TOPK_OFF);
  mmio_write(MU_IO_BASE, MU_TOPK_STRIDE, CMEM_STREAM_STRIDE);
  mmio_write(MU_IO_BASE, MU_SYS_CTRL, MU_TASK_START);
}

/* 等 MU 把手上这一笔做完 */
static void mu_wait(void) {
  while (mmio_read(MU_IO_BASE, MU_SYS_STATUS) & MU_BUSY) {
  }
}

/* FC1 与 FC3：任务链上是一步，里面两笔 MU。几个专家共用一份 token，各出一份
 * 结果。
 *
 * 每笔 MU 做完都会报一次 dsa_done，报两次会让 TS 把任务链推过头，所以这一档的
 * task_dsa_en 配 0：软件轮询 SYS_STATUS.BUSY 等两笔都做完再通知 TS */
TASK void task_mu_fc13(void) {
  mu_moe(MOE_TOKEN_OFF, MMEM_W1_OFF, MOE_FC1_OFF,
         MOE_KBLOCK_FC13, MOE_NBLOCK_FC13, 0);
  mu_wait();
  mu_moe(MOE_TOKEN_OFF, MMEM_W3_OFF, MOE_FC3_OFF,
         MOE_KBLOCK_FC13, MOE_NBLOCK_FC13, 0);
  mu_wait();
  task_done();
}

/* FC2：每个专家一份激活，按 topK 权重合并成一份结果。
 *
 * 结果按出核的 reduce 包切成 MOE_PIECE_NUM 段，一段一笔 MU，写进各自那一格
 * 格首 16 B 之后（摆放见 bach.h）。权重按 N 块连着放，第 k 段从第
 * k × MOE_PIECE_NBLOCK 个 N 块起。几笔 MU 各报一次 dsa_done，所以这一档与
 * FC1、FC3 一样收 RV core 那一路：轮询都做完再通知 TS */
TASK void task_mu_fc2(void) {
  u32 k;
  for (k = 0; k < MOE_PIECE_NUM; ++k) {
    mu_moe(MOE_ACT_OFF,
           MMEM_W2_OFF + k * MOE_PIECE_NBLOCK * MOE_KBLOCK_FC2 * MU_TILE_BYTES,
           moe_piece(k) + MOE_SW_HEAD_BYTES, MOE_KBLOCK_FC2, MOE_PIECE_NBLOCK,
           1);
    mu_wait();
  }
  task_done();
}

/* 专家权重累加，combine 那一步。输出降到 BF16 */
TASK void task_mu_accumulate(void) {
  u32 base = stream_base();
  mu_launch(base + CMEM_FC1_OFF, MMEM_WEIGHT_BASE, base + CMEM_ACT_OFF,
            MU_PRIM_TYPE_K256_N32 | (0u << MU_VLANE_SHIFT)
                | (0u << MU_DTYPE_AB_SHIFT) | MU_DTYPE_C_BF16,
            1, 1);
  task_done();
}

/* ===== R core 链二的第一步 =====
 *
 * 一个用户在本 core 占一个槽，两笔各一半，一笔 MOE_PIECE_NUM 包，每包搬完由硬件
 * 把这一包的 valid 标志置起来。这里循环扫标志表：一个槽前一半的几包都到了，后
 * 一半的几包也都到了（链首只等前一半），就把这个槽的标志清掉、把槽号交给后面几
 * 步、把这个槽是哪个用户写回身份寄存器，再向 TS 报完成。
 *
 * 扫不到就一直扫：这个 RV core 在 R core 的任务链上没有别的活，长期占用不挡
 * 同一个 core 上的其他 task。这一档不调 DSA。 */

/* 第 s 槽第 h 半第 k 包的标志。k 可以跨过前一半数进后一半 */
static u32 rc_flag(u32 s, u32 h, u32 k) {
  return RC_FLAG_OFF + ((s * 2u + h) * MOE_PIECE_NUM + k) * 4u;
}

/* 这一槽这一半的几包是不是都到了 */
static u32 rc_half_ready(u32 s, u32 h) {
  u32 k;
  for (k = 0; k < MOE_PIECE_NUM; ++k) {
    if (smem_read(rc_flag(s, h, k)) == 0) return 0;
  }
  return 1;
}

TASK void task_rc_find(void) {
  u32 head = smem_read(RC_HEAD_OFF);
  u32 s = 0;
  for (;;) {
    if (rc_half_ready(s, 0) && (head != 0 || rc_half_ready(s, 1))) {
      u32 k;
      for (k = 0; k < 2u * MOE_PIECE_NUM; ++k) smem_write(rc_flag(s, 0, k), 0);
      smem_write(RC_SLOT_OFF + stream_id() * 4, s);
      set_user_id(smem_read(RC_USER_OFF + s * 4));
      task_done();
      return;
    }
    s = s + 1;
    if (s >= RC_SLOTS) s = 0;
  }
}

void kernel_init(void) {
  /* 异常复位默认全屏蔽，写 0 打开上报 */
  mmio_write(MU_IO_BASE, MU_EXCEPT_MASK, 0);
}
