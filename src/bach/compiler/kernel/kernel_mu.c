/* MU RV core 的 kernel。矩阵计算那一档。
 *
 * 寄存器名与位域照《Matrix Unit DSA》§Register Map Overview。地址模型：token
 * 与结果的物理地址由硬件按 base + stream_id × stream_stride 算，kernel 只写
 * base 与 stride；身份三项 streamID / taskID / userID 由 RV core 的 CSR 直连，
 * 不再写寄存器。
 */

#include "bach.h"

#define TASK __attribute__((section(".text.task"), noinline, used))

/* 权重落 Matrix Mem 的起点。Core Mem 那一侧的分区在 bach.h 里 */
#define MMEM_WEIGHT_BASE 0x00000u

/* 一条原语：从 Core Mem 读 token、Matrix Mem 读权重，结果写回 Core Mem。
 * 1×K128×N64，MXFP8 进、BF16 出，token 与权重的 scale 都随数据从旁带读出来。
 * 只有一笔任务，task_last 置 1，做完由 dsa_done 报 TS。 */
TASK void task_mu_compute(void) {
  dsa_write(MU_PRIMITIVE_DIM, 1u | (1u << MU_KBLOCK_SHIFT));
  dsa_write(MU_A_ADDR, CMEM_STREAM_BASE + CMEM_TOKEN_OFF);
  dsa_write(MU_A_STREAM_STRIDE, CMEM_STREAM_STRIDE);
  dsa_write(MU_C_ADDR, CMEM_STREAM_BASE + CMEM_FC1_OFF);
  dsa_write(MU_C_STREAM_STRIDE, CMEM_STREAM_STRIDE);
  dsa_write(MU_B_ADDR, MMEM_WEIGHT_BASE);
  dsa_write(MU_PRIMITIVE_MODE,
            MU_A_DTYPE_MXFP8 | MU_ROUTER_EP_DTYPE_MXFP8 | MU_C_DTYPE_BF16 |
                MU_TASK_LAST);
  dsa_write(MU_TASK_TRIGGER, MU_TRIGGER_VALID);
  task_done(1);
}

/* ===== 一层 MoE 那一段 =====
 *
 * 形状与摆放照 bach.h 里的 MOE_*。几笔的差别在：读哪个矩阵、token 从哪一段起、
 * 结果写哪、几个专家各出一份还是合并成一份、按哪种原语切块。 */

static void mu_moe(u32 token_off, u32 weight_off, u32 out_off, u32 mode,
                   u32 kblock, u32 nblock, u32 ac_stride, u32 ep_reduce,
                   u32 task_last) {
  dsa_write(MU_PRIMITIVE_DIM, nblock | (kblock << MU_KBLOCK_SHIFT));
  dsa_write(MU_A_ADDR, CMEM_STREAM_BASE + token_off);
  dsa_write(MU_A_STREAM_STRIDE, CMEM_STREAM_STRIDE);
  dsa_write(MU_C_ADDR, CMEM_STREAM_BASE + out_off);
  dsa_write(MU_C_STREAM_STRIDE, CMEM_STREAM_STRIDE);
  dsa_write(MU_B_ADDR, weight_off);
  dsa_write(MU_B_EXPERT_STRIDE, MMEM_EXPERT_STRIDE);
  /* AC_expert_stride 拆两侧：FC2 用 token 那侧，FC1/FC3 用 output 那侧，不用的
   * 写 0。 */
  dsa_write(MU_AC_EXPERT_STRIDE,
            ep_reduce ? (ac_stride << MU_TOKEN_EXPERT_STRIDE_SHIFT)
                      : (ac_stride << MU_OUTPUT_EXPERT_STRIDE_SHIFT));
  u32 full = mode | (MOE_EXPERTS << MU_ROUTER_EXPERT_SHIFT);
  if (ep_reduce) full |= MU_ROUTER_EP_REDUCE_EN;
  if (task_last) full |= MU_TASK_LAST;
  dsa_write(MU_PRIMITIVE_MODE, full);
  dsa_write(MU_TASK_TRIGGER, MU_TRIGGER_VALID);
}

/* FC1 与 FC3 的部分和：token 第 s 段乘本 core 的 W1、W3，几个专家共用一份 token，
 * 各出一份结果，写进部分和那一包。1×K128×N64，MXFP8 进、BF16 出。
 *
 * 两笔连着下发，只在最后一笔（FC3）置 task_last：MU 做完这一笔才报一次 dsa_done，
 * TS 据此推进任务链。RV core 下发完即可交还自己，不等 DSA 执行完。 */
static void mu_part(u32 s) {
  u32 mode = MU_A_DTYPE_MXFP8 | MU_ROUTER_EP_DTYPE_MXFP8 | MU_C_DTYPE_BF16;
  u32 token = MOE_TOKEN_OFF + s * MOE_SEG_EMBED;
  mu_moe(token, MMEM_W1_OFF, MOE_FC1_OFF, mode, MOE_KBLOCK_FC13,
         MOE_NBLOCK_FC13, MOE_PART_STRIDE, 0, 0);
  mu_moe(token, MMEM_W3_OFF, MOE_FC3_OFF, mode, MOE_KBLOCK_FC13,
         MOE_NBLOCK_FC13, MOE_PART_STRIDE, 0, 1);
  task_done(1);
}

/* FC2 第 s 段：每个专家一份 FC2 输入，按 topK 权重在 MU 内合并成一份，写进 concat
 * 区第 s 段。1×K64×N128，也就是 K128×N64 阵列开 primitive_type = 1 */
static void mu_fc2(u32 s) {
  u32 mode = MU_PRIM_TYPE_K64_N128 | MU_A_DTYPE_MXFP8 |
             MU_ROUTER_EP_DTYPE_MXFP8 | MU_C_DTYPE_BF16;
  mu_moe(MOE_ACT_OFF, MMEM_W2_OFF, moe_concat(s), mode, MOE_KBLOCK_FC2,
         MOE_NBLOCK_FC2, MOE_ACT_STRIDE, 1, 1);
  task_done(1);
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
 * 槽号就是 user_id。链一已经把 ready 的用户按顺序写入 FIFO，这里只比头尾指针：
 * 相等就继续等，不等就弹出队头，把槽号交给后面几步。弹出时清掉这一槽的映射计数
 * 和硬件标志，同一用户下一轮重新计。这一档不调 DSA。 */

TASK void task_rc_find(void) {
  for (;;) {
    u32 h = smem_read(RC_READY_HEAD_OFF);
    u32 t = smem_read(RC_READY_TAIL_OFF);
    if (h == t) continue;
    u32 s = smem_read(RC_READY_Q_OFF + (h & (RC_READY_CAP - 1u)) * 4u);
    smem_write(RC_READY_HEAD_OFF, h + 1u);
    smem_write(RC_MAP_OFF + s * 4u, 0);
    smem_write(rc_flag_off(s, 0), 0);
    smem_write(rc_flag_off(s, 1), 0);
    smem_write(RC_SLOT_OFF + stream_id() * 4, s);
    set_user_id(s);
    task_done(1);
    return;
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
      u32 u;
      do {
        u = smem_read(BC_USER_OFF + slot * 4);
      } while ((u & 0x80000000u) == 0);
      smem_write(BC_USER_OFF + slot * 4, 0);
      set_user_id(u & 0x7fffffffu);
      task_done(1);
      return;
    }
  }
}

void kernel_init(void) {
  /* 异常配置寄存器已随新寄存器表移除，本轮无初始化项。 */
}
