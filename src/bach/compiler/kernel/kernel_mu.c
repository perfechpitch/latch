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

/* 一条原语：从 Core Mem 读 token、Matrix Mem 读权重，结果写回 Core Mem。
 * 1×K128×N64，MXFP8 进、BF16 出，token 与权重的 scale 都随数据从旁带读出来 */
TASK void task_mu_compute(void) {
  u32 base = stream_base();
  mu_launch(base + CMEM_TOKEN_OFF, MMEM_WEIGHT_BASE, base + CMEM_FC1_OFF,
            MU_PRIM_TYPE_K128_N64 | MU_DTYPE_MXFP8 | MU_DTYPE_C_BF16, 1, 1);
  task_done();
}

/* ===== 一层 MoE 那一段 =====
 *
 * 形状与摆放照 bach.h 里的 MOE_*。几笔的差别在：读哪个矩阵、token 从哪一段起、
 * 结果写哪、几个专家各出一份还是合并成一份、按哪种原语切块。 */

static void mu_moe(u32 token_off, u32 weight_off, u32 out_off, u32 cfg,
                   u32 kblock, u32 nblock, u32 ac_stride, u32 ep_reduce) {
  u32 base = stream_base();
  mmio_write(MU_IO_BASE, MU_STREAM_ID, stream_id());
  mmio_write(MU_IO_BASE, MU_TASK_ID, task_id());
  mmio_write(MU_IO_BASE, MU_USER_ID, user_id());
  mmio_write(MU_IO_BASE, MU_TASK_CFG, cfg);
  mmio_write(MU_IO_BASE, MU_TASK_BLOCK, kblock | (nblock << MU_NBLOCK_SHIFT));
  mmio_write(MU_IO_BASE, MU_ADDR_TOKEN, base + token_off);
  mmio_write(MU_IO_BASE, MU_ADDR_WEIGHT, weight_off);
  mmio_write(MU_IO_BASE, MU_ADDR_OUT, base + out_off);
  mmio_write(MU_IO_BASE, MU_AC_EXPERT_STRIDE, ac_stride);
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

/* FC1 与 FC3 的部分和：token 第 s 段乘本 core 的 W1、W3，几个专家共用一份 token，
 * 各出一份结果，写进部分和那一包。1×K128×N64，MXFP8 进、BF16 出。
 *
 * 每笔 MU 做完都会报一次 dsa_done，报两次会让 TS 把任务链推过头，所以这一档的
 * task_dsa_en 配 0：软件轮询 SYS_STATUS.BUSY 等两笔都做完再通知 TS */
static void mu_part(u32 s) {
  u32 cfg = MU_PRIM_TYPE_K128_N64 | MU_DTYPE_MXFP8 | MU_DTYPE_C_BF16;
  u32 token = MOE_TOKEN_OFF + s * MOE_SEG_EMBED;
  mu_moe(token, MMEM_W1_OFF, MOE_FC1_OFF, cfg, MOE_KBLOCK_FC13,
         MOE_NBLOCK_FC13, MOE_PART_STRIDE, 0);
  mu_wait();
  mu_moe(token, MMEM_W3_OFF, MOE_FC3_OFF, cfg, MOE_KBLOCK_FC13,
         MOE_NBLOCK_FC13, MOE_PART_STRIDE, 0);
  mu_wait();
  task_done();
}

/* FC2 第 s 段：每个专家一份 FC2 输入，按 topK 权重在 MU 内合并成一份，写进 concat
 * 区第 s 段。1×K64×N128，也就是 K128×N64 阵列开 vlane = 2 */
static void mu_fc2(u32 s) {
  mu_moe(MOE_ACT_OFF, MMEM_W2_OFF, moe_concat(s),
         MU_PRIM_TYPE_K128_N64 | MU_VLANE2 | MU_DTYPE_MXFP8 | MU_DTYPE_C_BF16,
         MOE_KBLOCK_FC2, MOE_NBLOCK_FC2, MOE_ACT_STRIDE, 1);
  mu_wait();
  task_done();
}

/* 按槽位变化的任务每个槽位一个入口，由 TCHAIN 的 PC 选 */
TASK void task_mu_part_s0(void) { mu_part(0); }
TASK void task_mu_part_s1(void) { mu_part(1); }
TASK void task_mu_part_s2(void) { mu_part(2); }
TASK void task_mu_part_s3(void) { mu_part(3); }
TASK void task_mu_part_s4(void) { mu_part(4); }
TASK void task_mu_part_s5(void) { mu_part(5); }
TASK void task_mu_part_s6(void) { mu_part(6); }
TASK void task_mu_part_s7(void) { mu_part(7); }

TASK void task_mu_fc2_s0(void) { mu_fc2(0); }
TASK void task_mu_fc2_s1(void) { mu_fc2(1); }
TASK void task_mu_fc2_s2(void) { mu_fc2(2); }
TASK void task_mu_fc2_s3(void) { mu_fc2(3); }
TASK void task_mu_fc2_s4(void) { mu_fc2(4); }
TASK void task_mu_fc2_s5(void) { mu_fc2(5); }
TASK void task_mu_fc2_s6(void) { mu_fc2(6); }
TASK void task_mu_fc2_s7(void) { mu_fc2(7); }

#if MOE_SLOTS != 8
#error "task_mu_part_s0～s7 与 task_mu_fc2_s0～s7 按 8 个槽位写死，MOE_SLOTS 变了要跟着改"
#endif

/* ===== R core 链二的第一步 =====
 *
 * 一个用户在本 core 占一个槽，两半各一包，每包搬完由硬件把这一半的 valid 标志置
 * 起来。这里循环扫标志表：一个槽前一半到了，后一半也到了（链首只等前一半），就把
 * 这个槽的两个标志清掉、把槽号交给后面几步、把这个槽是哪个用户写回身份寄存器，
 * 再向 TS 报完成。
 *
 * 扫不到就一直扫：这个 RV core 在 R core 的任务链上没有别的活，长期占用不挡
 * 同一个 core 上的其他 task。这一档不调 DSA。 */

/* 第 s 槽第 h 半的标志 */
static u32 rc_flag(u32 s, u32 h) {
  return RC_FLAG_OFF + (s * 2u + h) * 4u;
}

TASK void task_rc_find(void) {
  u32 head = smem_read(RC_HEAD_OFF);
  u32 s = 0;
  for (;;) {
    if (smem_read(rc_flag(s, 0)) != 0 &&
        (head != 0 || smem_read(rc_flag(s, 1)) != 0)) {
      smem_write(rc_flag(s, 0), 0);
      smem_write(rc_flag(s, 1), 0);
      smem_write(RC_SLOT_OFF + stream_id() * 4, s);
      set_user_id(smem_read(RC_USER_OFF + s * 4));
      task_done();
      return;
    }
    s = s + 1;
    if (s >= RC_SLOTS) s = 0;
  }
}

/* ===== B core 链二的第一步：等有没发出去的 token =====
 *
 * 硬件把一笔搬进 Matrix Mem 之后置那一格的 valid，这里循环查 tail 指的那一格：
 * 置起来了就把 tail 推一格。head 与 tail 不相等就说明有还没发的，把 head 那一
 * 格是哪个用户写回身份寄存器，交给后面那一步发。
 *
 * 等不到就一直等：这个 RV core 在 B core 的任务链上没有别的活，长期占用不挡同
 * 一个 core 上的其他 task。这一档不调 DSA。 */
TASK void task_bc_wait(void) {
  for (;;) {
    u32 tail = smem_read(BC_TAIL_OFF);
    if (smem_read(BC_FLAG_OFF + (tail % BC_SLOTS) * 4) != 0) {
      tail = tail + 1;
      smem_write(BC_TAIL_OFF, tail);
    }
    u32 head = smem_read(BC_HEAD_OFF);
    if (head != tail) {
      u32 slot = head % BC_SLOTS;
      /* 认下这一格就把它从待发那一段里划走：这一笔报完成，TS 就把下一条链的
       * task 0 发下来，而本条链的 DTE 还没发，划晚了会认到同一格，同一笔发两遍 */
      smem_write(BC_FLAG_OFF + slot * 4, 0);
      smem_write(BC_HEAD_OFF, head + 1);
      smem_write(BC_SLOT_OFF + stream_id() * 4, slot);
      set_user_id(smem_read(BC_USER_OFF + slot * 4));
      task_done();
      return;
    }
  }
}

void kernel_init(void) {
  /* 异常复位默认全屏蔽，写 0 打开上报 */
  mmio_write(MU_IO_BASE, MU_EXCEPT_MASK, 0);
}
