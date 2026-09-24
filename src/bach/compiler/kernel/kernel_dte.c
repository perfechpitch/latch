/* DTE RV core 的 kernel。
 *
 * 每笔 task 一个函数，函数地址就是 task_chain 里的 TASK_PC。函数名与
 * .bachir 的 (unit, opcode) 一一对应，编译器按这张对应关系取地址填表。
 * 段名统一放 .text.task.*，让链接脚本把它们排在 firmware 后面。
 */

#include "bach.h"

#define TASK __attribute__((section(".text.task"), noinline, used))

/* 一笔搬运：段 1 装数据，可再带一段 scale（段 2），最后写 CFG_TRANS_MODE 与
 * CFG_TRIGGER。寄存器落在 Config 区（0x0000~0x004C），照《DTE DSA》的地址空间。
 * len 是数据段的字节数，CFG_DATA_LEN 直接收字节，不再按 8 B 格折算。mode 的低
 * 3 位是 transfer_mode，高位带 DTE_SCALE_VALID 时 scale 随数据一起搬，scale 的
 * 字节数是软件按 len / 32 算好写进段 2 的 CFG_DATA_LEN。last 置位的那一笔带
 * ack_ts_en：一个 task 拆成几笔搬运时
 * 只有最后一笔带，DTE 做完它才通知 TS。前一笔还没交出去时寄存器接口顶住写，
 * 所以几笔可以接着配 */
static void dte_move(u32 src, u32 dst, u32 len, u32 mode, u32 last) {
  u32 tmode = mode & 0x7u;
  /* stride：Mmem 一侧软件给物理地址，不叠 stream 偏移；Cmem 一侧叠。MM→CM 那
   * 一档源是 Mmem 目标 Cmem，CFG_STRIDE 只作用在目标端（源端硬件强制 0），所以
   * 这里按目标端填 Cmem 的跨度。 */
  u32 stride = (tmode == DTE_MODE_MMEM_TO_ROUTER) ? 0u : CMEM_STREAM_STRIDE;
  u32 addr_valid = (1u << 1);  /* 段 1 = 数据 */

  /* 源端地址：Mmem 出发的两种 mode 打 Mmem tag，Cmem 出发 tag 是 0 不用打。
   * dst 是收方落点（Router 那几档）或本核 Cmem（MM→CM），都写纯地址不带 tag。 */
  u32 src1 = (tmode == DTE_MODE_MMEM_TO_ROUTER || tmode == DTE_MODE_MMEM_TO_CMEM)
                 ? dte_ep(DTE_EP_MMEM, src)
                 : src;
  dsa_write(DTE_ADDR1_SRC, src1);
  dsa_write(DTE_ADDR1_DST, dst);
  dsa_write(DTE_STRIDE1, stride);
  dsa_write(DTE_DATA_LEN1, len);

  if (mode & DTE_SCALE_VALID) {
    addr_valid |= (1u << 2);
    /* scale 段地址带 SCALE tag，低位给对应数据地址，DTE 落进数据那块存储的
     * scale 旁带。stride 与数据一致。 */
    dsa_write(DTE_ADDR2_SRC, dte_ep(DTE_EP_SCALE, src & 0x0FFFFFFFu));
    dsa_write(DTE_ADDR2_DST, dte_ep(DTE_EP_SCALE, dst & 0x0FFFFFFFu));
    dsa_write(DTE_STRIDE2, stride);
    /* 每 32 B 数据一个 scale，不足 32 B 的末段也算一个 */
    dsa_write(DTE_DATA_LEN2, (len + 31u) / 32u);
  }

  u32 trans = tmode | (addr_valid << DTE_ADDR_VALID_SHIFT)
                    | (mode & (DTE_HW_HEADER_OP | DTE_WR_SHAREMEM_FLAG))
                    | (last ? DTE_ACK_TS_EN : 0u);
  dsa_write(DTE_TRANS_MODE, trans);
  dsa_write(DTE_TRIGGER, 0u);
}

/* 把 Core Mem 上某一段搬到 Router 发出去。段的起点与长度由 shape 定。
 * 配的是段内偏移，落在哪一片由硬件按 stream_id 叠 */
static void send_seg(u32 off, u32 bytes) {
  dte_move(off, 0, bytes, DTE_MODE_CMEM_TO_ROUTER, 1);
}

/* 进核那一笔的配置：数据从 Router 的包来（没有源地址），落 dst。带 scale 时包尾
 * 那一段进 scale 旁带；mode 带 DTE_WR_SHAREMEM_FLAG 时搬完往 Share Mem 的
 * flag_addr 写 4 B 的 1（置到齐/占用标志）。flag_addr 可以是 0（标志表第 0 项），
 * 所以写不写看 mode，不看地址。落 Core Mem 叠 stream 偏移，落 Matrix Mem 是物理
 * 地址不叠。完成后带 ack_ts_en：普通 core 的 recv_unit=kDsa 等 DSA 这一路；B/R core 的 no_ack 由 SCP
 * 切模式时配，Fire 时压掉这一档。 */
static inline __attribute__((always_inline)) void dte_inbound(u32 dst, u32 len,
                                                               u32 mode,
                                                               u32 flag_addr) {
  u32 tmode = mode & 0x7u;
  u32 stride = (tmode == DTE_MODE_ROUTER_TO_CMEM) ? CMEM_STREAM_STRIDE : 0u;
  u32 addr_valid = (1u << 1);  /* 段 1 = 数据 */
  dsa_write(DTE_ADDR1_DST, dst);
  dsa_write(DTE_STRIDE1, stride);
  dsa_write(DTE_DATA_LEN1, len);
  if (mode & DTE_SCALE_VALID) {
    addr_valid |= (1u << 2);
    dsa_write(DTE_ADDR2_DST, dte_ep(DTE_EP_SCALE, dst & 0x0FFFFFFFu));
    dsa_write(DTE_STRIDE2, stride);
    /* 每 32 B 数据一个 scale，不足 32 B 的末段也算一个 */
    dsa_write(DTE_DATA_LEN2, (len + 31u) / 32u);
  }
  if (mode & DTE_WR_SHAREMEM_FLAG) {
    dsa_write(DTE_SM_W_ADDR, flag_addr);
    dsa_write(DTE_SM_W_DATA, 1u);
  }
  u32 trans = tmode | (addr_valid << DTE_ADDR_VALID_SHIFT)
                    | (mode & (DTE_HW_HEADER_OP | DTE_WR_SHAREMEM_FLAG))
                    | DTE_ACK_TS_EN;
  dsa_write(DTE_TRANS_MODE, trans);
  dsa_write(DTE_TRIGGER, 0u);
}

/* 一个包的 payload 里数据那一段的长度：带 scale 时 size = D + ceil(D / 32)，由此
 * 反推 D。 */
static u32 data_bytes(u32 size, u32 scale_valid) {
  return scale_valid ? size - (size + 32u) / 33u : size;
}

/* datain：配置驱动下由这里照包头配一笔进核搬运，再把包头弹掉。落点与 scale 都
 * 从包头读来，DTE 按配置搬。一个计算 core 上几项搬入任务（token、FC2 输入、归约
 * 结果、concat）都走它。 */
TASK void task_dte_user_init(void) {
  u32 size = hdr_size();
  u32 scale = hdr_scale_valid();
  u32 data = data_bytes(size, scale);
  dte_inbound(hdr_dst_addr(), data,
              DTE_MODE_ROUTER_TO_CMEM | (scale ? DTE_SCALE_VALID : 0u), 0);
  hdr_pop();
  task_done(1);
}

/* ===== 单 core 用例的几笔 ===== */

/* token 从 Core Mem 搬到 Router，往下游发，scale 随它走 */
TASK void task_dte_move(void) {
  dte_move(CMEM_TOKEN_OFF, 0, E2E_TOKEN_BYTES,
           DTE_MODE_CMEM_TO_ROUTER | DTE_SCALE_VALID, 1);
  task_done(1);
}

/* 把 MU 算完的那一段发给下游 */
TASK void task_dte_send_fc1(void) {
  send_seg(CMEM_FC1_OFF, E2E_OUT_BYTES);
  task_done(1);
}

/* 把 VU 算完的那一段发给下游 */
TASK void task_dte_send_act(void) {
  send_seg(CMEM_ACT_OFF, E2E_ACT_BYTES);
  task_done(1);
}

/* ===== 一层 MoE 那一段 ===== */

/* 部分和出核，逐级 reduce：一包里两个专家的 FC1 与 FC3。链上每个 core 发的包头
 * 都写 dot core 上收归约结果的那一处，Router 归约时照抄首份分量的包头，链尾交回
 * dot core 的那一份就落在那里。这笔任务由 Router 报完成 */
TASK void task_dte_send_part(void) {
  dte_move(MOE_PART_OFF, MOE_RED_OFF, MOE_PART_BYTES, DTE_MODE_CMEM_TO_ROUTER,
           1);
  task_done(1);
}

/* dot core：FC2 输入广播给本 chip 另外 7 个计算 core，scale 随它走，落在各自同一处 */
TASK void task_dte_send_fc2in(void) {
  dte_move(MOE_ACT_OFF, MOE_ACT_OFF, MOE_ACT_BYTES,
           DTE_MODE_CMEM_TO_ROUTER | DTE_SCALE_VALID, 1);
  task_done(1);
}

/* 计算 core：把 FC2 第 s 段发给 dot core，落点是 concat 区第 s 段。按槽位变化，
 * 每个槽位一个入口 */
static void send_concat(u32 s) {
  dte_move(moe_concat(s), moe_concat(s), MOE_FC2_BYTES, DTE_MODE_CMEM_TO_ROUTER,
           1);
  task_done(1);
}
TASK void task_dte_send_concat_s0(void) { send_concat(0); }
TASK void task_dte_send_concat_s1(void) { send_concat(1); }
TASK void task_dte_send_concat_s2(void) { send_concat(2); }
TASK void task_dte_send_concat_s3(void) { send_concat(3); }
TASK void task_dte_send_concat_s4(void) { send_concat(4); }
TASK void task_dte_send_concat_s5(void) { send_concat(5); }
TASK void task_dte_send_concat_s6(void) { send_concat(6); }

#if MOE_SLOTS != 8 || MOE_DOT_SLOT != 7
#error "task_dte_send_concat_s0～s6 按 8 个槽位、dot core 在槽位 7 写死，要跟着改"
#endif

/* dot core：行链出核，逐级 reduce。一包是 16 B 头加 concat 区，落到本行 R core 那个
 * 槽的前一半；一行 4 颗 chip 的 dot core 发的包头都写这一处，Router 归约时照抄首份
 * 分量的包头。这笔任务由 Router 报完成 */
TASK void task_dte_send_row(void) {
  dte_move(MOE_ROW_OFF, rc_land(user_id(), 0), MOE_ROW_BYTES,
           DTE_MODE_CMEM_TO_ROUTER, 1);
  task_done(1);
}

/* ===== R core 的两段 =====
 *
 * 进核那一笔的落点由包头 dst_addr 给（落哪一半是发方算好的），这一笔照它配进核
 * 搬运，搬完由 Completion RS 往标志表写这一半的 valid。这里另把这个槽是哪个用户
 * 记下来，链二找到齐了的槽之后要按它认人；再弹掉这个包的包头。 */
TASK void task_dte_rc_datain(void) {
  smem_write(RC_USER_OFF + (user_id() % RC_SLOTS) * 4, user_id());
  u32 landing = hdr_dst_addr();
  u32 data = hdr_size();  /* BF16，不带 scale */
  dte_inbound(landing, data, DTE_MODE_ROUTER_TO_MMEM | DTE_WR_SHAREMEM_FLAG,
              RC_FLAG_OFF + (landing / RC_HALF_BYTES) * 4u);
  hdr_pop();
  task_yield();
}

/* 链二的第二步：把那个槽的两半从 Matrix Mem 搬到 Core Mem。两边都连着摆，一笔
 * 搬运就够 */
TASK void task_dte_rc_load(void) {
  u32 slot = smem_read(RC_SLOT_OFF + stream_id() * 4);
  dte_move(RC_MM_BASE + slot * RC_SLOT_BYTES, RC_A_OFF, RC_SLOT_BYTES,
           DTE_MODE_MMEM_TO_CMEM, 1);
  task_done(1);
}

/* 链二的最后一步：求和结果送下一行的 R core，一包，与收进来时同一个摆法。落到那
 * 边哪个槽的哪一半，按同一条规则算：上一行的累加结果落后一半 */
TASK void task_dte_rc_send(void) {
  dte_move(RC_SUM_OFF, rc_land(user_id(), 1), MOE_ROW_BYTES,
           DTE_MODE_CMEM_TO_ROUTER, 1);
  task_done(1);
}

/* B core 的链一：token 落进 Matrix Mem 的环形缓冲，落点按自己收下的笔数取模算，
 * scale 随它进 scale 旁带，搬完置那一格的 valid。这里把这一格是哪个用户记下来，
 * 链二发的时候要按它认人。
 *
 * 记在第几格按自己收下的笔数算，与发方算落点用的是同一条规则 */
TASK void task_dte_bc_datain(void) {
  u32 n = smem_read(BC_RECV_OFF);
  smem_write(BC_USER_OFF + (n % BC_SLOTS) * 4, user_id());
  smem_write(BC_RECV_OFF, n + 1);
  u32 landing = bc_land(n);
  dte_inbound(landing, BC_TOKEN_BYTES,
              DTE_MODE_ROUTER_TO_MMEM | DTE_SCALE_VALID | DTE_WR_SHAREMEM_FLAG,
              BC_FLAG_OFF + (landing / BC_TOKEN_BYTES) * 4u);
  hdr_pop();
  task_yield();
}

/* B core 的链二第二步：把上一步认下的那一格广播给本组各 core，scale 随它走 */
TASK void task_dte_bc_send(void) {
  u32 slot = smem_read(BC_SLOT_OFF + stream_id() * 4);
  dte_move(bc_land(slot), MOE_TOKEN_OFF, BC_TOKEN_BYTES,
           DTE_MODE_MMEM_TO_ROUTER | DTE_SCALE_VALID, 1);
  task_done(1);
}

/* B core 的链二第三步：同一格再送一份给下一个 EP 组的 B core。落点要自己算好
 * 写进包头：收方那边按它收下的笔数取模找格子，两边同一条规则，所以这里按自
 * 己转出的笔数算 */
TASK void task_dte_bc_relay(void) {
  u32 slot = smem_read(BC_SLOT_OFF + stream_id() * 4);
  u32 n = smem_read(BC_SENT_OFF);
  smem_write(BC_SENT_OFF, n + 1);
  dte_move(bc_land(slot), bc_land(n), BC_TOKEN_BYTES,
           DTE_MODE_MMEM_TO_ROUTER | DTE_SCALE_VALID, 1);
  task_done(1);
}

/* 用户退休：本核这个用户的搬运都做完了，向上游还 credit */
TASK void task_dte_retire(void) {
  task_done(1);
}

/* weights 加载模式下由 datain_task 的 pc 指到这里。
 * 这一阶段进核那一笔落 Matrix Mem，落点是包头里的 dst_addr，scale 随包头的标记落
 * 进 scale 旁带，这里照它配一笔进核搬运，另数搬进来几笔。数满了中断 SCP，SCP 再
 * 把这颗 core 切到业务模式。Matrix Mem 一侧硬件不叠 stream 偏移，包头里带的就是
 * 最终地址。 */
TASK void task_dte_weights_loader(void) {
  u32 n = smem_read(WEIGHTS_CNT_OFF);
  smem_write(WEIGHTS_CNT_OFF, n + 1);
  u32 size = hdr_size();
  u32 scale = hdr_scale_valid();
  u32 data = data_bytes(size, scale);
  dte_inbound(hdr_dst_addr(), data,
              DTE_MODE_ROUTER_TO_MMEM | (scale ? DTE_SCALE_VALID : 0u), 0);
  hdr_pop();
  task_yield();
}

/* MSG 解析：DTE 要能解析包头并执行，MU 与 VU 不需要。软件包头表那套在新模型里
 * 不落地，这里只剩占位。 */
TASK void task_dte_msg_parse(void) {
  task_done(1);
}

/* 不调 DSA 的那一档：TS 的 unit 是 CU 或 SKIP，只跑 RV core。
 * 这类 task 在链上只起占位与推进作用，做完直接报完成。 */
TASK void task_rv_nop(void) {
  task_done(1);
}

/* 旧模型在模板 0 里写 stream_stride=0；新模型 stride 是逐段的 CFG_STRIDEi，
 * dte_move 每笔自己配，这里没有全局要预置的，留空。 */
void kernel_init(void) {}

