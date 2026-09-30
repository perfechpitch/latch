/* DTE RV core 的 kernel。
 *
 * 每笔 task 一个函数，函数地址就是 task_chain 里的 TASK_PC。函数名与
 * .bachir 的 (unit, opcode) 一一对应，编译器按这张对应关系取地址填表。
 * 段名统一放 .text.task.*，让链接脚本把它们排在 firmware 后面。
 */

#include "bach.h"

#define TASK __attribute__((section(".text.task"), noinline, used))

/* 模板：tpl_setup 把每套的常量字段配好，业务 task 只写随任务变化的那几项，再带
 * 模板号写 CFG_TRIGGER（《DTE DSA》加速配置方式）。本笔显式写过的字段盖过模板里
 * 的同名字段。模型不跑 firmware，tpl_setup 由每个 core 最先跑的那种进核任务在
 * 第一次时调一次，见 tpl_once。 */
#define TPL_IN_TOKEN    0u  /* token 进核：落点、长度、scale、topK 都是常量 */
#define TPL_IN_CM       1u  /* 进 Core Mem、不带 scale：归约结果、concat、行链那一包 */
#define TPL_SEND_PART   2u  /* 部分和出核，字段全是常量 */
#define TPL_SEND_FC2IN  3u  /* FC2 输入广播，字段全是常量 */
#define TPL_SEND_ROW    4u  /* 行链出核，落点按用户变 */
#define TPL_SEND_CONCAT 5u  /* concat 出核，源与落点按槽位变 */
#define TPL_RC_SEND     6u  /* R core 把槽里的一份送进 Rmem，源与落点按用户变 */
#define TPL_BC_SEND     7u  /* B core 广播与转发，源与落点按格子变 */

/* 一组寄存器的基址：0 是 Config 区；模板里没有 CFG_TRIGGER，各字段比 Config 区
 * 低 4 B，所以第 n 套模板的基址取它的起点减 4。偏移一律是常量，dsawi 直接编码 */
#define DTE_CFG 0u
#define DTE_TPL(n) (DTE_TEMPLATE_BASE + (n) * DTE_TEMPLATE_STRIDE - 4u)

/* 带模板号提交：只写本笔变的那几项之后调它 */
static inline __attribute__((always_inline)) void dte_fire(u32 tpl) {
  dsa_write(DTE_TRIGGER, DTE_TEMP_VALID | (tpl << DTE_TEMP_INDEX_SHIFT));
}

/* 包头（段 0）地址。段 0 恒参与，端点由地址高 4 bit 译码选落点（见 bach.h
 * DTE_EP_*）：计算 core 打 header tag 落 Hmem（低 28 bit 给 stream_id）；B/R core
 * 写 Core Mem 纯地址（tag 0），它们的数据都进 Matrix Mem，Core Mem 让给包头、按
 * 用户分一块（64 B，够 48 B 包头上下文）。 */
#define CMEM_HDR_STRIDE 0x40u
static inline u32 dte_hdr_h(void) { return dte_ep(DTE_EP_HDR, stream_id()); }
static inline u32 dte_hdr_cmem(u32 user) { return user * CMEM_HDR_STRIDE; }

/* 一笔搬运：段 1 装数据，可再带一段 scale（段 2），最后写 CFG_TRANS_MODE 与
 * CFG_TRIGGER。寄存器落在 Config 区（0x0000~0x004C），照《DTE DSA》的地址空间。
 * len 是数据段的字节数，CFG_DATA_LEN 直接收字节，不再按 8 B 格折算。mode 的低
 * 3 位是 transfer_mode，高位带 DTE_SCALE_VALID 时 scale 随数据一起搬，scale 的
 * 字节数是软件按 len / 32 算好写进段 2 的 CFG_DATA_LEN。last 置位的那一笔带
 * ack_ts_en：一个 task 拆成几笔搬运时
 * 只有最后一笔带，DTE 做完它才通知 TS。前一笔还没交出去时寄存器接口顶住写，
 * 所以几笔可以接着配 */
static inline __attribute__((always_inline)) void dte_fields(
    u32 base, u32 src, u32 dst, u32 len, u32 mode, u32 last, u32 topk_idx) {
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
  dsa_write(base + DTE_ADDR1_SRC, src1);
  dsa_write(base + DTE_ADDR1_DST, dst);
  dsa_write(base + DTE_STRIDE1, stride);
  dsa_write(base + DTE_DATA_LEN1, len);

  if (mode & DTE_SCALE_VALID) {
    addr_valid |= (1u << 2);
    /* scale 段地址带 SCALE tag，低位给对应数据地址，DTE 落进数据那块存储的
     * scale 旁带。stride 与数据一致。 */
    dsa_write(base + DTE_ADDR2_SRC, dte_ep(DTE_EP_SCALE, src & 0x0FFFFFFFu));
    dsa_write(base + DTE_ADDR2_DST, dte_ep(DTE_EP_SCALE, dst & 0x0FFFFFFFu));
    dsa_write(base + DTE_STRIDE2, stride);
    /* 每 32 B 数据一个 scale，不足 32 B 的末段也算一个 */
    dsa_write(base + DTE_DATA_LEN2, (len + 31u) / 32u);
  }

  if (mode & DTE_TOPK_VALID) {
    /* topK 旁带（段 3）：不读写存储，地址打 TOPK tag，低位是 MU topK_ep_table 的
     * 下标（计算 core 是 stream_id，B core 是 user_id）。进核那一笔把包里的 topK
     * 写进表，出核那一笔把表里的 topK 附回要发的包。 */
    u32 inbound = tmode == DTE_MODE_ROUTER_TO_CMEM ||
                  tmode == DTE_MODE_ROUTER_TO_MMEM;
    addr_valid |= (1u << 3);
    dsa_write(base + DTE_ADDR3_SRC, inbound ? 0u : dte_ep(DTE_EP_TOPK, topk_idx));
    dsa_write(base + DTE_ADDR3_DST, inbound ? dte_ep(DTE_EP_TOPK, topk_idx) : 0u);
    dsa_write(base + DTE_STRIDE3, 0);
    dsa_write(base + DTE_DATA_LEN3, MOE_TOPK_BYTES);
  }

  u32 trans = tmode | (addr_valid << DTE_SEG_VALID_SHIFT)
                    | (mode & (DTE_HW_HEADER_OP | DTE_WR_SHAREMEM_FLAG))
                    | (last ? DTE_ACK_TS_EN : 0u);
  dsa_write(base + DTE_TRANS_MODE, trans);
}

static void dte_move(u32 src, u32 dst, u32 len, u32 mode, u32 last) {
  dte_fields(DTE_CFG, src, dst, len, mode, last, 0);
  dsa_write(DTE_ADDR0_SRC, dte_hdr_h());  /* 段 0 = 包头，出核从 Hmem 读回 */
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
 * 地址不叠。ack 置位的那一笔带 ack_ts_en、完成后通知 TS；B/R core 与 weights 加载
 * 阶段进核不建 stream 表项、没有可报的对象，配 0。 */
static inline __attribute__((always_inline)) void dte_inbound(u32 dst, u32 len,
                                                               u32 mode,
                                                               u32 flag_addr,
                                                               u32 topk_idx,
                                                               u32 ack,
                                                               u32 hdr) {
  u32 tmode = mode & 0x7u;
  u32 stride = (tmode == DTE_MODE_ROUTER_TO_CMEM) ? CMEM_STREAM_STRIDE : 0u;
  u32 addr_valid = (1u << 1);  /* 段 1 = 数据 */
  dsa_write(DTE_ADDR0_DST, hdr);  /* 段 0 = 包头，地址 tag 选 Hmem / Core Mem */
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
  if (mode & DTE_TOPK_VALID) {
    /* topK 旁带：不落存储，按下标写进 MU 的 topK_ep_table，见 dte_fields */
    addr_valid |= (1u << 3);
    dsa_write(DTE_ADDR3_DST, dte_ep(DTE_EP_TOPK, topk_idx));
    dsa_write(DTE_STRIDE3, 0);
    dsa_write(DTE_DATA_LEN3, MOE_TOPK_BYTES);
  }
  if (mode & DTE_WR_SHAREMEM_FLAG) {
    dsa_write(DTE_SM_W_ADDR, flag_addr);
    dsa_write(DTE_SM_W_DATA, 1u);
  }
  u32 trans = tmode | (addr_valid << DTE_SEG_VALID_SHIFT)
                    | (mode & (DTE_HW_HEADER_OP | DTE_WR_SHAREMEM_FLAG))
                    | (ack ? DTE_ACK_TS_EN : 0u);
  dsa_write(DTE_TRANS_MODE, trans);
  dsa_write(DTE_TRIGGER, 0u);
}

/* 一个包的 payload 里数据那一段的长度：带 scale 时 size = D + ceil(D / 32)，由此
 * 反推 D。 */
static u32 data_bytes(u32 size, u32 scale_valid) {
  return scale_valid ? size - (size + 32u) / 33u : size;
}

static void tpl_setup(void);

/* 模板配过没有：配完写 TPL_READY。DTCM 装载时不清零（真机上由 firmware 清
 * .bss，模型不跑 firmware，读出来是填充值），所以认这个特定值，不认零与非零 */
#define TPL_READY 0x7E1C0DE5u
static u32 tpl_ready;

/* 每个 core 最先跑的那种进核任务开头调它：计算 core 与 dot core 是 token 进核，
 * B core、R core 是各自的 datain，单 core 用例是 task_dte_user_init。用到模板的
 * 出核任务都排在这些之后 */
static inline __attribute__((always_inline)) void tpl_once(void) {
  if (tpl_ready != TPL_READY) {
    tpl_setup();
    tpl_ready = TPL_READY;
  }
}

/* ===== 进核 datain：配置驱动 =====
 *
 * 《DTE DSA》进核是配置驱动：收方 kernel 知道自己要收什么，长度、落点、带不带
 * scale 都是编译期定值，照它配 CFG，不读包头字段，包头只在 hdr_pop() 里弹掉。
 * MoE 那几种搬入各是一个入口。 */

/* token 进核（IN_PATH）：整份 token 落到 MOE_TOKEN_OFF，scale 与 topK 随它走，topK
 * 按 stream_id 写进 MU 的 topK_ep_table。其余字段都在模板里 */
TASK void task_dte_token_datain(void) {
  tpl_once();
  dsa_write(DTE_ADDR0_DST, dte_hdr_h());
  dsa_write(DTE_ADDR3_DST, dte_ep(DTE_EP_TOPK, stream_id()));
  dte_fire(TPL_IN_TOKEN);
  hdr_pop();
  task_done(0);
}

/* FC2 输入进核（FC2_BCAST_PATH）：dot core 广播给本 chip 各计算 core，落在各自
 * 同一处 MOE_ACT_OFF，scale 随它 */
TASK void task_dte_fc2in_datain(void) {
  dte_inbound(MOE_ACT_OFF, MOE_ACT_BYTES,
              DTE_MODE_ROUTER_TO_CMEM | DTE_SCALE_VALID, 0, 0, 1, dte_hdr_h());
  hdr_pop();
  task_done(0);
}

/* 不带 scale 的进核：落点与长度每笔写，其余在模板里 */
static void in_cm(u32 dst, u32 len) {
  dsa_write(DTE_ADDR0_DST, dte_hdr_h());
  dsa_write(DTE_ADDR1_DST, dst);
  dsa_write(DTE_DATA_LEN1, len);
  dte_fire(TPL_IN_CM);
  hdr_pop();
  task_done(0);
}

/* 归约结果进核（CHIP_RED_PATH）：dot core 收 chip 内归约结果 */
TASK void task_dte_red_datain(void) { in_cm(MOE_RED_OFF, MOE_PART_BYTES); }

/* concat 第 s 段进核（CONCAT_PATH[s]）：dot core 收槽位 s 的 FC2，落在 concat 区
 * 第 s 段，与 task_dte_send_concat_s* 一进一出同一套落点 */
TASK void task_dte_concat_datain_s0(void) { in_cm(moe_concat(0), MOE_FC2_BYTES); }
TASK void task_dte_concat_datain_s1(void) { in_cm(moe_concat(1), MOE_FC2_BYTES); }
TASK void task_dte_concat_datain_s2(void) { in_cm(moe_concat(2), MOE_FC2_BYTES); }
TASK void task_dte_concat_datain_s3(void) { in_cm(moe_concat(3), MOE_FC2_BYTES); }
TASK void task_dte_concat_datain_s4(void) { in_cm(moe_concat(4), MOE_FC2_BYTES); }
TASK void task_dte_concat_datain_s5(void) { in_cm(moe_concat(5), MOE_FC2_BYTES); }
TASK void task_dte_concat_datain_s6(void) { in_cm(moe_concat(6), MOE_FC2_BYTES); }

/* 行链上一颗 chip 送来的那一包进核：落在 MOE_ROW_IN_OFF */
TASK void task_dte_row_datain(void) { in_cm(MOE_ROW_IN_OFF, MOE_ROW_BYTES); }

/* ===== 单 core 用例的几笔 ===== */

/* 单 core 用例的进核：没有编译期定死的长度与落点，照包头配一笔 */
TASK void task_dte_user_init(void) {
  tpl_once();
  u32 size = hdr_size();
  u32 scale = hdr_scale_valid();
  u32 data = data_bytes(size, scale);
  dte_inbound(hdr_dst_addr(), data,
              DTE_MODE_ROUTER_TO_CMEM | (scale ? DTE_SCALE_VALID : 0u), 0, 0, 1,
              dte_hdr_h());
  hdr_pop();
  task_done(0);
}

/* token 从 Core Mem 搬到 Router，往下游发，scale 随它走 */
TASK void task_dte_move(void) {
  dte_move(CMEM_TOKEN_OFF, 0, E2E_TOKEN_BYTES,
           DTE_MODE_CMEM_TO_ROUTER | DTE_SCALE_VALID, 1);
  task_done(0);
}

/* 把 MU 算完的那一段发给下游 */
TASK void task_dte_send_fc1(void) {
  send_seg(CMEM_FC1_OFF, E2E_OUT_BYTES);
  task_done(0);
}

/* 把 VU 算完的那一段发给下游 */
TASK void task_dte_send_act(void) {
  send_seg(CMEM_ACT_OFF, E2E_ACT_BYTES);
  task_done(0);
}

/* ===== 一层 MoE 那一段 ===== */

/* 部分和出核，逐级 reduce：一包里两个专家的 FC1 与 FC3。链上每个 core 发的包头
 * 都写 dot core 上收归约结果的那一处，Router 归约时照抄首份分量的包头，链尾交回
 * dot core 的那一份就落在那里。这笔任务由 Router 报完成 */
TASK void task_dte_send_part(void) {
  dsa_write(DTE_ADDR0_SRC, dte_hdr_h());
  dte_fire(TPL_SEND_PART);
  task_done(0);
}

/* dot core：FC2 输入广播给本 chip 另外 7 个计算 core，scale 随它走，落在各自同一处 */
TASK void task_dte_send_fc2in(void) {
  dsa_write(DTE_ADDR0_SRC, dte_hdr_h());
  dte_fire(TPL_SEND_FC2IN);
  task_done(0);
}

/* 计算 core：把 FC2 第 s 段发给 dot core，落点是 concat 区第 s 段。按槽位变化，
 * 每个槽位一个入口 */
static void send_concat(u32 s) {
  dsa_write(DTE_ADDR0_SRC, dte_hdr_h());
  dsa_write(DTE_ADDR1_SRC, moe_concat(s));
  dsa_write(DTE_ADDR1_DST, moe_concat(s));
  dte_fire(TPL_SEND_CONCAT);
  task_done(0);
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

/* 不带 ack_ts_en 的那一档 TRANS_MODE：一个 task 发两笔时前一笔用它盖过模板 */
#define ROW_PART_MODE \
  (DTE_MODE_CMEM_TO_ROUTER | ((1u << 1) << DTE_SEG_VALID_SHIFT))

/* dot core：行链出核，逐级 reduce。一包是 16 B 头加 concat 区，落点 dst 由下一跳
 * 定：下一颗 chip 的 dot core 落它的 MOE_ROW_IN_OFF，本行 R core 落这个用户那一
 * 槽的前一半。行首只送本 core 那一包；其余几颗先送上一颗 chip 落在本 core 的那
 * 一包，再送本 core 那一包，两包作为本 core 的两个操作数进本级 Rmem 相加，两笔
 * 包头都写 dst，只有后一笔带 ack_ts_en。这笔任务由 Router 报完成 */
static void send_row(u32 add, u32 dst) {
  if (add) {
    dsa_write(DTE_ADDR0_SRC, dte_hdr_h());
    dsa_write(DTE_ADDR1_SRC, MOE_ROW_IN_OFF);
    dsa_write(DTE_ADDR1_DST, dst);
    dsa_write(DTE_TRANS_MODE, ROW_PART_MODE);
    dte_fire(TPL_SEND_ROW);
  }
  dsa_write(DTE_ADDR0_SRC, dte_hdr_h());
  dsa_write(DTE_ADDR1_DST, dst);
  dte_fire(TPL_SEND_ROW);
  task_done(0);
}
/* 行首，下一跳是本行 R core，或者这一行只有一颗 chip、结果从 E 口出去 */
TASK void task_dte_send_row(void) { send_row(0, rc_land(user_id(), 0)); }
/* 行首，下一跳是下一颗 chip 的 dot core */
TASK void task_dte_send_row_next(void) { send_row(0, MOE_ROW_IN_OFF); }
/* 不是行首，下一跳是本行 R core，或者这一行没有 R core、结果从 E 口出去 */
TASK void task_dte_add_row(void) { send_row(1, rc_land(user_id(), 0)); }
/* 不是行首，下一跳是下一颗 chip 的 dot core */
TASK void task_dte_add_row_next(void) { send_row(1, MOE_ROW_IN_OFF); }

/* 把一个已经 ready 的 user_id 写入软件用户 FIFO 的尾。队满就等链二把队头弹走。 */
static void rc_fifo_push(u32 u) {
  u32 t = smem_read(RC_READY_TAIL_OFF);
  u32 h = smem_read(RC_READY_HEAD_OFF);
  while (t - h >= RC_READY_CAP) {
    h = smem_read(RC_READY_HEAD_OFF);
  }
  smem_write(RC_READY_Q_OFF + (t & (RC_READY_CAP - 1u)) * 4u, u);
  smem_write(RC_READY_TAIL_OFF, t + 1u);
}

/* ===== R core 的两段 =====
 *
 * 进核那一笔的落点由包头 dst_addr 给（落哪一半是发方算好的），长度是定值
 * MOE_ROW_BYTES，照它配进核搬运，搬完由 Completion RS 往标志表写这一半的 valid。再按 user_id 把软件映射表
 * 加一，记下又来了一包。非链首加到两包、链首加到一包，就把这个用户写入 FIFO，
 * 每个用户只入队一次。不等数据落地：等标志置齐是链二弹出之后的事，这里等的话
 * DTE 的 RV core 在搬运期间什么也做不了。最后弹掉这个包的包头。user_id 就是槽
 * 号。 */
TASK void task_dte_rc_datain(void) {
  tpl_once();
  u32 u = user_id();
  u32 landing = hdr_dst_addr();
  dte_inbound(landing, MOE_ROW_BYTES,
              DTE_MODE_ROUTER_TO_MMEM | DTE_WR_SHAREMEM_FLAG,
              RC_FLAG_OFF + (landing / RC_HALF_BYTES) * 4u, 0, 0,
              dte_hdr_cmem(u));
  u32 n = smem_read(RC_MAP_OFF + u * 4u) + 1u;
  smem_write(RC_MAP_OFF + u * 4u, n);
  if (n == rc_need()) rc_fifo_push(u);
  hdr_pop();
  task_yield();
}

/* 不带 ack_ts_en 的那一档 TRANS_MODE：一个 task 发两笔时前一笔用它盖过模板 */
#define RC_PART_MODE \
  (DTE_MODE_MMEM_TO_ROUTER | ((1u << 1) << DTE_SEG_VALID_SHIFT))

/* 链二的第二步：把那个槽的两份从 Matrix Mem 作为本 core 的两个操作数送进本级
 * Rmem，Rmem 相加后直接发往下一行的 R core，落到那边这个用户那一槽的后一半，与
 * 收进来时同一个摆法。Rmem 的结果照抄首份操作数的包头，两笔包头都写这个落点。
 * 链首那一行没有上一行，只送本行那一份。只有最后一笔带 ack_ts_en。这笔任务由
 * Router 报完成 */
TASK void task_dte_rc_reduce(void) {
  u32 slot = smem_read(RC_SLOT_OFF + stream_id() * 4);
  u32 head = smem_read(RC_HEAD_OFF);
  u32 dst = rc_land(user_id(), 1);
  u32 hdr = dte_hdr_cmem(user_id());
  if (head == 0u) {
    dsa_write(DTE_ADDR0_SRC, hdr);
    dsa_write(DTE_ADDR1_SRC, dte_ep(DTE_EP_MMEM, rc_land(slot, 0)));
    dsa_write(DTE_ADDR1_DST, dst);
    dsa_write(DTE_TRANS_MODE, RC_PART_MODE);
    dte_fire(TPL_RC_SEND);
    dsa_write(DTE_ADDR1_SRC, dte_ep(DTE_EP_MMEM, rc_land(slot, 1)));
  } else {
    dsa_write(DTE_ADDR1_SRC, dte_ep(DTE_EP_MMEM, rc_land(slot, 0)));
  }
  dsa_write(DTE_ADDR0_SRC, hdr);
  dsa_write(DTE_ADDR1_DST, dst);
  dte_fire(TPL_RC_SEND);
  task_done(0);
}

/* B core 的链一：token 落进 Matrix Mem 的环形缓冲，落点按自己收下的笔数取模算，
 * scale 随它进 scale 旁带，topK 随它按 user_id 进 MU 的 topK 表，搬完置那一格的
 * valid。这里把这一格是哪个用户记下来，链二发的时候要按它认人。
 *
 * 记在第几格按自己收下的笔数算，与发方算落点用的是同一条规则 */
TASK void task_dte_bc_datain(void) {
  tpl_once();
  u32 n = smem_read(BC_RECV_OFF);
  /* 高位置 1：链二等得到“已经写过”，user_id 0 也能用 */
  smem_write(BC_USER_OFF + (n % BC_SLOTS) * 4, user_id() | 0x80000000u);
  smem_write(BC_RECV_OFF, n + 1);
  u32 landing = bc_land(n);
  dte_inbound(landing, BC_TOKEN_BYTES,
              DTE_MODE_ROUTER_TO_MMEM | DTE_SCALE_VALID | DTE_WR_SHAREMEM_FLAG |
                  DTE_TOPK_VALID,
              BC_FLAG_OFF + (landing / BC_TOKEN_BYTES) * 4u, user_id(), 0,
              dte_hdr_cmem(user_id()));
  hdr_pop();
  task_yield();
}

/* B core 的链二第二步：把上一步认下的那一格广播给本组各 core，scale 与 topK 随它走 */
/* 源在 Matrix Mem、带 scale 与 topK：数据与 scale 两段的源和落点按格子写，topK 按
 * 这一格的 user_id 从表里取，其余在模板里 */
static inline __attribute__((always_inline)) void bc_move(u32 src, u32 dst) {
  dsa_write(DTE_ADDR0_SRC, dte_hdr_cmem(user_id()));
  dsa_write(DTE_ADDR1_SRC, dte_ep(DTE_EP_MMEM, src));
  dsa_write(DTE_ADDR1_DST, dst);
  dsa_write(DTE_ADDR2_SRC, dte_ep(DTE_EP_SCALE, src & 0x0FFFFFFFu));
  dsa_write(DTE_ADDR2_DST, dte_ep(DTE_EP_SCALE, dst & 0x0FFFFFFFu));
  dsa_write(DTE_ADDR3_SRC, dte_ep(DTE_EP_TOPK, user_id()));
  dte_fire(TPL_BC_SEND);
}

TASK void task_dte_bc_send(void) {
  u32 slot = smem_read(BC_SLOT_OFF + stream_id() * 4);
  bc_move(bc_land(slot), MOE_TOKEN_OFF);
  task_done(0);
}

/* B core 的链二第三步：同一格再送一份给下一个 EP 组的 B core。落点要自己算好
 * 写进包头：收方那边按它收下的笔数取模找格子，两边同一条规则，所以这里按自
 * 己转出的笔数算 */
TASK void task_dte_bc_relay(void) {
  u32 slot = smem_read(BC_SLOT_OFF + stream_id() * 4);
  u32 n = smem_read(BC_SENT_OFF);
  smem_write(BC_SENT_OFF, n + 1);
  bc_move(bc_land(slot), bc_land(n));
  task_done(0);
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
              DTE_MODE_ROUTER_TO_MMEM | (scale ? DTE_SCALE_VALID : 0u), 0, 0, 0,
              dte_hdr_h());
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

/* 八套模板的常量字段。token 那一套整笔配，只有 topK 的表下标每笔写；不带 scale
 * 的进核那一套只配段长跨度与 TRANS_MODE，落点与长度每笔写；出核那几套按整笔配，
 * 随任务变的字段每笔再写一遍盖过去。 */
static void tpl_setup(void) {
  dte_fields(DTE_TPL(TPL_IN_TOKEN), 0, MOE_TOKEN_OFF, MOE_EMBED,
             DTE_MODE_ROUTER_TO_CMEM | DTE_SCALE_VALID | DTE_TOPK_VALID, 1, 0);
  dte_fields(DTE_TPL(TPL_IN_CM), 0, 0, 0, DTE_MODE_ROUTER_TO_CMEM, 1, 0);
  dte_fields(DTE_TPL(TPL_SEND_PART), MOE_PART_OFF, MOE_RED_OFF, MOE_PART_BYTES,
             DTE_MODE_CMEM_TO_ROUTER, 1, 0);
  dte_fields(DTE_TPL(TPL_SEND_FC2IN), MOE_ACT_OFF, MOE_ACT_OFF, MOE_ACT_BYTES,
             DTE_MODE_CMEM_TO_ROUTER | DTE_SCALE_VALID, 1, 0);
  dte_fields(DTE_TPL(TPL_SEND_ROW), MOE_ROW_OFF, 0, MOE_ROW_BYTES,
             DTE_MODE_CMEM_TO_ROUTER, 1, 0);
  dte_fields(DTE_TPL(TPL_SEND_CONCAT), 0, 0, MOE_FC2_BYTES,
             DTE_MODE_CMEM_TO_ROUTER, 1, 0);
  dte_fields(DTE_TPL(TPL_RC_SEND), 0, 0, MOE_ROW_BYTES,
             DTE_MODE_MMEM_TO_ROUTER, 1, 0);
  dte_fields(DTE_TPL(TPL_BC_SEND), 0, 0, BC_TOKEN_BYTES,
             DTE_MODE_MMEM_TO_ROUTER | DTE_SCALE_VALID | DTE_TOPK_VALID, 1, 0);
}

/* firmware 的入口还会调它；模型不跑 firmware，模板由 tpl_once 配 */
void kernel_init(void) {}

