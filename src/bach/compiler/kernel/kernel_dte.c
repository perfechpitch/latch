/* DTE RV core 的 kernel。
 *
 * 每笔 task 一个函数，函数地址就是 task_chain 里的 TASK_PC。函数名与
 * .bachir 的 (unit, opcode) 一一对应，编译器按这张对应关系取地址填表。
 * 段名统一放 .text.task.*，让链接脚本把它们排在 firmware 后面。
 */

#include "bach.h"

#define TASK __attribute__((section(".text.task"), noinline, used))

/* 一笔搬运的四个寄存器加 Trigger，Trigger 必须最后写。
 * 寄存器落在模板 0 里，一套模板 64 B，照《DTE寄存器配置参数》的地址空间。
 * len 是字节数，CFG_DATA_LEN 收的是 8 B 一格的格数，所以这里除 8。传进来的
 * 字节数必须是 8 的倍数，硬件配不出不足一格的尾巴。
 * last 置位的那一笔带 task_last：一个 task 拆成几笔搬运时只有最后一笔带，DTE
 * 做完它才通知 TS。前一笔还没交出去时寄存器接口顶住写，所以几笔可以接着配 */
static void dte_move(u32 src, u32 dst, u32 len, u32 mode, u32 last) {
  u32 tpl = dte_template(0);
  mmio_write(DTE_IO_BASE, tpl + DTE_SRC_ADDR, src);
  mmio_write(DTE_IO_BASE, tpl + DTE_DST_ADDR, dst);
  mmio_write(DTE_IO_BASE, tpl + DTE_DATA_LEN, len / DTE_DATA_LEN_GRAIN);
  mmio_write(DTE_IO_BASE, tpl + DTE_TRIGGER, mode | (last ? DTE_TASK_LAST : 0u));
}

/* 把 Core Mem 上某一段搬到 Router 发出去。段的起点与长度由 shape 定。
 * 配的是段内偏移，落在哪一片由硬件按 stream_id 叠 */
static void send_seg(u32 off, u32 bytes) {
  dte_move(off, 0, bytes, DTE_MODE_CMEM_TO_ROUTER, 1);
}

/* datain：包一到 DTE 就自己起搬运，不必这里再配一笔。这一笔要做的是软件包头
 * 那一档：把硬件包头抄进按 stream 排的软件包头表，再弹掉这个包的包头。搬运
 * 的完成由 DTE 报 TS，所以这里交还自己就走，不通知 TS。 */
TASK void task_dte_user_init(void) {
  u32 tpl = dte_template(0);
  u32 head = mmio_read(DTE_IO_BASE, tpl + DTE_HW_HEADER_ADDR);
  mmio_write(DTE_IO_BASE, tpl + DTE_SW_HEADER_ADDR, head);
  hdr_pop();
  task_done();
}

/* Core Mem 搬到 Router，往下游发 */
TASK void task_dte_move(void) {
  dte_move(CMEM_TOKEN_OFF, 0, TOKEN_BYTES, DTE_MODE_CMEM_TO_ROUTER, 1);
  task_done();
}

/* 逐级 reduce：发进本级 ReduceModule，完成由 Router 报 TS */
TASK void task_dte_reduce(void) {
  dte_move(CMEM_TOKEN_OFF, 0, TOKEN_BYTES, DTE_MODE_CMEM_TO_ROUTER, 1);
  task_done();
}

/* 树形 reduce 的结果汇聚 */
TASK void task_dte_reduction(void) {
  dte_move(CMEM_TOKEN_OFF, 0, TOKEN_BYTES, DTE_MODE_CMEM_TO_ROUTER, 1);
  task_done();
}

/* 多份数据拼接后写回 Core Mem */
TASK void task_dte_concat(void) {
  dte_move(0, CMEM_TOKEN_OFF, TOKEN_BYTES, 0, 1);
  task_done();
}

/* 与外部节点收发 */
TASK void task_dte_fifo_in(void) {
  dte_move(0, 0, TOKEN_BYTES, 0, 1);
  task_done();
}

TASK void task_dte_fifo_out(void) {
  dte_move(0, 0, TOKEN_BYTES, 2, 1);
  task_done();
}

/* 把 MU 算完的那一段发给下游 */
TASK void task_dte_send_fc1(void) {
  send_seg(CMEM_FC1_OFF, FC1_BYTES);
  task_done();
}

/* 把 VU 算完的那一段发给下游 */
TASK void task_dte_send_act(void) {
  send_seg(CMEM_ACT_OFF, ACT_BYTES);
  task_done();
}

/* 把一层 MoE 那一段算完的结果发给下游。结果拆成 MOE_PIECE_NUM 个 reduce 包，
 * 一格一包，格首那 16 B 软件辅助信息随包一起发（摆放见 bach.h）。
 * 落到 R core 的哪个槽、哪一半，包头里带着走：链上每个 core 算出来的是同一个
 * 值，Router 归约时照抄首份分量的包头，末端那一份带的就是它。第 k 包落那一半的
 * 第 k 格。
 * 不走归约时一次发完，只有最后一包带 task_last，这个 task 只报一次完成 */
TASK void task_dte_send_moe(void) {
  u32 land = rc_land(user_id(), smem_read(MOE_SEND_HALF_OFF));
  u32 k;
  for (k = 0; k < MOE_PIECE_NUM; ++k) {
    dte_move(moe_piece(k), land + k * MOE_PIECE_STRIDE, MOE_PIECE_BYTES,
             DTE_MODE_CMEM_TO_ROUTER, k + 1 == MOE_PIECE_NUM);
  }
  task_done();
}

/* 走逐级 reduce 时一包一个任务：任务链上配 MOE_PIECE_NUM 项 reduce 任务，第 k
 * 项只发第 k 包，这笔任务由 Router 报完成 */
#if MOE_PIECE_NUM != 3
#error "task_dte_send_moe_p0～p2 按三包写死，MOE_PIECE_NUM 变了要跟着改"
#endif
static void send_moe_piece(u32 k) {
  u32 land = rc_land(user_id(), smem_read(MOE_SEND_HALF_OFF));
  dte_move(moe_piece(k), land + k * MOE_PIECE_STRIDE, MOE_PIECE_BYTES,
           DTE_MODE_CMEM_TO_ROUTER, 1);
}
TASK void task_dte_send_moe_p0(void) {
  send_moe_piece(0);
  task_done();
}
TASK void task_dte_send_moe_p1(void) {
  send_moe_piece(1);
  task_done();
}
TASK void task_dte_send_moe_p2(void) {
  send_moe_piece(2);
  task_done();
}

/* ===== R core 的两段 =====
 *
 * 进核那一笔的落点与 valid 标志都由硬件按包头办：Header Parser 解析包头就建
 * 描述符，搬完由 Completion RS 把标志写出去。这里只把这个槽是哪个用户记下来，
 * 链二找到齐了的槽之后要按它认人；再弹掉这个包的包头。 */
TASK void task_dte_rc_datain(void) {
  smem_write(RC_USER_OFF + (user_id() % RC_SLOTS) * 4, user_id());
  hdr_pop();
  task_yield();
}

/* 链二的第二步：把那个槽的两笔从 Matrix Mem 搬到 Core Mem。两边都按格连着
 * 摆，一笔搬运就够 */
TASK void task_dte_rc_load(void) {
  u32 slot = smem_read(RC_SLOT_OFF + stream_id() * 4);
  dte_move(RC_MM_BASE + slot * RC_SLOT_BYTES, RC_A_OFF, RC_SLOT_BYTES,
           DTE_MODE_MMEM_TO_CMEM, 1);
  task_done();
}

/* 链二的最后一步：求和结果送下一组的 R core，一格一包，与收进来时同一个摆法。
 * 落到那边哪个槽的哪一半，按同一条规则算：上游组的中间结果落后一半。这一步
 * 不走归约，只在最后一包带 task_last，报一次完成 */
TASK void task_dte_rc_send(void) {
  u32 land = rc_land(user_id(), 1);
  u32 k;
  for (k = 0; k < MOE_PIECE_NUM; ++k) {
    dte_move(RC_SUM_OFF + k * MOE_PIECE_STRIDE, land + k * MOE_PIECE_STRIDE,
             MOE_PIECE_BYTES, DTE_MODE_CMEM_TO_ROUTER, k + 1 == MOE_PIECE_NUM);
  }
  task_done();
}

/* B core 的链一：token 落进 Matrix Mem 的环形缓冲，落点与 valid 标志都由硬件按
 * 包头办。这里只把这一格是哪个用户记下来，链二发的时候要按它认人。
 *
 * 记在第几格按自己收下的笔数算，与发方算落点用的是同一条规则 */
TASK void task_dte_bc_datain(void) {
  u32 n = smem_read(BC_RECV_OFF);
  smem_write(BC_USER_OFF + (n % BC_SLOTS) * 4, user_id());
  smem_write(BC_RECV_OFF, n + 1);
  hdr_pop();
  task_yield();
}

/* B core 的链二第二步：把上一步认下的那一格广播给本组各 core */
TASK void task_dte_bc_send(void) {
  u32 slot = smem_read(BC_SLOT_OFF + stream_id() * 4);
  dte_move(bc_land(slot), 0, BC_TOKEN_BYTES, DTE_MODE_MMEM_TO_ROUTER, 1);
  task_done();
}

/* B core 的链二第三步：同一格再送一份给下一个 EP 组的 B core。落点要自己算好
 * 写进包头：收方那边按它收下的笔数取模找格子，两边同一条规则，所以这里按自
 * 己转出的笔数算 */
TASK void task_dte_bc_relay(void) {
  u32 slot = smem_read(BC_SLOT_OFF + stream_id() * 4);
  u32 n = smem_read(BC_SENT_OFF);
  smem_write(BC_SENT_OFF, n + 1);
  dte_move(bc_land(slot), bc_land(n), BC_TOKEN_BYTES, DTE_MODE_MMEM_TO_ROUTER, 1);
  task_done();
}

/* 用户退休：本核这个用户的搬运都做完了，向上游还 credit */
TASK void task_dte_retire(void) {
  task_done();
}

/* weights 加载模式下由 datain_task 的 pc 指到这里。
 * 这一阶段进核那一笔落 Matrix Mem，落点是包头里的 dst_addr，搬运由 DTE 自己
 * 起，所以这里只数搬进来几笔。数满了中断 SCP，SCP 再把这颗 core 切到业务模式。
 * Matrix Mem 一侧硬件不叠 stream 偏移，包头里带的就是最终地址。 */
TASK void task_dte_weights_loader(void) {
  u32 n = smem_read(WEIGHTS_CNT_OFF);
  smem_write(WEIGHTS_CNT_OFF, n + 1);
  hdr_pop();
  task_yield();
}

/* MSG 解析：DTE 要能解析包头并执行，MU 与 VU 不需要 */
TASK void task_dte_msg_parse(void) {
  u32 tpl = dte_template(0);
  u32 head = mmio_read(DTE_IO_BASE, tpl + DTE_HW_HEADER_ADDR);
  mmio_write(DTE_IO_BASE, tpl + DTE_SW_HEADER_ADDR, head);
  task_done();
}

/* 不调 DSA 的那一档：TS 的 unit 是 CU 或 SKIP，只跑 RV core。
 * 这类 task 在链上只起占位与推进作用，做完直接报完成。 */
TASK void task_rv_nop(void) {
  task_done();
}

void kernel_init(void) {
  mmio_write(DTE_IO_BASE, dte_template(0) + DTE_STREAM_STRIDE, 0);
}
