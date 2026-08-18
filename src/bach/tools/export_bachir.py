#!/usr/bin/env python3
"""把一张 Bach 的 Map 编译成中间文件。

编译规则不在这里重写，直接用 bach_snapshot/ 里那份从 Bach 拷来的编译链：
TopologyParser 解析 schema，map_phaser.build_software_tables() 把 timeline 展开成
每个 Core 的任务表与 credit 表。这个脚本只负责把编译产物按中间文件格式写出来。

格式定义见 doc/bach/intermediate_format.md。

用法：

    python3 src/bach/tools/export_bachir.py \\
        --map /path/to/bach_topology.json \\
        --users 1 --streams 1 \\
        --out build/bach/bach_topology.bachir
"""

import argparse
import datetime
import hashlib
import sys
from pathlib import Path

# 快照目录要先于任何同名模块进 sys.path
SNAPSHOT_DIR = Path(__file__).resolve().parent / "bach_snapshot"
sys.path.insert(0, str(SNAPSHOT_DIR))

from config import GLOBAL_CONFIG as CFG  # noqa: E402
import map_phaser as phaser  # noqa: E402
from map_tools.chip_rule_utils import parse_chip_local_core_rule  # noqa: E402

FORMAT_VERSION = 5

# Bach 的配置属性名到中间文件 PARAM 名字的对应。PARAM 名字必须与
# src/bach/common/params.h 的字段名一致，解析侧认不出的名字会直接报错。
PARAM_MAP = [
    ("_TS_LOGIC_TIME", "ts_logic_time"),
    ("_DTE_SETUP_TIME", "dte_setup_time"),
    ("_MU_SETUP_TIME", "mu_setup_time"),
    ("_VU_SETUP_TIME", "vu_setup_time"),
    ("SETUP_AHEAD_DEPTH", "setup_ahead_depth"),
    ("_CM_ARB_DELAY", "cm_arb_delay"),
    ("_MM_ARB_DELAY", "mm_arb_delay"),
    ("NOC_BASIC_ROUTER_DELAY", "noc_router_delay"),
    ("_NOC_WIRE_DELAY", "noc_wire_delay"),
    ("_NOC_ACCESS_DELAY", "noc_access_delay"),
    ("CROSS_CHIP_DELAY", "cross_chip_delay"),
    ("CROSS_NODE_DELAY", "cross_node_delay"),
    ("HOST_PUSH_DELAY", "host_push_delay"),
    ("DTE_REDUCE_TIME", "dte_reduce_time"),
    ("NOC_BANDWIDTH", "noc_bandwidth"),
    ("PCIE_BANDWIDTH", "pcie_bandwidth"),
    ("DTE_BANDWIDTH", "dte_bandwidth"),
    ("_IF_DTE_CM", "if_dte_cm"),
    ("_IF_DTE_MM", "if_dte_mm"),
    ("_IF_VU_CM", "if_vu_cm"),
    ("_IF_MU_CM", "if_mu_cm"),
    ("_IF_MU_MM", "if_mu_mm"),
    ("STREAM_COUNT", "stream_count"),
    ("MATRIX_FIFO_CREDIT", "matrix_fifo_credit"),
    ("NUM_USERS", "num_users"),
    ("WATCHDOG_LIFESPAN_LIMIT", "watchdog_lifespan"),
    ("HOP_COUNT_WAR", "hop_count_warn"),
    ("HOP_COUNT_ERR", "hop_count_err"),
]

# NOC_BASIC_ROUTER_DELAY 没配时 Router 每跳取 1
PARAM_FALLBACK = {"noc_router_delay": 1}

# 中间文件格式认得的任务元数据键。遇到不在这里的键就停，不静默丢，
# 因为元数据决定运行时分支，丢一个就等于两边跑的不是同一份任务表。
KNOWN_TMETA_KEYS = {
    "recv_init",
    "no_credit_return",
    "local",
    "local_compute",
    "res_sum_local",
    "semantic_op",
    "require_dynamic_hitmap",
    "dynamic_hitmap",
    "hitmap_source",
    "dsa_route",
    "dsa_local_transfer",
    "source_endpoint",
    "destination_endpoint",
    "wire_tag",
    "payload_role",
}

DIRECTIONS = ("LEFT", "RIGHT", "TOP", "BOTTOM")


def sha256_of_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def snapshot_fingerprint():
    """快照目录里全部 .py 的合成指纹，用来记住是哪一份编译链产出的。"""
    files = sorted(p for p in SNAPSHOT_DIR.rglob("*.py") if "__pycache__" not in p.parts)
    h = hashlib.sha256()
    for p in files:
        h.update(p.relative_to(SNAPSHOT_DIR).as_posix().encode())
        h.update(sha256_of_file(p).encode())
    return h.hexdigest()


def opcode_name(op):
    name = op.name
    return "DONTCARE" if name.lstrip("_") == "DONTCARE" else name


def bool_to_int(value):
    if isinstance(value, bool):
        return 1 if value else 0
    return value


def emit_meta(out, map_path):
    out.append(f"BACHIR {FORMAT_VERSION}")
    out.append("")
    out.append(f"META map_path {map_path}")
    out.append(f"META map_sha256 {sha256_of_file(map_path)}")
    out.append(f"META compiler_sha256 {snapshot_fingerprint()}")
    out.append(f"META generated_at {datetime.datetime.now().isoformat(timespec='seconds')}")
    out.append("META generator src/bach/tools/export_bachir.py")
    out.append("")


def emit_params(out):
    for attr, name in PARAM_MAP:
        value = getattr(CFG, attr, None)
        if value is None:
            value = PARAM_FALLBACK.get(name)
        if value is None:
            continue
        out.append(f"PARAM {name} {int(value)}")
    out.append(f"MODE dte_execution_mode {str(CFG.DTE_EXECUTION_MODE).strip().lower()}")
    out.append(f"MODE dte_dsa_mode {str(CFG.DTE_DSA_MODE).strip().lower()}")
    out.append("")


def emit_dim(out):
    out.append(
        "DIM {} {} {} {} {} {}".format(
            CFG.CHIP_ROWS,
            CFG.CHIP_COLS,
            CFG.CORE_ROWS_PER_CHIP,
            CFG.CORE_COLS_PER_CHIP,
            CFG.NODE_CHIP_ROWS,
            CFG.NODE_CHIP_COLS,
        )
    )
    out.append("")


def emit_cores(out, table):
    parser = CFG.MAP_PARSER
    cores_cfg = parser.get_all_cores_config()
    for core_id in sorted(table):
        cfg = cores_cfg.get(str(core_id)) or {}
        group_id = cfg.get("group_id")
        group_id = -1 if group_id is None else int(group_id)
        core_type = parser.get_core_type(core_id).name
        out.append(f"CORE {core_id} {group_id} {core_type}")
    out.append("")


def emit_tasks(out, table, credit_table):
    for core_id in sorted(table):
        entries = table[core_id]
        core_credit = credit_table.get(core_id, {})
        for task_id, entry in enumerate(entries):
            unit, opcode, tag, up_cid, down_cid, dst_coords, time_or_vol = entry
            dst_row, dst_col = dst_coords
            out.append(
                "TASK {} {} {} {} {} {} {} {} {} {}".format(
                    core_id,
                    task_id,
                    unit.name,
                    opcode_name(opcode),
                    int(tag),
                    int(up_cid),
                    down_cid,
                    int(dst_row),
                    int(dst_col),
                    int(time_or_vol),
                )
            )

            meta = phaser.get_task_metadata(core_id, task_id)
            for key in sorted(meta):
                if key not in KNOWN_TMETA_KEYS:
                    raise SystemExit(
                        f"任务元数据出现未知键 {key!r}（core {core_id} task {task_id}）。"
                        " 先把它写进中间文件格式，再改这里的 KNOWN_TMETA_KEYS。"
                    )
                value = bool_to_int(meta[key])
                if value is None or value is False:
                    continue
                out.append(f"TMETA {core_id} {task_id} {key} {value}")

            targets = core_credit.get(task_id)
            if targets:
                joined = " ".join(str(t) for t in targets)
                out.append(f"CREDIT {core_id} {task_id} {joined}")

            skip_src = phaser.get_skip_source_metadata(core_id, task_id)
            if skip_src:
                groups = " ".join(str(g) for g in skip_src.get("sender_group_ids", ()))
                line = "SKIPSRC {} {} {} {} {}".format(
                    core_id,
                    task_id,
                    skip_src["sender"],
                    skip_src["phase_type"],
                    skip_src["phase_idx"],
                )
                out.append(line + (f" {groups}" if groups else ""))
        out.append("")


def emit_ext_nodes(out):
    for coord in sorted(CFG.EXTERNAL_NODES):
        node = CFG.EXTERNAL_NODES[coord]
        out.append(
            "EXTNODE {} {} {} {} {} {} {} {} {}".format(
                node["name_id"],
                node["device_type"],
                int(coord[0]),
                int(coord[1]),
                int(node["target"]),
                int(node["volume"]),
                node.get("port") or "UNKNOWN",
                int(node.get("pcie_bandwidth") or 0),
                int(node.get("pcie_delay") or 0),
            )
        )
    out.append("")


def local_to_global(chip_r, chip_c, local_id):
    rows = CFG.CORE_ROWS_PER_CHIP
    cols = CFG.CORE_COLS_PER_CHIP
    abs_r = chip_r * rows + local_id // cols
    abs_c = chip_c * cols + local_id % cols
    return CFG.get_id_by_coords(abs_r, abs_c)


def default_link_delay(src_chip, dst_chip):
    src_node = CFG.get_node_coords_by_chip_coords(*src_chip)
    dst_node = CFG.get_node_coords_by_chip_coords(*dst_chip)
    return CFG.CROSS_CHIP_DELAY if src_node == dst_node else CFG.CROSS_NODE_DELAY


def side_props(rule, prefix, default_delay):
    bandwidth = rule.get(f"{prefix}_bandwidth")
    delay = rule.get(f"{prefix}_delay")
    return (
        int(bandwidth) if bandwidth is not None else 0,
        int(default_delay if delay is None else delay),
    )


def emit_chip_links(out):
    """把片间 PCIe 链路显式算出来，解析侧照表接线，不复刻推导规则。

    规则跟 Bach 的 build_multichip_mesh 一致：两侧都写 NONE 时用默认边，只有一侧
    写规则是错误，两侧都写就按各自的网关列表逐对接。每条物理链路写两条记录，
    每个方向一条，因为带宽与延迟是各自那一侧端口上的取值，可以不对称。
    """
    rows = CFG.CORE_ROWS_PER_CHIP
    cols = CFG.CORE_COLS_PER_CHIP
    per_chip = rows * cols
    rules = CFG.CHIP_RULES or {}

    def rule_of(chip_id):
        return rules.get(str(chip_id), {}) or {}

    def parse_side(value, chip_id, direction):
        return parse_chip_local_core_rule(
            value, per_chip, f"Chip {chip_id} {direction} rule"
        )

    def link(a_core, a_port, a_props, b_core, b_port, b_props):
        out.append(
            f"CHIPLINK {a_core} {a_port} {b_core} {b_port} {a_props[0]} {a_props[1]}"
        )
        out.append(
            f"CHIPLINK {b_core} {b_port} {a_core} {a_port} {b_props[0]} {b_props[1]}"
        )

    for r in range(CFG.CHIP_ROWS):
        for c in range(CFG.CHIP_COLS):
            chip_id = r * CFG.CHIP_COLS + c
            cur = rule_of(chip_id)

            if c < CFG.CHIP_COLS - 1:
                east_id = r * CFG.CHIP_COLS + c + 1
                east = rule_of(east_id)
                right = str(cur.get("RIGHT", "NONE")).strip().upper()
                left = str(east.get("LEFT", "NONE")).strip().upper()
                delay = default_link_delay((r, c), (r, c + 1))
                if right == "NONE" and left == "NONE":
                    pairs = [(cols - 1, 0)]
                elif right == "NONE" or left == "NONE":
                    raise SystemExit(
                        f"Chip {chip_id} 与 Chip {east_id} 的水平 PCIe 规则必须两侧都写。"
                    )
                else:
                    a_list = parse_side(right, chip_id, "RIGHT")
                    b_list = parse_side(left, east_id, "LEFT")
                    if len(a_list) != len(b_list):
                        raise SystemExit(
                            f"Chip {chip_id} RIGHT 有 {len(a_list)} 个端点，"
                            f"Chip {east_id} LEFT 有 {len(b_list)} 个，数量必须相等。"
                        )
                    pairs = list(zip(a_list, b_list))
                for a_local, b_local in pairs:
                    link(
                        local_to_global(r, c, a_local),
                        "PCIE_EAST",
                        side_props(cur, "RIGHT", delay),
                        local_to_global(r, c + 1, b_local),
                        "PCIE_WEST",
                        side_props(east, "LEFT", delay),
                    )

            if r < CFG.CHIP_ROWS - 1:
                south_id = (r + 1) * CFG.CHIP_COLS + c
                south = rule_of(south_id)
                bottom = str(cur.get("BOTTOM", "NONE")).strip().upper()
                top = str(south.get("TOP", "NONE")).strip().upper()
                delay = default_link_delay((r, c), (r + 1, c))
                if bottom == "NONE" and top == "NONE":
                    # 默认边有两条：本 chip 最后一行的最右与最左，各接一条
                    pairs = [
                        ((rows - 1) * cols + (cols - 1), cols - 1),
                        ((rows - 1) * cols, 0),
                    ]
                elif bottom == "NONE" or top == "NONE":
                    raise SystemExit(
                        f"Chip {chip_id} 与 Chip {south_id} 的垂直 PCIe 规则必须两侧都写。"
                    )
                else:
                    a_list = parse_side(bottom, chip_id, "BOTTOM")
                    b_list = parse_side(top, south_id, "TOP")
                    if len(a_list) != len(b_list):
                        raise SystemExit(
                            f"Chip {chip_id} BOTTOM 有 {len(a_list)} 个端点，"
                            f"Chip {south_id} TOP 有 {len(b_list)} 个，数量必须相等。"
                        )
                    pairs = list(zip(a_list, b_list))
                for a_local, b_local in pairs:
                    link(
                        local_to_global(r, c, a_local),
                        "PCIE_SOUTH",
                        side_props(cur, "BOTTOM", delay),
                        local_to_global(r + 1, c, b_local),
                        "PCIE_NORTH",
                        side_props(south, "TOP", delay),
                    )
    out.append("")


OPPOSITE_PCIE_PORT = {
    "PCIE_EAST": "PCIE_WEST",
    "PCIE_WEST": "PCIE_EAST",
    "PCIE_NORTH": "PCIE_SOUTH",
    "PCIE_SOUTH": "PCIE_NORTH",
    "PCIE_UP": "PCIE_DOWN",
    "PCIE_DOWN": "PCIE_UP",
}


def pcie_endpoint(endpoint):
    """一条 PCIe 链路的一端写成三个字段：类型、被指的那个东西、它自己那一头的端口。

    交换节点那一端的端口是 Map 起的名字。核那一端是十二方向之一。外部设备那一端写的
    是它自己路由器上的端口，也就是核侧那个端口的反向：Map 里登记的是核侧的。
    """
    kind = endpoint["type"]
    if kind == "PCIE_SW":
        return f"SW {endpoint['id']} {endpoint['port']}"
    if kind == "CORE":
        return f"CORE {endpoint['id']} {endpoint['port']}"

    node = CFG.EXTERNAL_NODES[tuple(endpoint["coord"])]
    core_side = str(node.get("port") or "UNKNOWN").upper()
    own_side = OPPOSITE_PCIE_PORT.get(core_side)
    if own_side is None:
        raise SystemExit(
            f"{kind} {node['name_id']} 接在交换节点上，但核那一侧的端口 {core_side} 没有反向。"
        )
    return f"{kind} {node['name_id']} {own_side}"


def emit_pcie(out):
    """显式 PCIe 交换拓扑。Map 里的 flows 写法由 config 展开成链路加输入路由，
    这里只把展开好的结果写出来，不重写展开规则。"""
    if not CFG.PCIE_SWITCHES_CONFIG:
        return

    for cfg in CFG.PCIE_SWITCHES_CONFIG:
        chip = cfg.get("chip")
        row, col = cfg["coord"]
        out.append(
            "PCIESW {} {} {} {}".format(
                cfg["key"], -1 if chip is None else int(chip), int(row), int(col)
            )
        )

    for link in CFG.PCIE_LINKS_CONFIG:
        out.append(
            "PCIELINK {} {} {} {} {}".format(
                link["id"],
                pcie_endpoint(link["a"]),
                pcie_endpoint(link["b"]),
                int(link["bandwidth"]),
                int(link["delay"]),
            )
        )

    for switch_id in sorted(CFG.PCIE_ROUTE_TABLES):
        table = CFG.PCIE_ROUTE_TABLES[switch_id]
        for dst in sorted(table):
            out.append(
                f"PCIEROUTE {switch_id} {int(dst[0])} {int(dst[1])} {table[dst]}"
            )

    for switch_id in sorted(getattr(CFG, "PCIE_INPUT_ROUTE_TABLES", {})):
        table = CFG.PCIE_INPUT_ROUTE_TABLES[switch_id]
        for ingress, dst in sorted(table):
            out.append(
                "PCIEIROUTE {} {} {} {} {}".format(
                    switch_id, ingress, int(dst[0]), int(dst[1]), table[(ingress, dst)]
                )
            )

    for host_id in sorted(getattr(CFG, "PCIE_HOST_GROUP_TARGETS", {})):
        targets = CFG.PCIE_HOST_GROUP_TARGETS[host_id]
        for group_id in sorted(targets):
            out.append(f"HOSTGROUP {host_id} {group_id} {int(targets[group_id])}")

    out.append("")


# 以太网交换节点的五个口。名字与解析侧那份白名单一一对应。
ETH_PORTS = [
    ("res_ingress", "ETH_SWITCH_RES_INGRESS_BANDWIDTH_GBps"),
    ("phase1_moe_ingress", "ETH_SWITCH_PHASE1_MOE_INGRESS_BANDWIDTH_GBps"),
    (
        "phase2_moe_result_ingress",
        "ETH_SWITCH_PHASE2_MOE_RESULT_INGRESS_BANDWIDTH_GBps",
    ),
    (
        "phase2_moe_request_egress",
        "ETH_SWITCH_PHASE2_MOE_REQUEST_EGRESS_BANDWIDTH_GBps",
    ),
    ("phase3_res_join_egress", "ETH_SWITCH_PHASE3_RES_JOIN_EGRESS_BANDWIDTH_GBps"),
]

ETH_ROUTES = [
    ("phase1_moe_ingress", "phase2_moe_request_egress"),
    ("res_ingress", "phase3_res_join_egress"),
    ("phase2_moe_result_ingress", "phase3_res_join_egress"),
]


def sink_role(sink):
    """落点角色。Bach 的两个 MOCK 后缀在这一层没有区别，归到同一族。"""
    role = str(sink.get("role", sink.get("sink_role", "GENERIC"))).strip().upper()
    if role in ("MOE", "MOE_MOCK"):
        return "MOE"
    if role in ("BYPASS", "BYPASS_MOCK"):
        return "BYPASS"
    return "GENERIC"


def lane_uids(lane):
    if lane.get("user_ids"):
        return [int(u) for u in lane["user_ids"]]
    rng = lane.get("user_range")
    if rng:
        return list(range(int(rng[0]), int(rng[1])))
    return list(range(int(CFG.NUM_USERS)))


def lane_sink_id(lane, key, sinks_by_id):
    target = lane.get(key, lane.get(key.replace("_target", "_sink")))
    if target is None:
        return ""
    name = str(target).strip().upper()
    if name not in sinks_by_id:
        raise SystemExit(
            f"Phase1 通道 {lane['id']} 的 {key} 指向 {target!r}，"
            "它不是任何一个 phase1_sinks 的 id。通道只往落点发。"
        )
    return name


def lane_group_id(lane):
    for key in ("chip_group_id", "group_id"):
        if lane.get(key) is not None:
            return int(lane[key])
    coord = tuple(lane["coord"])
    node = CFG.EXTERNAL_NODES.get(coord, {})
    value = node.get("chip_group_id")
    return -1 if value is None else int(value)


def eth_group_contract():
    """一个 EPGroup 上有几个专家、一共几个专家。

    这两个数由 Map 定，不另设参数：路由到的专家总数除以 EPGroup 个数就是组大小。
    另设一个参数就多出一处会与 Map 对不上的地方，那正是 Bach 那边要专门写一条校验
    去查的事。
    """
    strategy = (CFG.MOE_PROFILE or {}).get("strategy", {})
    routed = strategy.get("routed_experts")
    groups = len(CFG.MOE_EP_GROUPS)
    if not routed or not groups:
        return int(CFG.ETH_SWITCH_EXPERT_GROUP_SIZE), 0
    routed = int(routed)
    if routed % groups != 0:
        raise SystemExit(
            f"路由专家数 {routed} 不能被 EPGroup 数 {groups} 整除，"
            "组大小定不下来。这种 Map 要用 EXPGROUP 逐个写明对应关系。"
        )
    return routed // groups, routed


def emit_phase(out, mode):
    """Phase1 那一段与以太网交换节点。

    四个开关只放行两种组合：只判 Phase1 边界，或者一路走到 Phase3 汇合。别的组合
    在 Bach 那边也是前置条件不成立，写出来解析侧照样会拒。
    """
    lanes = list(CFG.PHASE1_LANES_CONFIG)
    sinks = list(CFG.PHASE1_SINKS_CONFIG)
    if mode == "off":
        if lanes or sinks:
            raise SystemExit(
                "这张 Map 有 phase1_lanes 或 phase1_sinks，要写它们就得给 --phase。"
            )
        return
    if not lanes:
        raise SystemExit("--phase 要求这张 Map 有 phase1_lanes。")

    sinks_by_id = {str(s["id"]).strip().upper(): s for s in sinks}
    for sink in sinks:
        out.append(
            "PHASE1SINK {} {}".format(str(sink["id"]).strip().upper(), sink_role(sink))
        )

    for lane in lanes:
        name = str(lane["id"]).strip().upper()
        bypass = lane_sink_id(lane, "bypass_target", sinks_by_id)
        moe = lane_sink_id(lane, "moe_target", sinks_by_id)
        volume = int(lane.get("volume") or 0)
        out.append(
            "PHASE1LANE {} {} {} {} {} {} {} {} {} {} {} {}".format(
                name,
                int(lane.get("layer_id") or 0),
                lane_group_id(lane),
                bypass or "-",
                moe or "-",
                int(lane.get("bypass_volume") or volume),
                int(lane.get("moe_volume") or volume),
                int(lane.get("fc0_delay") or 0),
                int(lane.get("res_delay") or 0),
                int(lane.get("norm_delay") or 0),
                int(lane.get("router_delay") or 0),
                int(CFG.HOST_PUSH_DELAY if lane.get("push_delay") is None
                    else lane["push_delay"]),
            )
        )
        hit_map = lane.get("hit_map", lane.get("hitmap", lane.get("HitMap", ())))
        if hit_map:
            out.append(
                "PHASE1HIT {} {}".format(
                    name, " ".join(str(int(e)) for e in hit_map)
                )
            )
        out.append(
            "PHASE1USER {} {}".format(
                name, " ".join(str(u) for u in lane_uids(lane))
            )
        )

    group_size, expert_count = eth_group_contract()
    out.append(
        "ETHSW {} {} {} {} {} {}".format(
            int(CFG.ETH_SWITCH_SWITCH_PROCESSING_DELAY_NS),
            int(CFG.ETH_SWITCH_HEADER_BYTES),
            int(CFG.ETH_SWITCH_PAYLOAD_NUMEL),
            int(CFG.ETH_SWITCH_PAYLOAD_BYTES_PER_ELEM),
            group_size,
            expert_count,
        )
    )
    for port_id, attr in ETH_PORTS:
        out.append(
            "ETHPORT {} {} {} {} {}".format(
                port_id,
                int(getattr(CFG, attr)),
                int(CFG.ETH_SWITCH_PROPAGATION_DELAY_NS),
                int(CFG.ETH_SWITCH_DEFAULT_EGRESS_QUEUE_CAPACITY_PACKETS),
                int(CFG.ETH_SWITCH_DEFAULT_PENDING_CAPACITY_PACKETS),
            )
        )
    for in_port, out_port in ETH_ROUTES:
        out.append(f"ETHROUTE {in_port} {out_port}")

    for expert_id, groups in sorted((CFG.ETH_SWITCH_EXPERT_GROUP_MAP or {}).items()):
        if isinstance(groups, (list, tuple)):
            listed = [int(g) for g in groups]
        else:
            listed = [int(groups)]
        out.append(
            "EXPGROUP {} {}".format(int(expert_id), " ".join(str(g) for g in listed))
        )

    full = mode == "full"
    out.append("ETHMODE {} {} {} {}".format(0 if full else 1, *((1, 1, 1) if full else (0, 0, 0))))

    if full:
        emit_phase_host_groups(out)
    out.append("")


def emit_phase_host_groups(out):
    """一路走到 Phase3 时要知道每个 group 该注入哪个核。

    有 PCIe 交换拓扑时这张表已经由它写过了，这里只补没写的那种：从 Host 的绑定与
    它自己的注入目标推出来。一个 Host 只有一个注入目标，所以它名下那几个 group 都
    指向同一个核。
    """
    if getattr(CFG, "PCIE_HOST_GROUP_TARGETS", {}):
        return
    if not CFG.MOE_HOST_BINDINGS:
        raise SystemExit("要把 MoE 请求注入 Phase2，Map 就得有 Host 与 EPGroup 的绑定。")
    target_of = {str(h["id"]): int(h["target"]) for h in CFG.HOSTS_CONFIG}
    for host_name in sorted(CFG.MOE_HOST_BINDINGS):
        if host_name not in target_of:
            raise SystemExit(f"EPGroup 绑到了 Host {host_name}，Map 里没有这个 Host。")
        for group_id in sorted(CFG.MOE_HOST_BINDINGS[host_name]):
            out.append(f"HOSTGROUP {host_name} {group_id} {target_of[host_name]}")


def emit_phase_users(out):
    """Phase 模式下 uid 由通道产。

    它们不写注入目标：进哪个核要等交换节点把专家换算成 EPGroup 之后才知道。分片数
    恒为一，因为这时判完成的是交换节点，数的是一次次工作，不是收齐几个包。
    """
    uids = []
    for lane in CFG.PHASE1_LANES_CONFIG:
        name = str(lane["id"]).strip().upper()
        for uid in lane_uids(lane):
            uids.append((uid, name))
    seen = {}
    for uid, name in uids:
        if uid in seen:
            raise SystemExit(
                f"uid {uid} 同时属于通道 {seen[uid]} 与 {name}，"
                "各条通道的 uid 集合必须互不重叠。"
            )
        seen[uid] = name

    out.append(f"TOTALUSERS {len(uids)}")
    # 判完成的是交换节点，时钟也由它停，所以这个数在 Phase 模式下没人读。
    out.append("TOTALPACKETS 0")
    for uid, name in uids:
        out.append(f"USER {uid} PHASE1_LANE {name}")
        out.append(f"HITMAP {uid}")
        out.append(f"OUTFRAG {uid} 1")
    out.append("")


def emit_gateways(out):
    rules = CFG.CHIP_RULES or {}
    per_chip = CFG.CORE_ROWS_PER_CHIP * CFG.CORE_COLS_PER_CHIP
    for chip_id in sorted(rules, key=lambda k: int(k)):
        rule = rules[chip_id] or {}
        for direction in DIRECTIONS:
            value = str(rule.get(direction, "NONE")).strip().upper()
            if value == "NONE":
                continue
            cores = parse_chip_local_core_rule(
                value, per_chip, f"Chip {chip_id} {direction} rule"
            )
            joined = " ".join(str(x) for x in cores)
            out.append(f"GATEWAY {chip_id} {direction} {joined}")
            bandwidth = rule.get(f"{direction}_bandwidth")
            delay = rule.get(f"{direction}_delay")
            if bandwidth is not None or delay is not None:
                out.append(
                    "DIRATTR {} {} {} {}".format(
                        chip_id,
                        direction,
                        int(bandwidth or 0),
                        int(delay or 0),
                    )
                )
    out.append("")


def host_uids(host):
    """一个 Host 负责的 uid 集合：user_ids、user_range、NUM_USERS 依次取第一个可用者。"""
    user_ids = host.get("user_ids")
    if isinstance(user_ids, (list, tuple)) and user_ids:
        return [int(u) for u in user_ids]
    user_range = host.get("user_range")
    if isinstance(user_range, (list, tuple)) and len(user_range) == 2:
        return list(range(int(user_range[0]), int(user_range[1])))
    return list(range(int(CFG.NUM_USERS)))


def count_output_sends(table):
    """一个 user 会往汇聚点送几包：数任务表里目标是 OUT 类外部节点的 DTE 发送行。

    它决定时钟什么时候停，不决定一个 user 什么时候算完成。dense 与广播两种 Map 上这
    些行全都会执行，所以数出来就是会到的包数；MoE 下有些边会被 HitMap 挡掉，那时按
    本次命中的 EPGroup 算。
    """
    out_coords = {
        tuple(coord)
        for coord, node in CFG.EXTERNAL_NODES.items()
        if str(node.get("device_type", "")).strip().upper() == "OUT"
    }
    if not out_coords:
        raise SystemExit("这张 Map 没有 OUT 节点，结果无处可去。")

    count = 0
    for entries in table.values():
        for unit, opcode, _tag, _up, _down, coords, _vol in entries:
            if unit.name != "DTE":
                continue
            if opcode_name(opcode) == "RETIRE":
                continue
            if tuple(coords) in out_coords:
                count += 1
    if count == 0:
        raise SystemExit("任务表里没有一条发往 OUT 节点的行，结果无处可去。")
    return count


# 类名必须叫 Host：dispatcher 按 type(dev).__name__ 挑出注入源。它只读三个字段，
# 别的都是运行时的东西，导出时用不上。
class Host:
    def __init__(self, name_id, map_host_id, target_id):
        self.name_id = name_id
        self.map_host_id = map_host_id
        self.target_id = target_id


def emit_credit_edges(out):
    """软件 credit 图：谁向谁开户，额度上限看下游核的类型。

    这张图与物理连线、与任务表都不互相推导，所以必须单独写出去。少了它，没有验资任务
    的核就不知道该给哪些下游开户，下游退休时额度就还不回去。
    """
    graph = CFG.MAP_PARSER.get_core_type_graph()
    for src in sorted(graph):
        for entry in graph[src]:
            out.append(
                "CREDITEDGE {} {} {}".format(
                    int(src), int(entry["target"]), entry["core_type"].name
                )
            )
    out.append("")


def emit_moe_config(out):
    """EPGroup 与 Host 的绑定关系。这两张表是分发结果的来源，一并写出去便于核对。"""
    for group in CFG.MOE_EP_GROUPS:
        shared = group.get("shared_ep_id")
        out.append(
            "EPGROUP {} {} {}".format(
                group["group_id"],
                -1 if shared is None else int(shared),
                int(group.get("routed_ep_num") or 0),
            )
        )
    for host_name, group_ids in CFG.MOE_HOST_BINDINGS.items():
        for group_id in group_ids:
            out.append(f"HOSTBIND {host_name} {group_id}")
    if CFG.MOE_EP_GROUPS or CFG.MOE_HOST_BINDINGS:
        out.append("")


def emit_moe_users(out):
    """复刻 dispatcher 每个 token 的路由决策，把结果写成表。

    随机序列必须与 Bach 完全一致，所以这里不自己实现策略，而是拿同一个 dispatcher
    对象按同样的顺序调它的路由函数：同一个种子、同一串调用，得到的就是同一串结果。
    只有等额度与推包间隔那两步跳过，那是运行时的事，不影响随机序列。
    """
    from moe_dispatcher import MoEDispatcher

    hosts = [
        Host(h["id"], h.get("map_host_id", h["id"]), int(h["target"]))
        for h in CFG.HOSTS_CONFIG
    ]
    disp = MoEDispatcher(None, hosts, config=CFG)

    routes = {
        "RANDOM": lambda a, r: disp._route_random(a, r),
        "HOTSPOT": lambda a, r: disp._route_hotspot(a, r),
        "BALANCED": lambda a, r: disp._route_balanced(a, r),
        "SKEWED": lambda a, r: disp._route_skewed(a, r, disp.skew_factor),
    }
    if disp.routing_policy not in routes:
        raise SystemExit(f"未知的 MoE 路由策略 {disp.routing_policy!r}。")
    act_shared = disp.strategy["active_shared_experts"]
    act_routed = disp.strategy["active_routed_experts"]

    out.append(f"TOTALUSERS {CFG.get_total_users()}")
    total_packets = 0
    for uid in range(int(CFG.NUM_USERS)):
        group_hits = routes[disp.routing_policy](act_shared, act_routed)
        if not group_hits:
            raise SystemExit(f"uid {uid} 没有命中任何 EPGroup，分发结果是空的。")

        sorted_groups = sorted(group_hits.items(), key=lambda item: item[0].name_id)
        token_hits = dict(sorted_groups)
        hit_map = disp._build_hit_map(token_hits)
        moe_bitmap = disp._build_moe_bitmap(token_hits)
        by_target = disp._group_hits_by_host_target(disp._group_hits_by_host(token_hits))
        sorted_targets = sorted(
            by_target.items(),
            key=lambda item: (
                item[0][0].name_id,
                -1 if item[0][1] is None else int(item[0][1]),
            ),
        )

        first_host = sorted_targets[0][0][0]
        out.append(f"USER {uid} DISPATCHER {first_host.name_id}")
        for (host, target_id), hosted in sorted_targets:
            target = host.target_id if target_id is None else int(target_id)
            out.append(
                f"USERTARGET {uid} {host.name_id} {target} {sum(hosted.values())}"
            )
        out.append("HITMAP {} {}".format(uid, " ".join(str(g) for g in hit_map)).strip())
        for group_id, count in moe_bitmap:
            out.append(f"BITMAP {uid} {group_id} {count}")
        fragments = disp._expected_output_fragments(group_hits)
        out.append(f"OUTFRAG {uid} {fragments}")
        total_packets += int(fragments)
    # MoE 下会到的包数就是各 user 期望分片数之和：被 HitMap 挡掉的那些边本来就不发。
    out.append(f"TOTALPACKETS {total_packets}")
    out.append("")


def emit_users(out, table):
    if CFG.get_moe_host_ids():
        emit_moe_users(out)
        return

    out.append(f"TOTALUSERS {CFG.get_total_users()}")

    # 注入源开跑的 user 只等一个分片。它的结果可以落在几个汇聚点上，但 Bach 的注入
    # 源不数它们：收到第一个就算这个 user 完成了。会到的包仍然要等齐，时钟才停，所以
    # 那个数单独给。
    per_user = count_output_sends(table)
    out.append(f"TOTALPACKETS {per_user * int(CFG.get_total_users())}")
    seen = {}
    for host in CFG.HOSTS_CONFIG:
        name = host["id"]
        target = int(host["target"])
        for uid in host_uids(host):
            if uid in seen:
                raise SystemExit(
                    f"uid {uid} 同时属于 Host {seen[uid]} 与 Host {name}，"
                    " 各 Host 的 uid 集合必须互不重叠。"
                )
            seen[uid] = name
            out.append(f"USER {uid} HOST {name}")
            out.append(f"USERTARGET {uid} {name} {target} 0")
            out.append(f"HITMAP {uid}")
            out.append(f"OUTFRAG {uid} 1")
    out.append("")


def main():
    ap = argparse.ArgumentParser(description="Bach Map 到中间文件")
    ap.add_argument("--map", required=True, help="Map 文件路径")
    ap.add_argument("--out", required=True, help="输出的 .bachir 路径")
    ap.add_argument("--users", type=int, default=None, help="覆盖 NUM_USERS")
    ap.add_argument("--streams", type=int, default=None, help="覆盖 STREAM_COUNT")
    ap.add_argument("--seed", type=int, default=None, help="覆盖 RANDOM_SEED")
    ap.add_argument(
        "--dsa",
        choices=("off", "five_route"),
        default=None,
        help="覆盖 DTE_DSA_MODE，five_route 要求执行通道是 split",
    )
    ap.add_argument(
        "--phase",
        choices=("off", "boundary", "full"),
        default="off",
        help="Phase 那一段写不写。boundary 只判 Phase1 边界，"
             "full 一路走到 Phase3 汇合",
    )
    args = ap.parse_args()

    map_path = str(Path(args.map).resolve())
    CFG.load_topology_map(map_path)
    if args.users is not None:
        CFG.NUM_USERS = args.users
    if args.streams is not None:
        CFG.STREAM_COUNT = args.streams
    if args.seed is not None:
        CFG.RANDOM_SEED = args.seed
    if args.dsa is not None:
        if args.dsa == "five_route" and str(CFG.DTE_EXECUTION_MODE).strip().lower() != "split":
            raise SystemExit("five_route 要求 DTE_EXECUTION_MODE 是 split。")
        CFG.DTE_DSA_MODE = args.dsa

    table, credit_table = phaser.build_software_tables()

    out = []
    emit_meta(out, map_path)
    emit_params(out)
    emit_dim(out)
    emit_cores(out, table)
    emit_tasks(out, table, credit_table)
    emit_ext_nodes(out)
    emit_gateways(out)
    emit_chip_links(out)
    emit_pcie(out)
    emit_phase(out, args.phase)
    emit_credit_edges(out)
    emit_moe_config(out)
    if args.phase == "off":
        emit_users(out, table)
    else:
        emit_phase_users(out)

    dst = Path(args.out)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text("\n".join(out).rstrip("\n") + "\n", encoding="utf-8")

    tasks = sum(len(v) for v in table.values())
    print(f"写出 {dst}")
    print(f"  活跃 Core {len(table)} 个，任务表共 {tasks} 行，{len(out)} 行记录")


if __name__ == "__main__":
    main()
