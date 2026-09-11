"""把算出来的全套硬件配置写成一份 .bachir。

一行一条记录：记录名大写、空格分隔，地址写十六进制带前缀，其余十进制。逐行
可 diff，也能直接拿去跟硬件对表。装载的那一侧按记录名分发，不必知道每张表在
哪个地址。地址映射还没冻结，写进产物只会跟着它变。

版本 6 起产物装的是硬件配置（RTAB、TCHAIN、CFGMISC 这些）。此前几版装的是建模
抽象层的配置，路由直接写目的坐标，那一档现在是编译器的输入侧。版本 7 加了
RELROUTE（Release 静态路由，即 RTR_RELEASE_ROUTE）与 RTAB 末尾的 reduceNeedMask。
版本 8 的 TCHAIN 照 TASK_CHAIN_xx_PC / ATTR 的位域：TASK_TYPE 取代三个类型位，去掉
TASK_EXE_MASK 与每项的 SELF_START；CFGMISC 的第二个字段改成全局 SELF_START；加
TSRTAB（TS 的 ROUTER_TABLE）。PATHTASK 只给 DTE 那一份。
"""

HEADER = "BACHIR 8"


# 一层 MoE 那套拓扑的产物。core 一律按 (chip 号, 片内 core 号) 定位，与模型里
# 的编号同一套；装载那一侧按记录名分发。
UNIT_NAME = {"DTE": 0, "MU": 1, "VU": 2}
RECV_NAME = {"RV_ONLY": 0, "DSA": 1}
TASK_TYPE_NAME = {"NORMAL": 0, "REISSUE_OUT": 1, "B_REISSUE_IN": 2,
                  "P2P_REISSUE_IN": 3, "REDUCE": 4, "PID_UPDATE": 5}
ROLE_NAME = {"NORMAL": 0, "BROADCAST": 1, "REDUCTION": 2, "SPARE": 3}


def render_plan(plan, source, images):
    """把一份展开好的 Plan 写成 .bachir。"""
    out = [HEADER, f"SOURCE {source}", f"NAME {plan.name}"]

    out.append("# CHIP <chip> <形状 0 中间 1 第一列 2 最后一列>")
    for chip, shape in enumerate(plan.shapes):
        out.append(f"CHIP {chip} {shape}")

    out.append("# CORE <chip> <core> <角色 0 计算 1 广播 2 归约 3 不派>")
    for (chip, core), role in sorted(plan.role.items()):
        out.append(f"CORE {chip} {core} {ROLE_NAME[role]}")

    out.append("# CFGMISC <chip> <core> <stream_num> <self_start> <b_core_dir>"
               " <trigger_chain_en>")
    for chip, core in sorted(plan.chains):
        dirs = plan.bcast_dirs.get((chip, core), 0)
        out.append("CFGMISC {} {} {} {} 0x{:x} 1".format(
            chip, core, plan.stream_num, int(plan.self_start((chip, core))),
            dirs))

    out.append("# TCHAIN <chip> <core> <idx> <task_pc> <unit> <recv_unit>"
               " <wait_wake> <task_type> <p2p_reissue_tid> <credit_en>"
               " <path_id> <end>")
    for (chip, core), items in sorted(plan.chains.items()):
        for it in items:
            out.append(
                "TCHAIN {} {} {} 0x{:x} {} {} {} {} {} {} {} {}"
                .format(chip, core, it.idx, it.task_pc, UNIT_NAME[it.unit],
                        RECV_NAME[it.recv_unit], int(it.wait_wake),
                        TASK_TYPE_NAME[it.task_type], it.p2p_reissue_tid,
                        int(it.credit_en), it.path_id, int(it.end)))

    out.append("# TSRTAB <chip> <core> <pid> <dir bit0 上下 bit1 左 bit2 右"
               " bit3 本 core> <vcid>")
    for (chip, core), table in sorted(plan.ts_routes().items()):
        for pid, (d, vcid) in sorted(table.items()):
            out.append(f"TSRTAB {chip} {core} {pid} {d} {vcid}")

    out.append("# DATAIN <chip> <core> <task_pc> <weights_mode>")
    for (chip, core), pc in sorted(plan.datain_pc.items()):
        out.append(f"DATAIN {chip} {core} 0x{pc:x} 0")

    out.append("# RTAB <chip> <core> <path> <op_type> <flow_dir> <cur_vc>"
               " <nxt_vc×5> <mask_en> <mask_idx> <bypass> <need_buffer>"
               " <stream_tab_en> <cur_cr_type> <cur_cr_req> <nxt_cr_type×3>"
               " <nxt_cr_req×3> <rdc_dtype> <rdc_odtype> <rdc_in_mask>"
               " <operation> <stall_way> <ext_dst> <rdc_need>")
    for (chip, core), table in sorted(plan.entries.items()):
        for path_id, e in sorted(table.items()):
            nxt_vc = " ".join(str(e.vc) for _ in range(5))
            nxt_ct = " ".join(str(e.credit_type) for _ in range(3))
            nxt_cr = " ".join(str(e.credit_require) for _ in range(3))
            out.append(
                "RTAB {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {}"
                .format(chip, core, path_id, e.op_type, e.flow_dir, e.vc,
                        nxt_vc, int(e.mask_enable), e.mask_idx,
                        int(e.path_core_bypass), int(e.need_buffer),
                        int(e.stream_table_enable), e.credit_type,
                        e.credit_require, nxt_ct, nxt_cr, e.reduce_data_type,
                        e.reduce_outdata_type, e.reduce_in_mask, e.operation,
                        int(e.stall_way), e.ext_dst, int(e.reduce_need)))

    out.append("# RTABDTE <chip> <core> <path>　DTE 里那份副本，内容与 RTAB 同")
    for (chip, core), table in sorted(plan.dte_rtab.items()):
        for path_id in sorted(table):
            out.append(f"RTABDTE {chip} {core} {path_id}")

    out.append("# PATHTASK <chip> <core> <path> <task_id>　DTE 那一份 path_task_map")
    for (chip, core), table in sorted(plan.path_task.items()):
        for path_id, task_id in sorted(table.items()):
            out.append(f"PATHTASK {chip} {core} {path_id} {task_id}")

    out.append("# RELROUTE <chip> <core> <入口方向 0 mid 1 left 2 right>"
               " <出方向掩码 bit0 mid bit1 left bit2 right bit3 本级>")
    for (chip, core), table in sorted(plan.release_route.items()):
        for in_dir, mask in sorted(table.items()):
            out.append(f"RELROUTE {chip} {core} {in_dir} {mask}")

    out.append("# KERNEL <rv_core 0 DTE 1 MU 2 VU> <镜像>")
    for kind, name in images:
        out.append(f"KERNEL {UNIT_NAME[kind.upper()]} {name}")
    return out
