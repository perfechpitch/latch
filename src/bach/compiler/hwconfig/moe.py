"""一层 MoE 摊在阵列上那套拓扑的展开。

给的是参数：阵列摆多大、几个 EP 组、部分和按什么次序归约。这一层把它展开成
每个 core 的任务链与每条 path 在每个 core 上的表项。

常量与模型侧同源，改一处要一起改：方向与 flow 位照 `router_table.h`，chip 的
四个口照 `chip.h`，三种形状的 core 编号照 `ChipShape`。

三种形状的计算 core 都是 8 个，都摆成 2 行 4 列的格子。格子里的位置这里叫槽位，
槽位号是 行 × 4 + 列。中间列 chip 是 2×4，槽位号就是片内 core 号；第一列与最后
一列是 2×5，多出来的那一列放 B core、R core 与不派角色的那个，槽位换算要跳过
它们。
"""

# 方向与 flow 位
DIR_MID, DIR_LEFT, DIR_RIGHT, DIR_NUM = 0, 1, 2, 4
FLOW_MID, FLOW_LEFT, FLOW_RIGHT = 1, 2, 4
FLOW_REDUCE1, FLOW_REDUCE2 = 8, 16
# VC 数，与模型的 kVcNum 同源。
VC_NUM = 4
# Release 静态路由的出方向掩码：低三位与 flow 同位，这一位交给本级
RELEASE_SELF = 8

# chip 的四个对外口
CHIP_N, CHIP_E, CHIP_W, CHIP_S, CHIP_PORT_NUM = 0, 1, 2, 3, 4

# chip 的三种形状
SHAPE_MIDDLE, SHAPE_FIRST, SHAPE_LAST = 0, 1, 2

# 计算 core 的格子
COLS = 4
CORE_PER_CHIP = 8

# Operation：0 转发、1 reduce0 单流、2 reduce1 两流、3 reduce2 三流（Router MAS）
OP_FORWARD, OP_REDUCE0, OP_REDUCE1 = 0, 1, 2
# OpType：1 transfer、2 reduce
OPTYPE_TRANSFER, OPTYPE_REDUCE = 1, 2
# reduce 的输入输出精度都取 FP32
REDUCE_FP32 = 1

# 几条 path 各管一段。VC 由 path_id % 4 定，归约那条要落在 VC3 上。
IN_PATH = 4          # token 广播进各计算 core
BCAST_IN_PATH = 5    # 送进 B core 那一段
OUT_PATH = 7         # 部分和沿链归约
# R core 之间那条链与组间那一跳各用两个号轮换：一个 core 既要收上游又要往下游
# 发时，同一个号上填不下两种转法。
R_PATH = (8, 9)
RELAY_PATH = (6, 10)


def dir_of_port(port):
    """chip 的某个口用的是所在 core 的哪个方向。"""
    return DIR_LEFT if port in (CHIP_N, CHIP_W) else DIR_RIGHT


def slot_of_port(port):
    """四个 chip 口各坐在格子的哪个槽位上。"""
    return {CHIP_N: 0, CHIP_E: COLS - 1, CHIP_W: COLS, CHIP_S: 2 * COLS - 1}[port]


def port_of_slot(slot):
    """这个槽位上挂着哪个 chip 口。没有就返回 CHIP_PORT_NUM。"""
    for port in range(CHIP_PORT_NUM):
        if slot_of_port(port) == slot:
            return port
    return CHIP_PORT_NUM


def core_of_slot(shape, slot):
    """槽位号换成片内 core 号。"""
    row, col = slot // COLS, slot % COLS
    if shape == SHAPE_MIDDLE:
        return row * COLS + col
    base = 1 if shape == SHAPE_FIRST else 0
    return row * (COLS + 1) + base + col


def core_of_port(shape, port):
    """坐在某个 chip 口上的那个 core，不一定是计算 core。"""
    cols = COLS if shape == SHAPE_MIDDLE else COLS + 1
    row = 0 if port in (CHIP_N, CHIP_E) else 1
    at_tail = port in (CHIP_E, CHIP_S)
    return row * cols + (cols - 1 if at_tail else 0)


def port_sits_on_compute(shape, port):
    return core_of_port(shape, port) == core_of_slot(shape, slot_of_port(port))


def dir_between(a, b):
    """槽位 a 往槽位 b 发走哪个方向。不相邻返回 DIR_NUM。"""
    if a // COLS == b // COLS:
        if b == a + 1:
            return DIR_RIGHT
        if a == b + 1:
            return DIR_LEFT
        return DIR_NUM
    return DIR_MID if a % COLS == b % COLS else DIR_NUM


def flow_of(direction):
    if direction == DIR_MID:
        return FLOW_MID
    return FLOW_LEFT if direction == DIR_LEFT else FLOW_RIGHT


def opposite_of(direction):
    """从这个方向发出去的包，落在对面那个 core 的哪个方向上。"""
    if direction == DIR_MID:
        return DIR_MID
    return DIR_RIGHT if direction == DIR_LEFT else DIR_LEFT


class Entry:
    """一条 path 在一个 core 上的表项，字段与模型的 RouteEntry 一一对应。"""

    def __init__(self, flow_dir=0, vc=0, operation=OP_FORWARD,
                 op_type=OPTYPE_TRANSFER, bypass=True, mask_enable=False,
                 mask_idx=0, need_buffer=False, stream_table_enable=False,
                 credit_type=0, credit_require=0, reduce_in_mask=0,
                 reduce_data_type=0, reduce_outdata_type=0, stall_way=False,
                 ext_dst=0, reduce_need=False):
        self.flow_dir = flow_dir
        self.vc = vc
        self.operation = operation
        self.op_type = op_type
        self.path_core_bypass = bypass
        self.mask_enable = mask_enable
        self.mask_idx = mask_idx
        self.need_buffer = need_buffer
        self.stream_table_enable = stream_table_enable
        self.credit_type = credit_type
        self.credit_require = credit_require
        self.reduce_in_mask = reduce_in_mask
        self.reduce_data_type = reduce_data_type
        self.reduce_outdata_type = reduce_outdata_type
        self.stall_way = stall_way
        self.ext_dst = ext_dst
        self.reduce_need = reduce_need


# 表项里的 VC 一律留 0：一笔从哪个 VC 出核由 TS 的 ROUTER_TABLE 随 DTE 任务给出
# （见 Plan.ts_routes），路由表的 nxt_vc 是给转发那一跳改写包头用的，这几条 path
# 上不改。
def enter_and_spread(path_id, flow):
    """广播那一条：落进本 core，再按 flow 往下游复制。"""
    return Entry(flow_dir=flow, bypass=False)


def pass_through(path_id, flow):
    """只转发：坐在 chip 口上但不是计算 core 的那几个走这一档。"""
    return Entry(flow_dir=flow, bypass=True)


def reduce_hop(path_id, in_mask, flow, first, last):
    """归约那一条：本级收哪几路、算完往哪个方向发。结果不进任何 core。

    operation 按收几路编码：链首只收本 core 那一份，其余收两份。链尾那一级的下游
    不做归约，结果往下游发时不查下游 Reduce 资源。
    """
    return Entry(flow_dir=flow, op_type=OPTYPE_REDUCE,
                 bypass=True, reduce_in_mask=in_mask,
                 operation=OP_REDUCE0 if first else OP_REDUCE1,
                 reduce_need=not last,
                 reduce_data_type=REDUCE_FP32, reduce_outdata_type=REDUCE_FP32)


def land_in_core(path_id):
    """链尾那一份落进本 core，不再往下发。"""
    return Entry(flow_dir=0, bypass=False)


class Fabric:
    """逐 core 逐 path 的表项。铺法一条条往里写，写完交给上层。"""

    def __init__(self, shapes):
        self.shapes = shapes                 # 每颗 chip 的形状
        self.entries = {}                    # {(chip, 片内 core 号): {path: Entry}}
        # DTE 里那份 RouterTable 副本只在出核那几笔要查的 path 上配，内容与
        # Router 那份相同，这里只记哪几条要。
        self.dte = {}
        # Release 静态路由（RTR_RELEASE_ROUTE）：从某个口进来的业务 credit
        # release 转去哪几个方向、交不交给本级。
        # {(chip, 片内 core 号): {入口方向: 出方向掩码}}
        self.release_route = {}

    def put(self, chip, core, path_id, entry, dte=False):
        self.entries.setdefault((chip, core), {})[path_id] = entry
        if dte:
            self.dte.setdefault((chip, core), set()).add(path_id)

    def put_release_route(self, chip, core, in_dir, out_mask):
        """一个口一份静态掩码，不分 path：两条 path 要的方向不一样就配不出来。"""
        table = self.release_route.setdefault((chip, core), {})
        old = table.get(in_dir, out_mask)
        if old != out_mask:
            raise ValueError(f"chip {chip} core {core} 方向 {in_dir} 的 Release"
                             f"路由对不上：{old} 与 {out_mask}")
        table[in_dir] = out_mask

    def put_slot(self, chip, slot, path_id, entry, dte=False):
        self.put(chip, core_of_slot(self.shapes[chip], slot), path_id, entry,
                 dte)


def wire_broadcast(fab, chip, enter_port, out_ports, path_id=IN_PATH):
    """chip 内的广播树。

    token 从 enter_port 进来落在格子的第 0 列，沿本行往右铺满，再经 mid 到另一
    行的同一列，同样往右铺满。行末那个槽位坐在 chip 口上，要不要往外发由
    out_ports 给出。口上那个 core 不是计算 core 时，进出各多一跳转发。
    """
    shape = fab.shapes[chip]
    if not port_sits_on_compute(shape, enter_port):
        fab.put(chip, core_of_port(shape, enter_port), path_id,
                pass_through(path_id,
                             flow_of(opposite_of(dir_of_port(enter_port)))))
    head = slot_of_port(enter_port) // COLS * COLS
    for r, row_head in enumerate((head, head ^ COLS)):
        for j in range(COLS):
            slot = row_head + j
            flow = FLOW_RIGHT if j + 1 < COLS else 0
            if j + 1 == COLS:
                for port in out_ports:
                    if slot_of_port(port) == slot:
                        flow |= flow_of(dir_of_port(port))
            if r == 0 and j == 0:
                flow |= FLOW_MID
            fab.put_slot(chip, slot, path_id, enter_and_spread(path_id, flow))
    for port in out_ports:
        if port_sits_on_compute(shape, port):
            continue
        fab.put(chip, core_of_port(shape, port), path_id,
                pass_through(path_id, flow_of(dir_of_port(port))))


def wire_reduce_chain(fab, order, tail_flow, path_id=OUT_PATH):
    """归约链：按 order 逐跳铺。

    order 是全局槽位号（chip 号 × 8 加 chip 内槽位号），同一颗 chip 内相邻两跳
    走 core 之间的链路，跨 chip 那一跳走两颗 chip 对接的那个口。口上那个 core
    不是计算 core 时，那一跳多一次转发。

    本 core 自己那一份分量从 core 方向进 ReduceModule，默认落在 bit0 那一路，与
    mid 同一路；上游正好从 mid 来时两者会挤在一起，所以那种 core 的表项置
    reduce1，把自己那一份挪到 bit1。
    """
    for i, gid in enumerate(order):
        chip, self_slot = gid // CORE_PER_CHIP, gid % CORE_PER_CHIP
        shape = fab.shapes[chip]

        in_lane = DIR_NUM
        if i > 0:
            prev_chip = order[i - 1] // CORE_PER_CHIP
            prev = order[i - 1] % CORE_PER_CHIP
            if prev_chip == chip:
                in_lane = opposite_of(dir_between(prev, self_slot))
                if in_lane == DIR_NUM:
                    raise ValueError(f"归约链第 {i} 跳的两个槽位不相邻")
            else:
                port = port_of_slot(self_slot)
                if port == CHIP_PORT_NUM or port_of_slot(prev) == CHIP_PORT_NUM:
                    raise ValueError(f"归约链第 {i} 跳的槽位上没有 chip 口")
                in_lane = dir_of_port(port)
                if not port_sits_on_compute(shape, port):
                    fab.put(chip, core_of_port(shape, port), path_id,
                            pass_through(path_id,
                                         flow_of(opposite_of(in_lane))))
                    # 下游还回来的 Reduce release 从对面那个口进来，原路转回
                    # chip 口那一侧。
                    fab.put_release_route(chip, core_of_port(shape, port),
                                   opposite_of(in_lane), flow_of(in_lane))

        flow = tail_flow
        out_port = CHIP_PORT_NUM
        out_dir = DIR_NUM
        if i + 1 < len(order):
            next_chip = order[i + 1] // CORE_PER_CHIP
            nxt = order[i + 1] % CORE_PER_CHIP
            if next_chip == chip:
                d = dir_between(self_slot, nxt)
                if d == DIR_NUM:
                    raise ValueError(f"归约链第 {i} 跳往下走的两个槽位不相邻")
                flow = flow_of(d)
                out_dir = d
            else:
                out_port = port_of_slot(self_slot)
                if out_port == CHIP_PORT_NUM:
                    raise ValueError(f"归约链第 {i} 跳的出口槽位上没有口")
                flow = flow_of(dir_of_port(out_port))
                out_dir = dir_of_port(out_port)
        else:
            out_port = port_of_slot(self_slot)
        if out_port < CHIP_PORT_NUM and not port_sits_on_compute(shape, out_port):
            fab.put(chip, core_of_port(shape, out_port), path_id,
                    pass_through(path_id, flow_of(dir_of_port(out_port))))
            # 下游还回来的 Reduce release 从 chip 口那一侧进来，原路转回本 chip
            # 里面。
            fab.put_release_route(chip, core_of_port(shape, out_port),
                           dir_of_port(out_port),
                           flow_of(opposite_of(dir_of_port(out_port))))

        own_lane = 1 if in_lane == DIR_MID else 0
        if own_lane == 1:
            flow |= FLOW_REDUCE1
        mask = 1 << own_lane
        if in_lane != DIR_NUM:
            mask |= 1 << in_lane
        fab.put_slot(chip, self_slot, path_id,
                     reduce_hop(path_id, mask, flow, i == 0,
                                i + 1 == len(order)), dte=True)
        if i + 1 < len(order):
            # 下游做完一笔还回来的 release 从往下游去的那个口进来，交给本级。
            fab.put_release_route(chip, core_of_slot(shape, self_slot), out_dir,
                                  RELEASE_SELF)


def wire_bcore_broadcast(fab, chip, enter_port, out_ports=()):
    """B core 起头的广播树。

    token 从 enter_port 进来，坐在那个口上的 core 不派角色，只把它转给同一列的
    B core；B core 留一份在 Matrix Mem，再往右与往下各发一路，两行各自往右铺满。
    第一列 chip 的 B core 是 core0，不派角色的那个是 core5，两者同在第 0 列。
    """
    shape = fab.shapes[chip]
    if shape != SHAPE_FIRST:
        raise ValueError("B core 只在第一列形状的 chip 上")
    bcore, spare = 0, 5

    # 进来那一路：口上那个 core 往 B core 的方向转一跳，B core 收下不再往下传。
    fab.put(chip, core_of_port(shape, enter_port), BCAST_IN_PATH,
            pass_through(BCAST_IN_PATH, FLOW_MID))
    fab.put(chip, bcore, BCAST_IN_PATH, enter_and_spread(BCAST_IN_PATH, 0))

    # 广播出去那一路：B core 自己不收，往右给本行第一个计算 core，往下给另一行
    # 那个只转发的 core。
    fab.put(chip, bcore, IN_PATH,
            pass_through(IN_PATH, FLOW_RIGHT | FLOW_MID), dte=True)
    fab.put(chip, spare, IN_PATH, pass_through(IN_PATH, FLOW_RIGHT))

    for slot in range(CORE_PER_CHIP):
        flow = 0
        if (slot % COLS) + 1 < COLS:
            flow = FLOW_RIGHT
        else:
            for port in out_ports:
                if slot_of_port(port) == slot:
                    flow |= flow_of(dir_of_port(port))
        fab.put_slot(chip, slot, IN_PATH, enter_and_spread(IN_PATH, flow))
    for port in out_ports:
        if port_sits_on_compute(shape, port):
            continue
        fab.put(chip, core_of_port(shape, port), IN_PATH,
                pass_through(IN_PATH, flow_of(dir_of_port(port))))


class ChainItem:
    """任务链上的一笔，字段与 TASK_CHAIN_xx_PC / ATTR 一一对应。

    `sym` 是这一笔在 kernel 里的函数，(镜像, 函数名)；入口地址由上层查符号表填。
    `recv_unit` 取 "RV_ONLY"（只收 RV core 的 ACK）或 "DSA"（RV core 与 DSA 两路
    都收）。`task_type` 取 bachir.TASK_TYPE_NAME 里的一档。
    """

    def __init__(self, idx, unit, recv_unit, sym, path_id=0, wait_wake=False,
                 task_type="NORMAL", p2p_reissue_tid=0, credit_en=False,
                 end=False):
        self.idx = idx
        self.unit = unit
        self.recv_unit = recv_unit
        self.sym = sym
        self.path_id = path_id
        self.wait_wake = wait_wake
        self.task_type = task_type
        self.p2p_reissue_tid = p2p_reissue_tid
        self.credit_en = credit_en
        self.end = end
        self.task_pc = 0


# FC2 的结果拆成几个 reduce 包出核，与 kernel/bach.h 的 MOE_PIECE_NUM 同源。
REDUCE_PIECES = 3


def compute_chain(out_path):
    """计算 core 的任务链：EPTP-NN 那一段。

    token 进核那一项是搬入任务，标 wait_wake，Router 送来的 PID 按它匹配。
    FC2 在一个 task 里按 reduce 包分段发几笔 MU，每笔都报一次完成，所以与 FC1、
    FC3 一样收 RV core 那一路。结果拆成 REDUCE_PIECES 个 reduce 包出核，一包一项
    逐级 reduce 任务，由 Router 报完成；同一个用户同时最多一笔 reduce 在做，所以
    一项做完才轮到下一项。"""
    items = [
        ChainItem(0, "DTE", "DSA", ("dte", "task_dte_user_init"),
                  path_id=IN_PATH, wait_wake=True),
        ChainItem(1, "MU", "RV_ONLY", ("mu", "task_mu_fc13")),
        ChainItem(2, "VU", "RV_ONLY", ("vu", "task_vu_gate")),
        ChainItem(3, "MU", "RV_ONLY", ("mu", "task_mu_fc2")),
    ]
    for k in range(REDUCE_PIECES):
        items.append(ChainItem(4 + k, "DTE", "DSA",
                               ("dte", f"task_dte_send_moe_p{k}"),
                               path_id=out_path, task_type="REDUCE",
                               credit_en=True, end=k + 1 == REDUCE_PIECES))
    return items


def bcore_chain(out_path, next_path=0):
    """B core 的链二：等一格、广播出去，前五组还要往下一组转一笔。"""
    items = [
        ChainItem(0, "VU", "RV_ONLY", ("vu", "task_bc_wait")),
        ChainItem(1, "DTE", "DSA", ("dte", "task_dte_bc_send"),
                  path_id=out_path, credit_en=True, end=next_path == 0),
    ]
    if next_path:
        items.append(ChainItem(2, "DTE", "DSA", ("dte", "task_dte_bc_relay"),
                               path_id=next_path, end=True))
    return items


def rcore_chain(out_path):
    """R core 的链二：扫标志表、搬进 Core Mem、两笔求和、送下一组。"""
    return [
        ChainItem(0, "MU", "RV_ONLY", ("mu", "task_rc_find")),
        ChainItem(1, "DTE", "DSA", ("dte", "task_dte_rc_load")),
        ChainItem(2, "VU", "RV_ONLY", ("vu", "task_vu_add")),
        ChainItem(3, "DTE", "DSA", ("dte", "task_dte_rc_send"),
                  path_id=out_path, end=True),
    ]


# 三档 core 的 datain 任务：进来的包由哪一笔接。
DATAIN_SYM = {
    "NORMAL": ("dte", "task_dte_user_init"),
    "BROADCAST": ("dte", "task_dte_bc_datain"),
    "REDUCTION": ("dte", "task_dte_rc_datain"),
}


# 每颗 chip 的角色分配：第一列多一个 B core 与一个不派角色的，最后一列多一个
# 不派角色的与一个 R core，中间列全是计算 core。
BCORE_ID = 0
RCORE_ID = 9
SPARE_OF = {SHAPE_FIRST: 5, SHAPE_LAST: 4}

SHAPE_NAME = {"middle": SHAPE_MIDDLE, "first": SHAPE_FIRST, "last": SHAPE_LAST}
PORT_NAME = {"N": CHIP_N, "E": CHIP_E, "W": CHIP_W, "S": CHIP_S}
FLOW_NAME = {"mid": FLOW_MID, "left": FLOW_LEFT, "right": FLOW_RIGHT}


class Plan:
    """一份拓扑展开出来的全部配置。

    `entries` 是逐 core 逐 path 的路由表，`chains` 是逐 core 的任务链，`role`
    是每个 core 派的角色，`bcast_dirs` 只有 B core 有。core 一律按
    (chip 号, 片内 core 号) 定位。
    """

    def __init__(self):
        self.chip_num = 0
        self.shapes = []
        self.entries = {}
        self.dte_rtab = {}
        self.path_task = {}
        self.release_route = {}   # {(chip, core): {入口方向: 出方向掩码}}
        self.chains = {}
        self.role = {}
        self.datain = {}          # {(chip, core): (镜像, 函数名)}
        self.datain_pc = {}       # {(chip, core): 入口地址}，由上层查符号表填
        self.bcast_dirs = {}
        self.stream_num = 1
        self.name = ""

    def self_start(self, key):
        """B core 与 R core 是自启动模式：Task 0 不等 Router trigger 就启动。"""
        return self.role.get(key) in ("BROADCAST", "REDUCTION")

    def ts_routes(self):
        """TS 的 ROUTER_TABLE：每个配了任务链的 core，按它用到的 PID 各一项
        {pid: (TASK_DIR, TASK_VCID)}。方向取自这个 core 上那条 path 的路由表项，
        bit0 上下（mid）、bit1 左、bit2 右、bit3 进本 core；VCID 沿用出核按
        path_id 对 VC 数取模的挑法。"""
        out = {}
        for key, items in self.chains.items():
            table = self.entries.get(key, {})
            pids = {it.path_id for it in items} | set(self.path_task.get(key, {}))
            one = {}
            for pid in sorted(pids):
                e = table.get(pid)
                d = 0
                if e is not None:
                    d = e.flow_dir & (FLOW_MID | FLOW_LEFT | FLOW_RIGHT)
                    if not e.path_core_bypass:
                        d |= 8
                one[pid] = (d, pid % VC_NUM)
            out[key] = one
        return out


def cores_of(shape):
    """一颗 chip 上全部 core 的片内号，按角色分档。"""
    compute = [core_of_slot(shape, s) for s in range(CORE_PER_CHIP)]
    role = {c: "NORMAL" for c in compute}
    if shape == SHAPE_FIRST:
        role[BCORE_ID] = "BROADCAST"
        role[SPARE_OF[shape]] = "SPARE"
    elif shape == SHAPE_LAST:
        role[SPARE_OF[shape]] = "SPARE"
        role[RCORE_ID] = "REDUCTION"
    return role


def build_plan(desc):
    """把一份拓扑描述展开成 Plan。"""
    plan = Plan()
    plan.name = desc.get("name", "moe")
    shapes = [SHAPE_NAME[c["shape"]] for c in desc["chips"]]
    plan.shapes = shapes
    plan.chip_num = len(shapes)
    plan.stream_num = int(desc.get("stream_num", 1))

    for chip, shape in enumerate(shapes):
        for core, role in sorted(cores_of(shape).items()):
            plan.role[(chip, core)] = role

    fab = Fabric(shapes)
    bcast = desc["broadcast"]
    bcore_chips = set()
    for one in bcast:
        chip = int(one["chip"])
        enter = PORT_NAME[one["enter_port"]]
        outs = [PORT_NAME[p] for p in one.get("out_port", [])]
        if one.get("from_bcore"):
            wire_bcore_broadcast(fab, chip, enter, outs)
            bcore_chips.add(chip)
        else:
            wire_broadcast(fab, chip, enter, outs)

    for one in desc["reduce"]:
        wire_reduce_chain(fab, one["order"], FLOW_NAME[one["tail_flow"]],
                          int(one.get("path", OUT_PATH)))
    plan.entries = fab.entries
    plan.dte_rtab = fab.dte
    plan.release_route = fab.release_route

    for chip, shape in enumerate(shapes):
        for core, role in sorted(cores_of(shape).items()):
            if role == "NORMAL":
                plan.chains[(chip, core)] = compute_chain(OUT_PATH)
                # 进来那一路的包由链首那一笔接：Router 的请求只带 path_id，本表
                # 是把它翻译成任务链上第几步的唯一途径。
                plan.path_task[(chip, core)] = {IN_PATH: 0}
            elif role == "BROADCAST":
                plan.chains[(chip, core)] = bcore_chain(IN_PATH)
                # B core 搬出之前要查这几个方向的下游资源。
                plan.bcast_dirs[(chip, core)] = FLOW_RIGHT | FLOW_MID
            elif role == "REDUCTION":
                plan.chains[(chip, core)] = rcore_chain(OUT_PATH)
            if role != "SPARE":
                plan.datain[(chip, core)] = DATAIN_SYM[role]
    return plan


# 一颗 chip 内那条归约链的走法：键是 (从哪个口进, 从哪个口出)，进的那一项为
# None 表示本 chip 是整条链的起点。一笔画过 8 个槽位的路径只在进出两个口一横一
# 竖时存在，所以按转向列。与 reference/vectors.py 的 MOE_CHAIN 同源。
CHAIN_IN_CHIP = {
    (None, "e"): [0, 4, 5, 1, 2, 6, 7, 3],
    (None, "s"): [4, 0, 1, 5, 6, 2, 3, 7],
    (None, "n"): [4, 5, 6, 7, 3, 2, 1, 0],
    ("w", "s"): [4, 0, 1, 5, 6, 2, 3, 7],
    ("n", "e"): [0, 4, 5, 1, 2, 6, 7, 3],
    ("w", "n"): [4, 5, 6, 7, 3, 2, 1, 0],
    ("s", "e"): [7, 6, 5, 4, 0, 1, 2, 3],
}


def wire_relay_hop(fab, src_chip, mid_chip, dst_chip, path_id):
    """组间那一跳：一组的 B core 把同一份 token 再送一份给下一组的 B core。

    两者之间隔着本组第 1 层那颗第一列 chip，包在它上面从 N 口进、从 S 口出，横
    穿一整颗 chip。第一列 chip 的 core0 是 B core、core5 不派角色，两者同在第 0
    列，所以路线是 core0 往下到 core5，再沿第 1 行往右到坐着 S 口的那个 core。
    """
    def cross(chip):
        fab.put(chip, BCORE_ID, path_id, pass_through(path_id, FLOW_MID))
        fab.put(chip, core_of_port(SHAPE_FIRST, CHIP_W), path_id,
                pass_through(path_id, FLOW_RIGHT))
        for slot in range(CORE_PER_CHIP // 2, CORE_PER_CHIP):
            fab.put_slot(chip, slot, path_id, pass_through(path_id, FLOW_RIGHT))

    cross(src_chip)
    cross(mid_chip)
    fab.put(dst_chip, BCORE_ID, path_id, enter_and_spread(path_id, 0))
    # B core 出核那一笔要在 DTE 那一份副本里查得到本级的路由。
    fab.dte.setdefault((src_chip, BCORE_ID), set()).add(path_id)


def wire_rcore_hop(fab, here, mid, dst, path_id):
    """R core 之间那一段：从本组 R core 出发，往下经一颗中转 chip 到下一组。

    沿途只在最后一列的 chip 上走，进来落 core0，出去从 R core 那个 S 口。mid 与
    dst 给 None 表示这是链尾那一组，只把结果往 S 口发出去。
    """
    fab.put(here, RCORE_ID, path_id, pass_through(path_id, FLOW_RIGHT), dte=True)
    if mid is None or dst is None:
        return
    row1 = [core_of_slot(SHAPE_LAST, s) for s in range(CORE_PER_CHIP // 2,
                                                       CORE_PER_CHIP)]
    for chip in (mid, dst):
        fab.put(chip, 0, path_id, pass_through(path_id, FLOW_MID))
        for core in row1:
            fab.put(chip, core, path_id, pass_through(path_id, FLOW_RIGHT))
    fab.put(mid, RCORE_ID, path_id, pass_through(path_id, FLOW_RIGHT))
    fab.put(dst, RCORE_ID, path_id, land_in_core(path_id))


def group_order(chip_order, turn, g, cols, rows_per_group):
    """一个 EP 组的归约次序，展开成全局 core 号（chip 号 × 8 加片内槽位号）。"""
    out = []
    for i, chip in enumerate(chip_order):
        gy = g * rows_per_group + chip // cols
        gx = chip % cols
        base = (gy * cols + gx) * CORE_PER_CHIP
        key = (turn[i][0], turn[i][1])
        if key not in CHAIN_IN_CHIP:
            raise ValueError(f"第 {i} 颗 chip 的进出口 {key} 画不出一笔链")
        out += [base + c for c in CHAIN_IN_CHIP[key]]
    return out


def build_lpu_plan(desc):
    """一整个 LPU：chip 摆成 rows × cols，每几层一个 EP 组。

    token 只送进第一个组左上角那颗 chip 的 B core，它一边广播给本组各计算 core，
    一边把同一份往下一组的 B core 转，逐个传到全部组。每组的部分和沿蛇形链归约
    成组结果落进本组的 R core，各组的 R core 串成一条链逐组相加。
    """
    grid = desc["chips"]
    rows, cols = int(grid["rows"]), int(grid["cols"])
    groups = int(desc["groups"])
    if rows % groups:
        raise ValueError("层数要能被组数整除")
    rows_per_group = rows // groups

    plan = Plan()
    plan.name = desc.get("name", "moe_lpu")
    plan.stream_num = int(desc.get("stream_num", 1))
    plan.shapes = [
        SHAPE_FIRST if gx == 0 else SHAPE_LAST if gx == cols - 1
        else SHAPE_MIDDLE
        for gy in range(rows) for gx in range(cols)]
    plan.chip_num = len(plan.shapes)

    def bcore_chip(g):
        return rows_per_group * g * cols

    def rcore_chip(g):
        return (rows_per_group * g + 1) * cols + cols - 1

    fab = Fabric(plan.shapes)
    # 组内广播树：一组两层四列，起点是本组左上角那颗 chip 的 B core。每层往右铺
    # 满，第 0 层的头一颗往下带一层。
    for chip in range(plan.chip_num):
        gx, gy = chip % cols, chip // cols
        enter = CHIP_N if (gx == 0 and gy % rows_per_group == 1) else CHIP_W
        outs = []
        if gx + 1 < cols:
            outs.append(CHIP_E)
        if gx == 0 and gy % rows_per_group == 0:
            outs.append(CHIP_S)
        if chip == bcore_chip(gy // rows_per_group):
            wire_bcore_broadcast(fab, chip, enter, outs)
        else:
            wire_broadcast(fab, chip, enter, outs)

    red = desc["reduce"]
    turn = [(t[0], t[1]) for t in red["turn"]]
    for g in range(groups):
        order = group_order(red["chip_order"], turn, g, cols, rows_per_group)
        wire_reduce_chain(fab, order, FLOW_RIGHT, OUT_PATH)
        # 链尾那颗 chip 上从坐着 E 口一侧的 core 经不派角色的那个转两跳落进 R core。
        last = rcore_chip(g)
        fab.put(last, core_of_port(SHAPE_LAST, CHIP_E), OUT_PATH,
                pass_through(OUT_PATH, FLOW_MID))
        fab.put(last, RCORE_ID, OUT_PATH, land_in_core(OUT_PATH))

        if g + 1 < groups:
            wire_relay_hop(fab, bcore_chip(g), bcore_chip(g) + cols,
                           bcore_chip(g + 1), RELAY_PATH[g % 2])
        wire_rcore_hop(fab, last,
                       last + cols if g + 1 < groups else None,
                       rcore_chip(g + 1) if g + 1 < groups else None,
                       R_PATH[g % 2])
    plan.entries = fab.entries
    plan.dte_rtab = fab.dte
    plan.release_route = fab.release_route

    # 第一列每颗 chip 的 core0 与最后一列每颗的 core9 在构造期就是 B core 与
    # R core，但一个组只用得上一对：只有本组左上角那颗与本组最后那颗配任务链，
    # 其余那些只按路由表转发。
    bcores = {bcore_chip(g) for g in range(groups)}
    rcores = {rcore_chip(g) for g in range(groups)}
    for chip, shape in enumerate(plan.shapes):
        g = (chip // cols) // rows_per_group
        for core, role in sorted(cores_of(shape).items()):
            plan.role[(chip, core)] = role
            if role == "NORMAL":
                plan.chains[(chip, core)] = compute_chain(OUT_PATH)
                plan.path_task[(chip, core)] = {IN_PATH: 0}
            elif role == "BROADCAST" and chip in bcores:
                nxt = RELAY_PATH[g % 2] if g + 1 < groups else 0
                plan.chains[(chip, core)] = bcore_chain(IN_PATH, nxt)
                plan.bcast_dirs[(chip, core)] = FLOW_RIGHT | FLOW_MID
            elif role == "REDUCTION" and chip in rcores:
                plan.chains[(chip, core)] = rcore_chain(R_PATH[g % 2])
            else:
                continue
            plan.datain[(chip, core)] = DATAIN_SYM[role]
    return plan
