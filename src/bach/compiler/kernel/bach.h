/* Bach core 内 RV core 看得见的地址与寄存器。
 *
 * 每一组都照原始设计文档，出处写在各组上方。IO 基址与
 * compiler/hwconfig/layout.py 的 IP_OFFSET 同源，改一处要一起改。
 */

#ifndef BACH_KERNEL_H
#define BACH_KERNEL_H

typedef unsigned int u32;
typedef unsigned long long u64;

/* 各 DSA 的 IO reg 基址，core 内偏移。跨度按各 IP 文档的最大偏移留 */
#define DTE_IO_BASE 0x00008000u
#define MU_IO_BASE  0x00009000u
#define VU_IO_BASE  0x0000C000u

/* ===== DTE：《DTE寄存器配置参数》§DTE地址空间和寄存器配置 ===== */

/* 地址空间七段，照 §寄存器地址空间分配 */
#define DTE_CTRL_STATUS_BASE 0x000  /* 全局控制、状态、错误、性能、调试 */
#define DTE_ISSUE_BASE       0x100  /* 运行时动态字段、trigger/doorbell */
#define DTE_TEMPLATE_BASE    0x200  /* template[0..3]，每套 64 B 对齐 */
#define DTE_TASKQ_BASE       0x300  /* taskQ shadow/debug，深度 16 */
#define DTE_LUT_BASE         0x600  /* path_id_table / task_len_table */
#define DTE_HMEM_BASE        0x800  /* core_mask_table / sw_header_table */
#define DTE_RESERVED_BASE    0xC00

#define DTE_TEMPLATE_STRIDE  0x40

/* 一套模板内十一项的偏移，照 §DTE task寄存器配置信息 */
#define DTE_SRC_ADDR       0x00
#define DTE_DST_ADDR       0x04
#define DTE_STREAM_STRIDE  0x08
#define DTE_SCALE_ADDR     0x0C
#define DTE_TOPK_ADDR      0x10
#define DTE_HW_HEADER_ADDR 0x14
#define DTE_SW_HEADER_ADDR 0x18
#define DTE_SHAREMEM_WADDR 0x1C
#define DTE_SHAREMEM_WDATA 0x20
#define DTE_DATA_LEN       0x24
/* CFG_DATA_LEN 是 16 bit，一格 8 B */
#define DTE_DATA_LEN_GRAIN 8u
#define DTE_TRIGGER        0x28   /* transfer_mode/task_trigger，必须最后写 */

/* transfer_mode/task_trigger 的字段，照 §`transfer_mode/task_trigger` 字段建议 */
#define DTE_MODE_ROUTER_TO_CMEM 0u  /* 000 */
#define DTE_MODE_ROUTER_TO_MMEM 1u  /* 001 */
#define DTE_MODE_CMEM_TO_ROUTER 2u  /* 010 */
#define DTE_MODE_MMEM_TO_ROUTER 3u  /* 011 */
#define DTE_MODE_MMEM_TO_CMEM   4u  /* 100 */
#define DTE_EP_COUNT_SHIFT   3
#define DTE_SCALE_VALID      (1u << 11)
#define DTE_TOPK_VALID       (1u << 12)
#define DTE_HW_HEADER_OP     (1u << 13)
#define DTE_WR_SHAREMEM_FLAG (1u << 14)
#define DTE_TASK_LAST        (1u << 15)

/* ===== Core Mem 上一个 stream 的分区 =====
 *
 * 硬件按 stream_num 把 Core Mem 均等切分，一个 stream 一片；片内各段的起点与
 * 长度由模型的 shape 决定，编译期就定了。DTE、MU、VU 三份 kernel 用同一套，
 * 改一处要一起改。 */

#define CMEM_STREAM_BASE   0x00000u
#define CMEM_STREAM_STRIDE 0x10000u

#define CMEM_TOKEN_OFF 0x0000u   /* datain 的落点 */
#define CMEM_FC1_OFF   0x8000u   /* MU 算完的中间结果 */
#define CMEM_ACT_OFF   0xC000u   /* VU 算完的结果 */

/* 一条 MU 原语的形状是 K=256 配 N=32：BF16 的 token 512 B，FP32 的结果 128 B */
#define TOKEN_BYTES 512u
#define FC1_BYTES   128u
#define ACT_BYTES   128u

/* ===== 一层 MoE 那一段的形状与摆放 =====
 *
 * EP6+TP8 切完之后每个 core 拿到的分片。K 走完整的 embedding，中间维是 2048
 * 按 TP8 切下来的那一段，输出维占一个 tile_N。
 * 与 reference/vectors.py 的 MOE_* 同源，改一处要一起改。 */

#define MOE_K       6144u  /* 这个 core 分到的 embedding 那一段 */
#define MOE_INTER   256u   /* 这个 core 分到的 intermediate 那一段 */
#define MOE_OUT_N   6144u  /* 这个 core 要产出的 embedding 那一段 */
#define MOE_EXPERTS 2u     /* 这个 token 在本 EP Group 内激活了几个专家 */

/* MU 物理阵列的一笔原语是 1×K256×N32。K 与 N 都按这个尺寸切块，一笔任务走
 * kblock × 专家数 × nblock 遍 */
#define MU_TILE_K 256u
#define MU_TILE_N 32u
#define MOE_KBLOCK_FC13 (MOE_K / MU_TILE_K)
#define MOE_NBLOCK_FC13 (MOE_INTER / MU_TILE_N)
#define MOE_KBLOCK_FC2  (MOE_INTER / MU_TILE_K)
#define MOE_NBLOCK_FC2  (MOE_OUT_N / MU_TILE_N)
/* 一个权重块：K256 × N32 个 BF16。权重按 N 块连着放，一个 N 块占 kblock 个 */
#define MU_TILE_BYTES   (MU_TILE_K * MU_TILE_N * 2u)

/* Core Mem 上一个 stream 里的摆放。FC1、FC3、激活三段按专家隔开。token 占
 * MOE_K 个 BF16，整段要放得进 CMEM_STREAM_STRIDE */
#define MOE_TOKEN_OFF     0x0000u
#define MOE_TOPK_OFF      0x3000u
#define MOE_FC1_OFF       0x3800u
#define MOE_FC3_OFF       0x5800u
#define MOE_ACT_OFF       0x7800u
#define MOE_OUT_OFF       0x9800u
#define MOE_EXPERT_STRIDE 0x1000u

/* FC2 的结果出核时拆成 MOE_PIECE_NUM 个 reduce 包。一包最长只支持到
 * (16 K + 32) B（dte.md F69），整份 MOE_OUT_N 个 FP32（24 KiB）装不下；
 * ReduceModule 给一个用户留 32 KiB（按 FP32 驻留算，03-reduce 第二层那条对
 * 软件的硬约束），一包累加出来也不能超过它。照这两条约束按 8 KB 数据一包拆，
 * 任务链上跟着配几项逐级 reduce 任务（task_dte_send_moe_p0～p2），一项一包。
 *
 * 每包最前面 16 B 是软件辅助信息，Router 做加法时每包都跳过这一段。所以 Core
 * Mem 里一包占一格：格首 16 B 留给这一包的头，MU 把这一段结果写在它之后，DTE
 * 从格首发整包。格距按 128 B 对齐，因为收方按 128 B 对齐检查落点；一包落到
 * R core 那边也用同一个格距。 */
#define MOE_SW_HEAD_BYTES 16u
#define MOE_PIECE_DATA    8192u                                 /* 一包的数据 */
#define MOE_PIECE_NUM     (MOE_OUT_N * 4u / MOE_PIECE_DATA)     /* 拆成几包 */
#define MOE_PIECE_N       (MOE_PIECE_DATA / 4u)                 /* 一包几个 FP32 */
#define MOE_PIECE_NBLOCK  (MOE_PIECE_N / MU_TILE_N)             /* 一包几个 N 块 */
#define MOE_PIECE_BYTES   (MOE_SW_HEAD_BYTES + MOE_PIECE_DATA)  /* 一包的长度 */
#define MOE_PIECE_STRIDE  0x2080u                               /* 一格 */

_Static_assert(MOE_PIECE_NUM * MOE_PIECE_DATA == MOE_OUT_N * 4u,
               "FC2 的结果要正好拆成整数包");
_Static_assert(MOE_PIECE_STRIDE >= MOE_PIECE_BYTES && MOE_PIECE_STRIDE % 128u == 0,
               "一格要装得下一包，并且按 128 B 对齐");
_Static_assert(MOE_OUT_OFF + MOE_PIECE_NUM * MOE_PIECE_STRIDE <= CMEM_STREAM_STRIDE,
               "出核那几格要放得进一个 stream 的 Core Mem");

/* ===== R core：EP 组间那一层求和 =====
 *
 * 每个用户在本 core 占一个槽，槽里两半：本组 TP 内归约出来的组内结果落前一半，
 * 上游组送过来的中间结果落后一半。落哪一半由发方在包头的 dst_addr 里指定，槽号
 * 两个发方都按 user_id 取模算，算得一样。
 *
 * 一笔是 MOE_PIECE_NUM 包，一半里一包一格，格距与 Core Mem 那边相同。每包搬完
 * 硬件就把这一包的 valid 标志置起来；链二扫到一个槽两半的标志都齐了，就把整个
 * 槽搬进 Core Mem 求和。 */
#define RC_SLOTS       16u
/* 一笔占的地方：MOE_PIECE_NUM 格 */
#define RC_HALF_BYTES  (MOE_PIECE_NUM * MOE_PIECE_STRIDE)
#define RC_SLOT_BYTES  (2u * RC_HALF_BYTES)        /* 一个用户占的地方 */
#define RC_MM_BASE     0x000000u                   /* 槽在 Matrix Mem 的起点 */

/* Share Mem 里的两张表。标志表每包一项：硬件按落点除以格距找项，搬完一包写一
 * 项，所以第 s 槽第 h 半第 k 包那一项是 (s × 2 + h) × MOE_PIECE_NUM + k。用户
 * 表每槽一项，由 datain 那一段写，链二按它认这个槽是哪个用户 */
#define RC_FLAG_OFF    0x0000u
#define RC_USER_OFF    0x0200u
#define RC_SLOT_OFF    0x0300u                     /* 链二每 stream 记一个槽号 */
/* 本 core 是不是这条 R core 链的链首。链首那一组只有自己的组结果，槽的另一半
 * 一直是 0，加上去不改值，所以它等一笔就走；其余组等两笔。非零表示是链首 */
#define RC_HEAD_OFF    0x0380u

_Static_assert(RC_FLAG_OFF + RC_SLOTS * 2u * MOE_PIECE_NUM * 4u <= RC_USER_OFF,
               "标志表每包一项，要放得进用户表之前那一段");

/* Core Mem 里的摆放。两笔在 Matrix Mem 里按格连着，搬到 Core Mem 也原样连着，
 * 一笔搬运就够。每格首那 16 B 是软件辅助信息，求和逐格只算数据那一段。求和结果
 * 写回第一笔的同一格：VU 读完两边那一格才写，写的时候第一笔那一格已经进了 VRF；
 * 格首那 16 B 原样留着，DTE 从格首发整包 */
#define RC_A_OFF   0x0000u
#define RC_B_OFF   RC_HALF_BYTES
#define RC_SUM_OFF RC_A_OFF
/* 两笔求和借用的 VRF 起点。逐格相加，一条 VL 为 MOE_PIECE_N 的 FP32 向量占
 * MOE_PIECE_N × 4 / 128 个 entry */
#define RC_VRF     16u

/* 本组把结果送到 R core 的哪一半：这个用户在本组之前还有别的组时送前一半，
 * 本组是它的第一组时直接送下一组的后一半。boot 期由编译侧写进 Share Mem */
#define MOE_SEND_HALF_OFF 0x0400u

/* 出核那几格里第 k 格在本 stream 里的起点 */
static inline u32 moe_piece(u32 k) { return MOE_OUT_OFF + k * MOE_PIECE_STRIDE; }

/* 一个用户在 R core 上的落点。half 为 0 是本组结果，为 1 是上游组的中间结果 */
static inline u32 rc_land(u32 user, u32 half) {
  return RC_MM_BASE + (user % RC_SLOTS) * RC_SLOT_BYTES + half * RC_HALF_BYTES;
}

/* ===== B core：组内广播的发起点 =====
 *
 * 上游把 token 一笔笔送进来，硬件按包头的落点搬进 Matrix Mem 的环形缓冲，搬完
 * 置那一格的 valid。收发两条链靠 Share Mem 里一对 head 与 tail 指针耦合：
 *
 *   链一  datain。只把这一笔是哪个用户记进 Share Mem，搬运与置 valid 都是硬件
 *         的事
 *   链二  自启动。VU 查 tail 那一格的 valid，置起来了就把 tail 推一格；head 与
 *         tail 不相等就认下 head 那一格：清掉它的 valid、推一格 head、记下槽
 *         号，再交给 DTE 从 Matrix Mem 广播给本组各 core
 *
 * 认下那一格的几步全在 VU 那一步做完，不留到 DTE 那一步：一个 core 上几条链并
 * 行跑，划晚了另一条链会认到同一格，同一笔发两遍。
 *
 * 槽号一律按“第几笔”取模算：发方按自己送出的笔数算落点，本 core 的 datain 按
 * 自己收下的笔数记用户，链二的 tail 也是笔数。三处同一条规则，所以 user_id 取
 * 什么值都不影响落点，只要包按序送进来。
 *
 * 一份 token 在 B core 上发两笔：一笔广播给本组各 core，落点由收方自己的配置
 * 定；一笔转给下一个 EP 组的 B core，落点要按这一条规则算好写进包头 */
#define BC_SLOTS       16u
#define BC_TOKEN_BYTES (MOE_K * 2u)                /* 一笔 token：K 个 BF16 */
#define BC_MM_BASE     0x000000u

/* Share Mem 里的几样。标志表由硬件搬完之后写，用户表与收包计数由链一写，一对
 * 指针与槽号由链二写 */
#define BC_FLAG_OFF    0x0500u
#define BC_USER_OFF    0x0540u
#define BC_HEAD_OFF    0x0580u
#define BC_TAIL_OFF    0x0584u
#define BC_SLOT_OFF    0x0588u                     /* 链二每 stream 记一个槽号 */
#define BC_RECV_OFF    0x05C8u                     /* 链一收下的笔数 */
#define BC_SENT_OFF    0x05CCu                     /* 往下一组转出的笔数 */

/* weights 加载阶段搬进本 core 的笔数。数满了 loader 中断 SCP，SCP 才把这颗
 * core 切到业务模式 */
#define WEIGHTS_CNT_OFF 0x05D0u

/* 第几笔落在 Matrix Mem 的哪里 */
static inline u32 bc_land(u32 idx) {
  return BC_MM_BASE + (idx % BC_SLOTS) * BC_TOKEN_BYTES;
}

/* Matrix Mem 上三个矩阵各一段，段内按专家在本组内的序号隔开。W1 与 W3 各是
 * MOE_K × MOE_INTER 个 BF16，一个专家 3 MB */
#define MMEM_W1_OFF          0x000000u
#define MMEM_W3_OFF          0x600000u
#define MMEM_W2_OFF          0xC00000u
#define MMEM_EXPERT_STRIDE   0x300000u

static inline u32 dte_template(u32 idx) {
  return DTE_TEMPLATE_BASE + idx * DTE_TEMPLATE_STRIDE;
}

/* ===== MU：《Matrix Unit DSA》§Register Map Overview ===== */

#define MU_SYS_CTRL      0x000
#define MU_SYS_STATUS    0x004
#define MU_TASK_CFG      0x008
#define MU_TASK_BLOCK    0x00C
#define MU_ADDR_TOKEN    0x010
#define MU_ADDR_WEIGHT   0x014
#define MU_ADDR_SCALE    0x018
#define MU_ADDR_OUT      0x01C
#define MU_EXCEPT_STATUS 0x020
#define MU_EXCEPT_MASK   0x024

/* 身份三项。《Matrix Unit DSA》里没有这一组：MU 的 dsa_done 要填 stream_id 与
 * task_id，而 MU 没有从 RV core 直连过来的身份信号，只能由软件写进来。地址按
 * 建模计划的临时映射排在异常那一组之后，与 src/bach 的 mu/regfile.h 同源 */
#define MU_STREAM_ID     0x050
#define MU_TASK_ID       0x054
#define MU_USER_ID       0x058
#define MU_TOPK_STRIDE   0x05C
#define MU_TOPK_ADDR     0x048

/* 多专家那一组。原文给的是名字（AC_expert_stride、B_expert_stride，以及
 * primitive_mode 里的 router_expert_count 与 router_ep_reduce_en），地址同样
 * 无着落，按同一条临时映射往后排 */
#define MU_AC_EXPERT_STRIDE 0x060
#define MU_B_EXPERT_STRIDE  0x064
#define MU_EP_CTRL          0x068
/* EP_CTRL 位域：[7:0] router_expert_count、[8] router_ep_reduce_en */
#define MU_EP_REDUCE_EN (1u << 8)

/* SYS_CTRL 位域 */
#define MU_TASK_START  (1u << 0)   /* 写 1 启动，硬件接收后自清零 */
#define MU_SOFT_RESET  (1u << 1)
#define MU_CLK_GATE_EN (1u << 2)

/* SYS_STATUS 位域 */
#define MU_BUSY        (1u << 0)
#define MU_STATE_SHIFT 1           /* [2:1] 00 Idle、01 Running、10 Error */

/* TASK_CFG 位域 */
#define MU_PRIM_TYPE_K256_N32 0u   /* [0] 0 = 1*K256*N32 */
#define MU_PRIM_TYPE_K128_N64 1u
#define MU_VLANE_SHIFT   1         /* [2:1] 00 vlane=1、01 vlane=2 */
#define MU_DTYPE_AB_SHIFT 3        /* [4:3] 00 BF16、01 MXFP8、10 MXFP4 */
#define MU_DTYPE_C_BF16  (1u << 5) /* [5] 0 = FP32、1 = BF16 */

/* TASK_BLOCK 位域：[15:0] KBLOCK、[31:16] NBLOCK */
#define MU_NBLOCK_SHIFT 16

/* ===== VU：《VU-DSA 寄存器整理》§Register Map Overview ===== */

#define VU_DYNAMIC_BASE 0x0000
#define VU_STATIC_BASE  0x1000     /* 静态配置组 N*0x100 + 0x1000，N=0..7 */
#define VU_STATIC_STRIDE 0x100
#define VU_REGFILE_BASE 0x2000
#define VU_STATUS_BASE  0x3000
#define VU_PROFILE_BASE 0x4000

/* 动态参数寄存器，照 §Register Descriptions */
#define VU_MACRO_INST_TRIGGER 0x0000
#define VU_TYPE_VL            0x0004
#define VU_LD_ADDR            0x0008
#define VU_ST_ADDR            0x000C

/* 状态区：在飞的宏指令还剩几条。一个 task 发了几条时软件轮询它收尾 */
#define VU_MACRO_INST_LEFT    (VU_STATUS_BASE + 0x00)

/* macro_inst_trigger 位域 */
/* [7:0] STATIC_DYNAMIC_MASK：每位选一个参数取动态副本还是静态组里的那一份。
 * 位为 1 走动态。各 *_op / mask_op / PRF_op 没有动态副本，不在这里 */
#define VU_STATIC_DYNAMIC_MASK_SHIFT 0
#define VU_MASK_TYPE_VL  (1u << 0)
#define VU_MASK_LD_ADDR  (1u << 1)
#define VU_MASK_ST_ADDR  (1u << 2)
#define VU_MASK_VRF_RD   (1u << 3)
#define VU_MASK_VRF_WT   (1u << 4)
#define VU_MASK_MRF      (1u << 5)
#define VU_MASK_SRF      (1u << 6)
#define VU_CONFIG_IDX_SHIFT          8   /* [10:8] 用第几组静态配置 */
#define VU_EVENT_EN                  (1u << 16)
#define VU_STREAM_ID_OVERRIDE        (1u << 17)
#define VU_STREAM_ID_SHIFT           18  /* [21:18] */
#define VU_MACRO_INST_FENCE          (1u << 24)
#define VU_DATA_BROADCAST            (1u << 25)

/* TYPE_VL 位域：[15:0] VL、[16] DATA_TYPE、[19:17] ROUND_MODE */
#define VU_DATA_TYPE_SHIFT  16
#define VU_ROUND_MODE_SHIFT 17

/* 静态配置组内的偏移。前一段是各执行单元的 op 与 PRF_op，后一段是动态参数
 * 寄存器的静态副本，副本区起点是 VU_STATIC_DUP */
#define VU_LU_OP    0x00
#define VU_SU_OP    0x04
#define VU_VALU0_OP 0x08
#define VU_VALU1_OP 0x0C
#define VU_VSFU_OP  0x14
#define VU_MASK_OP  0x28
#define VU_PRF_OP   0x2C
#define VU_STATIC_DUP 0x2C

/* 动态参数寄存器里另外两项，静态副本在 VU_STATIC_DUP 加这个偏移 */
#define VU_VRF_RD_INDEX 0x10
#define VU_VRF_WT_INDEX 0x14

/* 各 op 寄存器的字段：OPCODE 与三路源选择各占一个字节 */
#define VU_SRC1_SHIFT 8
#define VU_SRC2_SHIFT 16

/* 源选择：高 4 位是来源类别 */
#define VU_SRC_LU      0x01
#define VU_SRC_VALU0   0x02
#define VU_SRC_VSFU    0x05
#define VU_SRC_VRF_P0  0x30

/* 用到的 opcode */
#define VU_LU_LD_FP32   0x04
#define VU_SU_NOP       0x00
#define VU_SU_ST_BF16   0x03
#define VU_VALU_FMUL_VV 0x06
#define VU_VALU_FADD_VV 0x01
#define VU_SU_ST_FP32   0x04
#define VU_VSFU_SIGMOID 0x04

static inline u32 vu_static_group(u32 idx) {
  return VU_STATIC_BASE + idx * VU_STATIC_STRIDE;
}

/* Share Mem：三个 RV core 都读写得了，DTE 搬完一笔之后也往这里写标志 */
#define SMEM_BASE 0x00040000u

/* ===== task 控制区 ===== */
/* 《寄存器描述》RV Core 页给了「自定义 task 信息寄存器」三项的位宽：
 * stream_id 4 位、task_id 6 位、user_id 16 位；CSR 地址一列是空的。
 *
 * 硬件上这几样是自定义 CSR 加 custom-0 的 task_done 指令。模型里走 MMIO：
 * rv32 的功能模型认标准 RV32IM，加一条自定义指令要动 codegen 出来的解码表；
 * 走约定地址读写，行为等价，而且配 DSA 寄存器本来就是 store。 */
#define TASK_CTRL_BASE 0x00030000u
#define TC_STREAM_ID     0x00   /* 只读，4 位 */
#define TC_TASK_ID       0x04   /* 只读，6 位 */
#define TC_USER_ID       0x08   /* 只读，16 位 */
/* TC_USER_ID 可读写：普通计算 core 上 TS 下发 task 时硬件写进来，B core 与
 * R core 上软件认出这一笔属于哪个用户之后自己写 */
#define TC_TASK_DONE     0x10   /* 写 1 通知 TS，写 0 不通知 */
#define TC_WAIT_TASK     0x14
/* 当前任务的 PID：TS 随任务送来，可读写。PID 更新任务把新值写进来，随 task_done
 * 回 TS */
#define TC_PATH_ID       0x18

static inline void mmio_write(u32 base, u32 off, u32 val) {
  *(volatile u32 *)(base + off) = val;
}

static inline u32 mmio_read(u32 base, u32 off) {
  return *(volatile u32 *)(base + off);
}

static inline u32 smem_read(u32 off) { return mmio_read(SMEM_BASE, off); }
static inline void smem_write(u32 off, u32 v) { mmio_write(SMEM_BASE, off, v); }

/* Router I/O reg：DTE core 那一段 Core Mem 往后 1 MB 起，映射到 CoreStation 的
 * 包头队列。读队头那个包的包头字段，写 ROUTER_HDR_POP 把它弹出，下一个包头映射
 * 上来。一个进核的包对应一笔 datain 任务，那一笔做完弹它自己的包头 */
#define ROUTER_IO_BASE 0x00180000u
#define ROUTER_HDR_POP 32u
static inline void hdr_pop(void) { mmio_write(ROUTER_IO_BASE, ROUTER_HDR_POP, 1); }

/* 当前 task 的身份，硬件随任务下发写进来 */
static inline u32 stream_id(void) { return mmio_read(TASK_CTRL_BASE, TC_STREAM_ID); }
static inline u32 task_id(void)   { return mmio_read(TASK_CTRL_BASE, TC_TASK_ID); }
static inline u32 user_id(void)   { return mmio_read(TASK_CTRL_BASE, TC_USER_ID); }
static inline u32 path_id(void)   { return mmio_read(TASK_CTRL_BASE, TC_PATH_ID); }
static inline void set_path_id(u32 v) { mmio_write(TASK_CTRL_BASE, TC_PATH_ID, v); }
/* 自启动的 B / R core：软件认出这一笔属于哪个用户后写回来 */
static inline void set_user_id(u32 v) {
  mmio_write(TASK_CTRL_BASE, TC_USER_ID, v);
}

/* 任务做完通知 TS，带 stream_id、task_id、user_id */
static inline void task_done(void) {
  mmio_write(TASK_CTRL_BASE, TC_TASK_DONE, 1);
}

/* firmware 结束时的那一条：停下等业务流 task，不通知 TS */
static inline void wait_for_task(void) {
  mmio_write(TASK_CTRL_BASE, TC_TASK_DONE, 0);
}

/* 交还自己但不通知 TS。自启动 core 上 Bypass 那一路 datain 与权重加载用它：
 * 这两类不占 stream 表项，完成不回 TS */
static inline void task_yield(void) {
  mmio_write(TASK_CTRL_BASE, TC_TASK_DONE, 0);
}

#endif
