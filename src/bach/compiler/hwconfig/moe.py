"""一层 MoE 摊在阵列上那套拓扑的展开。

给的是参数：阵列摆几行几列、每一列 chip 是哪一种、两层还是几层一个 EP 组、token
从不从 B core 进来。这一层把它展开成每颗 chip 的 core_bad_mask、每个 core 的角
色、任务链、进核配置与每条 path 在每个 core 上的表项。

数据流照 EP6+TP8 的 KN 拆分：B core 把 token 广播进各计算 core；每颗 chip 的 8
份 FC1、FC3 部分和沿 chip 内归约链归约进 dot core；dot core 做 silu·dot·量化，
把 FC2 输入广播回本 chip；8 段 FC2 在 dot core 上拼成 concat；一行 chip 的 dot
core 逐跳归约进本行 R core，各行 R core 串成一条链逐行相加。

常量与模型侧同源，改一处要一起改：方向与 flow 位照 `router_table.h`，chip 的
四个口与 2×5 的编号照 `chip.h`，角色分配照 `lpu_grid.h` 的 `RoleOf`，Core Mem
与 Share Mem 上的摆放照 kernel 的 `bach.h`。

每颗 chip 2×5 共 10 个 core，编号

    0 1 2 3 4
    5 6 7 8 9

中间两列 chip 的 core2、core7 是坏 core，两侧两列 10 个 core 全好；编译器只实现
这一种坏 core 布局。每颗 chip 的 8 个计算 core 当 2 行 4 列的格子用，格子里的
位置这里叫槽位，槽位号是 行 × 4 + 逻辑列。逻辑列到物理列按 chip 所在列换算，
同一行逻辑上相邻的两个槽位物理上可能隔着坏 core，隔着的 core 在那条 path 上只
转发。槽位 7 是 dot core。
"""

# 方向与 flow 位
DIR_MID, DIR_LEFT, DIR_RIGHT, DIR_NUM = 0, 1, 2, 4
FLOW_MID, FLOW_LEFT, FLOW_RIGHT = 1, 2, 4
FLOW_REDUCE1, FLOW_REDUCE2 = 8, 16
# 三个 R2R 方向那一组位
FLOW_ALL_DIR = FLOW_MID | FLOW_LEFT | FLOW_RIGHT
# streamNeedMask 的进本 core 那一位，低三位与 flow 同位
CORE_NEED = 8
# VC 数，与模型的 kVcNum 同源。
VC_NUM = 4

# chip 的四个对外口
CHIP_N, CHIP_E, CHIP_W, CHIP_S, CHIP_PORT_NUM = 0, 1, 2, 3, 4

# chip 所在列
COL_FIRST, COL_MIDDLE, COL_LAST = 0, 1, 2
COL_NAME = {"first": COL_FIRST, "middle": COL_MIDDLE, "last": COL_LAST}

# 一颗 chip 2 行 × 5 列
CHIP_COLS = 5
CHIP_CORES = 2 * CHIP_COLS

# 每列 chip 的 core_bad_mask：第 i 位为 1 表示 core i 是坏 core
BAD_MASK_OF_COL = {COL_FIRST: 0x000, COL_MIDDLE: 0x084, COL_LAST: 0x000}

# 计算 core 的格子
COLS = 4
CORE_PER_CHIP = 8
DOT_SLOT = 7
# chip 内归约链的逻辑槽位次序，链尾是 dot core。与 reference/vectors.py 的
# KN_CHIP_CHAIN 同源。
CHIP_CHAIN = (6, 5, 4, 0, 1, 2, 3, 7)

# 逻辑列 0～3 对应的物理列
PHYS_COL_OF = {
    COL_FIRST: (1, 2, 3, 4),
    COL_MIDDLE: (0, 1, 3, 4),
    COL_LAST: (0, 1, 2, 3),
}

# 四个口各坐在哪个 core 上：N 接 core0 左侧，E 接 core4 右侧，W 接 core5 左侧，
# S 接 core9 右侧
PORT_CORE = {CHIP_N: 0, CHIP_E: CHIP_COLS - 1, CHIP_W: CHIP_COLS,
             CHIP_S: CHIP_CORES - 1}

# Operation：0 转发、1 reduce0 单流、2 reduce1 两流、3 reduce2 三流（Router MAS）
OP_FORWARD, OP_REDUCE0, OP_REDUCE1 = 0, 1, 2
# OpType：1 transfer、2 reduce
OPTYPE_TRANSFER, OPTYPE_REDUCE = 1, 2
# reduce 的输入输出精度：0 BF16、1 FP32，与模型的 kReduceBf16 / kReduceFp32 同源
REDUCE_BF16 = 0

# 几条 path 各管一段。
IN_PATH = 4          # token 广播进各计算 core
BCAST_IN_PATH = 5    # 送进 B core 那一段
CHIP_RED_PATH = 3    # chip 内 FC1、FC3 部分和归约进 dot core
ROW_PATH = 7         # 一行 chip 的 dot core 逐跳归约进本行 R core
FC2_BCAST_PATH = 11  # dot core 把 FC2 输入广播回本 chip
# concat：本 chip 另外 7 个计算 core 把 FC2 那一段发给 dot core，每个槽位一条。
# dot core 上 7 项 concat 搬入各对一个 PID，DTE 的 path_task_map 一个 PID 对一项。
CONCAT_PATH = tuple(16 + s for s in range(DOT_SLOT))
# R core 之间那条链与组间那一跳各用两个号轮换：一个 core 既要收上游又要往下游
# 发时，同一个号上填不下两种转法。
R_PATH = (8, 9)
RELAY_PATH = (6, 10)

# 每条 path 走哪个 VC。VC3 专给逐级 reduce；FC2 输入广播与 concat 各用一个 VC，
# 与 token 广播分开。TS 的 ROUTER_TABLE 给出核那一跳的 VC，路由表的 nxt_vc 给后
# 面各跳，两处取同一个值。
REDUCE_VC = 3
VC_OF_PATH = {IN_PATH: 0, BCAST_IN_PATH: 0, RELAY_PATH[0]: 0,
              RELAY_PATH[1]: 0, R_PATH[0]: 0, R_PATH[1]: 0,
              CHIP_RED_PATH: REDUCE_VC, ROW_PATH: REDUCE_VC,
              FC2_BCAST_PATH: 1}
VC_OF_PATH.update({p: 2 for p in CONCAT_PATH})


def dir_of_port(port):
    """chip 的某个口用的是所在 core 的哪个方向。"""
    return DIR_LEFT if port in (CHIP_N, CHIP_W) else DIR_RIGHT


def slot_of_port(port):
    """四个 chip 口各坐在格子的哪个槽位上。"""
    return {CHIP_N: 0, CHIP_E: COLS - 1, CHIP_W: COLS, CHIP_S: 2 * COLS - 1}[port]


def core_of_slot(col, slot):
    """槽位号换成片内 core 号。col 是 chip 所在列。"""
    return slot // COLS * CHIP_COLS + PHYS_COL_OF[col][slot % COLS]


def core_of_port(port):
    """坐在某个 chip 口上的那个 core，不一定是计算 core。"""
    return PORT_CORE[port]


def port_sits_on_compute(col, port):
    return core_of_port(port) == core_of_slot(col, slot_of_port(port))


def cores_between(col, a, b):
    """同一行左右相邻的两个槽位之间隔着的 core，按从 a 到 b 的次序排。上下同列
    的两个槽位走 mid，不隔 core。"""
    if a // COLS != b // COLS:
        return []
    ca, cb = core_of_slot(col, a), core_of_slot(col, b)
    step = 1 if cb > ca else -1
    return list(range(ca + step, cb, step))


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


def bad_mask_of(col, given=None):
    """这一列 chip 的 core_bad_mask。描述里给了就核对：编译器只实现中间两列坏
    core2、core7、两侧全好这一种布局。"""
    want = BAD_MASK_OF_COL[col]
    if given is None:
        return want
    got = int(given, 0) if isinstance(given, str) else int(given)
    if got != want:
        raise ValueError(f"core_bad_mask 0x{got:03x} 不支持：中间两列只接受"
                         f" 0x084，两侧只接受 0")
    return got


class Entry:
    """一条 path 在一个 core 上的表项，字段与模型的 RouteEntry 一一对应。"""

    def __init__(self, flow_dir=0, vc=0, operation=OP_FORWARD,
                 op_type=OPTYPE_TRANSFER, bypass=True, mask_enable=False,
                 mask_idx=0, need_buffer=False, stream_need=0,
                 credit_type=0, credit_require=0, reduce_in_mask=0,
                 reduce_data_type=0, reduce_outdata_type=0, stall_way=False,
                 ext_dst=0):
        self.flow_dir = flow_dir
        self.vc = vc
        self.operation = operation
        self.op_type = op_type
        self.path_core_bypass = bypass
        self.mask_enable = mask_enable
        self.mask_idx = mask_idx
        self.need_buffer = need_buffer
        self.stream_need = stream_need
        self.credit_type = credit_type
        self.credit_require = credit_require
        self.reduce_in_mask = reduce_in_mask
        self.reduce_data_type = reduce_data_type
        self.reduce_outdata_type = reduce_outdata_type
        self.stall_way = stall_way
        self.ext_dst = ext_dst


def enter_and_spread(path_id, flow):
    """广播那一条：落进本 core，再按 flow 往下游复制。"""
    return Entry(flow_dir=flow, vc=VC_OF_PATH[path_id], bypass=False)


def pass_through(path_id, flow):
    """只转发：坐在 chip 口上但不是计算 core 的那几个、隔在两个槽位之间的坏
    core、发方自己，都走这一档。"""
    return Entry(flow_dir=flow, vc=VC_OF_PATH[path_id], bypass=True)


def reduce_hop(path_id, in_mask, flow, first, last):
    """归约那一条：本级收哪几路、算完往哪个方向发。flow 不置方向位时结果交回本
    core。

    operation 按收几路编码：链首只收本 core 那一份，其余收两份。输入输出都是
    BF16，中间按 FP32 累加。往下游发不查下游资源：下游的 Rmem 与 Core Mem 是一起
    分配的，一个用户在那边的那一项 Stream 资源同时代表两者。
    """
    del last
    return Entry(flow_dir=flow, vc=REDUCE_VC, op_type=OPTYPE_REDUCE,
                 bypass=True, reduce_in_mask=in_mask,
                 operation=OP_REDUCE0 if first else OP_REDUCE1,
                 reduce_data_type=REDUCE_BF16,
                 reduce_outdata_type=REDUCE_BF16)


def land_in_core(path_id):
    """落进本 core，不再往下发。"""
    return Entry(flow_dir=0, vc=VC_OF_PATH[path_id], bypass=False)


class Fabric:
    """逐 core 逐 path 的表项。铺法一条条往里写，写完交给上层。"""

    def __init__(self, cols):
        self.cols = cols                     # 每颗 chip 所在列
        self.entries = {}                    # {(chip, 片内 core 号): {path: Entry}}
        # DTE 里那份 RouterTable 副本只在出核那几笔要查的 path 上配，内容与
        # Router 那份相同，这里只记哪几条要。
        self.dte = {}
        # Release 静态路由（RTR_RELEASE_ROUTE）：从某个口进来的业务 credit
        # release 转去哪几个方向、交不交给本级。
        # {(chip, 片内 core 号): {入口方向: 出方向掩码}}
        self.release_route = {}

    def put(self, chip, core, path_id, entry, dte=False):
        table = self.entries.setdefault((chip, core), {})
        if path_id in table:
            raise ValueError(f"chip {chip} core {core} 的 path {path_id}"
                             f" 铺了两遍")
        table[path_id] = entry
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
        self.put(chip, core_of_slot(self.cols[chip], slot), path_id, entry,
                 dte)

    def put_hop(self, chip, a, b, path_id):
        """槽位 a 往槽位 b 那一跳隔着的 core 各配一条只转发的表项，方向与这一跳
        相同。"""
        d = dir_between(a, b)
        for core in cores_between(self.cols[chip], a, b):
            self.put(chip, core, path_id, pass_through(path_id, flow_of(d)))

    def put_walk(self, chip, cores, path_id, direction):
        """沿一串相邻的 core 往 direction 转发，每个 core 一条只转发的表项。"""
        for core in cores:
            self.put(chip, core, path_id,
                     pass_through(path_id, flow_of(direction)))


def wire_broadcast(fab, chip, enter_port, out_ports, path_id=IN_PATH):
    """chip 内的 token 广播树。

    token 从 enter_port 进来落在格子的第 0 列，沿本行往右铺满，再经 mid 到另一
    行的同一列，同样往右铺满。行末那个槽位坐在 chip 口上，要不要往外发由
    out_ports 给出。口上那个 core 不是计算 core 时，进出各多一跳转发。
    """
    col = fab.cols[chip]
    if not port_sits_on_compute(col, enter_port):
        fab.put(chip, core_of_port(enter_port), path_id,
                pass_through(path_id,
                             flow_of(opposite_of(dir_of_port(enter_port)))))
    head = slot_of_port(enter_port) // COLS * COLS
    for r, row_head in enumerate((head, head ^ COLS)):
        for j in range(COLS):
            slot = row_head + j
            flow = FLOW_RIGHT if j + 1 < COLS else 0
            if j + 1 < COLS:
                fab.put_hop(chip, slot, slot + 1, path_id)
            else:
                for port in out_ports:
                    if slot_of_port(port) == slot:
                        flow |= flow_of(dir_of_port(port))
            if r == 0 and j == 0:
                flow |= FLOW_MID
            fab.put_slot(chip, slot, path_id, enter_and_spread(path_id, flow))
    for port in out_ports:
        if port_sits_on_compute(col, port):
            continue
        fab.put(chip, core_of_port(port), path_id,
                pass_through(path_id, flow_of(dir_of_port(port))))


def wire_bcore_broadcast(fab, chip, enter_port, out_ports=()):
    """B core 起头的 token 广播树。

    token 从 enter_port 进来，坐在那个口上的 core 不派角色，只把它转给同一列的
    B core；B core 留一份在 Matrix Mem，再往右与往下各发一路，两行各自往右铺满。
    第一列 chip 的 B core 是 core0，它下面的 core5 不派角色，两者同在第 0 列。
    """
    col = fab.cols[chip]
    if col != COL_FIRST:
        raise ValueError("B core 只在第一列 chip 上")
    bcore, spare = BCORE_ID, BCORE_ID + CHIP_COLS

    # 进来那一路：口上那个 core 往 B core 的方向转一跳，B core 收下不再往下传。
    fab.put(chip, core_of_port(enter_port), BCAST_IN_PATH,
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
            fab.put_hop(chip, slot, slot + 1, IN_PATH)
        else:
            for port in out_ports:
                if slot_of_port(port) == slot:
                    flow |= flow_of(dir_of_port(port))
        fab.put_slot(chip, slot, IN_PATH, enter_and_spread(IN_PATH, flow))
    for port in out_ports:
        if port_sits_on_compute(col, port):
            continue
        fab.put(chip, core_of_port(port), IN_PATH,
                pass_through(IN_PATH, flow_of(dir_of_port(port))))


def wire_chip_reduce(fab, chip):
    """chip 内归约链：8 个计算 core 的 FC1、FC3 部分和按 CHIP_CHAIN 逐跳归约，链尾
    的结果交回 dot core。

    同一行相邻两跳隔着坏 core 时多一次转发；上下两行之间走 mid。本 core 自己那一
    份分量从 core 方向进 ReduceModule，默认落在 bit0 那一路，与 mid 同一路；上游
    正好从 mid 来时两者会挤在一起，所以那种 core 的表项置 reduce1，把自己那一份
    挪到 bit1。
    """
    col = fab.cols[chip]
    path = CHIP_RED_PATH
    for i, slot in enumerate(CHIP_CHAIN):
        in_lane = DIR_NUM
        if i > 0:
            prev = CHIP_CHAIN[i - 1]
            in_lane = opposite_of(dir_between(prev, slot))
            fab.put_hop(chip, prev, slot, path)

        last = i + 1 == len(CHIP_CHAIN)
        flow = 0
        if not last:
            out_dir = dir_between(slot, CHIP_CHAIN[i + 1])
            if out_dir == DIR_NUM:
                raise ValueError(f"chip 内归约链第 {i} 跳的两个槽位不相邻")
            flow = flow_of(out_dir)

        own_lane = 1 if in_lane == DIR_MID else 0
        if own_lane == 1:
            flow |= FLOW_REDUCE1
        mask = 1 << own_lane
        if in_lane != DIR_NUM:
            mask |= 1 << in_lane
        fab.put_slot(chip, slot, path,
                     reduce_hop(path, mask, flow, i == 0, last), dte=True)


def wire_fc2_broadcast(fab, chip):
    """FC2 输入广播树：dot core 发出，沿第 1 行往左铺满，同时经 mid 到第 0 行的
    同一列，再沿第 0 行往左铺满。dot core 自己不收。"""
    path = FC2_BCAST_PATH
    fab.put_slot(chip, DOT_SLOT, path,
                 pass_through(path, FLOW_LEFT | FLOW_MID), dte=True)
    # 第 0 行最右那个槽位经 mid 收下，再往左传。
    fab.put_slot(chip, COLS - 1, path, enter_and_spread(path, FLOW_LEFT))
    for head in (DOT_SLOT, COLS - 1):
        for slot in range(head - 1, head - COLS, -1):
            fab.put_hop(chip, slot + 1, slot, path)
            flow = FLOW_LEFT if slot % COLS else 0
            fab.put_slot(chip, slot, path, enter_and_spread(path, flow))


def wire_concat(fab, chip):
    """concat：另外 7 个计算 core 各走自己那一条 path 把 FC2 那一段送进 dot core。

    第 1 行的往右走到 dot core；第 0 行的往右走到最右那个槽位，再经 mid 下到 dot
    core。沿途的计算 core 与隔着的坏 core 只转发。"""
    for src in range(DOT_SLOT):
        path = CONCAT_PATH[src]
        slot = src
        dte = True
        while slot != DOT_SLOT:
            nxt = slot + 1 if slot % COLS + 1 < COLS else slot + COLS
            d = dir_between(slot, nxt)
            fab.put_slot(chip, slot, path, pass_through(path, flow_of(d)),
                         dte=dte)
            fab.put_hop(chip, slot, nxt, path)
            dte = False
            slot = nxt
        fab.put_slot(chip, DOT_SLOT, path, land_in_core(path))


def neighbor(chip, core, direction, rows, cols):
    """(chip, 片内 core) 往某个方向的下一个 core。

    片内按编号走；走到边上时从那个 chip 口出去，E 对 W、S 对 N：core0 坐在 N 口
    上接上一层，core4 坐在 E 口上接右边一颗，core5 坐在 W 口上接左边一颗，core9
    坐在 S 口上接下一层。走出阵列返回 None。
    """
    gx, gy = chip % cols, chip // cols
    row, col = core // CHIP_COLS, core % CHIP_COLS
    if direction == DIR_MID:
        return (chip, core + CHIP_COLS if row == 0 else core - CHIP_COLS)
    if direction == DIR_LEFT:
        if col > 0:
            return (chip, core - 1)
        if core == 0:
            return (chip - cols, CHIP_CORES - 1) if gy > 0 else None
        return (chip - 1, CHIP_COLS - 1) if gx > 0 else None
    if direction == DIR_RIGHT:
        if col + 1 < CHIP_COLS:
            return (chip, core + 1)
        if core == CHIP_COLS - 1:
            return (chip + 1, CHIP_COLS) if gx + 1 < cols else None
        return (chip + cols, 0) if gy + 1 < rows else None
    return None


def fill_credit_en(plan):
    """搬出那几笔要不要先向 Router 要 credit：这条 path 从本 core 出去的方向里有
    要查下游 Stream 资源的，就标上，TS 拿到授权再下发。

    reduce 那几笔不走这一路：它们要的是本级 Rmem 的一份，TS 自己记。
    """
    for key, items in plan.chains.items():
        table = plan.entries.get(key, {})
        recv = set(plan.path_task.get(key, {}))
        for it in items:
            if it.task_type == "REDUCE" or not it.path_id or it.path_id in recv:
                continue
            e = table.get(it.path_id)
            it.credit_en = bool(e is not None and e.stream_need & FLOW_ALL_DIR)


def fill_stream_need(plan, rows, cols):
    """铺 streamNeedMask，顺便给隔在中间的坏 core 配 release 的静态路由。

    一项 Stream 资源代表一个用户在一个 core 上的 Core Mem 与 Rmem 容量。进本 core
    那一位由本 core 自己定：数据真进 core 的置位，落 Matrix Mem 的 B core 与 R
    core 不置，只转发的也不置。往某个方向发那一位只看下一跳：下一跳那个 core 进
    核那一位置了，这一位就置，本 core 发之前查那个方向的表，那个用户在下游退休时
    还回来。

    逐级归约那几跳的表项一律不置进核位：下游做归约的那一份容量与它的 Core Mem 是
    一起分配的，不另记一笔。

    隔着的坏 core 不进 core、也不记账，它那一口配上把 release 往回转的掩码，还的
    那一笔才到得了上游。
    """
    for (chip, core), table in plan.entries.items():
        role = plan.role.get((chip, core))
        for e in table.values():
            if e.path_core_bypass or role in ("BROADCAST", "REDUCTION"):
                continue
            e.stream_need |= CORE_NEED

    for (chip, core), table in plan.entries.items():
        # 坏 core 只转发，不记账：那一跳的账记在它上游那个好 core 上。
        if (plan.bad_mask[chip] >> core) & 1:
            continue
        for path, e in table.items():
            for d in (DIR_MID, DIR_LEFT, DIR_RIGHT):
                if not e.flow_dir & flow_of(d):
                    continue
                skipped = []
                nxt = neighbor(chip, core, d, rows, cols)
                while nxt is not None and (plan.bad_mask[nxt[0]] >> nxt[1]) & 1:
                    skipped.append(nxt)
                    nxt = neighbor(nxt[0], nxt[1], d, rows, cols)
                if nxt is None:
                    continue
                down = plan.entries.get(nxt, {}).get(path)
                if down is None or not down.stream_need & CORE_NEED:
                    continue
                e.stream_need |= flow_of(d)
                for bad in skipped:
                    plan.release_route.setdefault(bad, {})[d] = \
                        flow_of(opposite_of(d))


def dot_core_of(col):
    return core_of_slot(col, DOT_SLOT)


def wire_row(fab, row_chips, has_rcore):
    """行链：一行 chip 的 dot core 从行首到行尾逐跳归约。

    行首那颗 chip 的 dot core 只有自己一份，经 mid 到 core4 从 E 口出；中间几颗从
    W 口进，沿第 1 行往右经过 core5 到 dot core 之间的各个 core，在 dot core 加上
    本 chip 那一份，再经 mid 到 core4 从 E 口出。行尾那颗在最后一列时 dot core 是
    core8，加完往右落进 R core core9 的 Matrix Mem；没有 R core 时行尾那颗同样从
    E 口出去，结果离开阵列。
    """
    path = ROW_PATH
    n = len(row_chips)
    for i, chip in enumerate(row_chips):
        col = fab.cols[chip]
        dot = dot_core_of(col)
        first = i == 0
        into_rcore = has_rcore and i + 1 == n
        last = i + 1 == n
        if into_rcore and dot != RCORE_ID - 1:
            raise ValueError("行尾那颗 chip 的 dot core 要紧挨着 R core")

        in_lane = DIR_NUM
        if not first:
            # 从 W 口进来，沿第 1 行往右到 dot core。
            walk = range(core_of_port(CHIP_W), dot)
            fab.put_walk(chip, walk, path, DIR_RIGHT)
            in_lane = DIR_LEFT

        if into_rcore:
            flow = FLOW_RIGHT
            fab.put(chip, RCORE_ID, path, land_in_core(path))
        else:
            if dot != RCORE_ID:
                raise ValueError("从 E 口出去的那颗 chip 的 dot core 要是 core9")
            flow = FLOW_MID
            fab.put(chip, core_of_port(CHIP_E), path,
                    pass_through(path, FLOW_RIGHT))

        mask = 1
        if in_lane != DIR_NUM:
            mask |= 1 << in_lane
        fab.put(chip, dot, path, reduce_hop(path, mask, flow, first, last),
                dte=True)


def wire_rcore_hop(fab, here, below, path_id):
    """R core 之间那一跳：本行 R core 从 S 口下到下一层最后一列那颗 chip 的 N 口，
    经 core0、core5、core6、core7、core8 落到 core9。below 给 None 表示这是最后一
    行，结果经 mid 到 core4 从 E 口出去。"""
    if below is None:
        fab.put(here, RCORE_ID, path_id, pass_through(path_id, FLOW_MID),
                dte=True)
        fab.put(here, core_of_port(CHIP_E), path_id,
                pass_through(path_id, FLOW_RIGHT))
        return
    fab.put(here, RCORE_ID, path_id, pass_through(path_id, FLOW_RIGHT),
            dte=True)
    fab.put(below, core_of_port(CHIP_N), path_id,
            pass_through(path_id, FLOW_MID))
    fab.put_walk(below, range(CHIP_COLS, RCORE_ID), path_id, DIR_RIGHT)
    fab.put(below, RCORE_ID, path_id, land_in_core(path_id))


def wire_relay_hop(fab, src_chip, mid_chip, dst_chip, path_id):
    """组间那一跳：一组的 B core 把同一份 token 再送一份给下一组的 B core。

    两者之间隔着本组第 1 层那颗第一列 chip，包在它上面从 N 口进、从 S 口出，横
    穿一整颗 chip。第一列 chip 的 core0 与 core5 同在第 0 列，所以路线是 core0
    往下到 core5，再沿第 1 行往右到坐着 S 口的那个 core。
    """
    def cross(chip):
        fab.put(chip, BCORE_ID, path_id, pass_through(path_id, FLOW_MID))
        fab.put(chip, core_of_port(CHIP_W), path_id,
                pass_through(path_id, FLOW_RIGHT))
        for slot in range(CORE_PER_CHIP // 2, CORE_PER_CHIP):
            fab.put_slot(chip, slot, path_id, pass_through(path_id, FLOW_RIGHT))

    cross(src_chip)
    cross(mid_chip)
    fab.put(dst_chip, BCORE_ID, path_id, enter_and_spread(path_id, 0))
    # B core 出核那一笔要在 DTE 那一份副本里查得到本级的路由。
    fab.dte.setdefault((src_chip, BCORE_ID), set()).add(path_id)


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


def compute_chain(slot):
    """计算 core（槽位 0～6）的任务链。

    两项搬入任务标 wait_wake，Router 送来的 PID 按它匹配：token 与 FC2 输入。MU
    的两笔各在一个 task 里发几笔 DSA 任务，收 RV core 那一路。部分和出核是逐级
    reduce 任务，由 Router 报完成。"""
    s = str(slot)
    return [
        ChainItem(0, "DTE", "DSA", ("dte", "task_dte_user_init"),
                  path_id=IN_PATH, wait_wake=True),
        ChainItem(1, "MU", "RV_ONLY", ("mu", "task_mu_part_s" + s)),
        ChainItem(2, "DTE", "DSA", ("dte", "task_dte_send_part"),
                  path_id=CHIP_RED_PATH, task_type="REDUCE", credit_en=True),
        ChainItem(3, "DTE", "DSA", ("dte", "task_dte_user_init"),
                  path_id=FC2_BCAST_PATH, wait_wake=True),
        ChainItem(4, "MU", "RV_ONLY", ("mu", "task_mu_fc2_s" + s)),
        ChainItem(5, "DTE", "DSA", ("dte", "task_dte_send_concat_s" + s),
                  path_id=CONCAT_PATH[slot], end=True),
    ]


# dot core 上 concat 搬入那 7 项从第几项起
DOT_CONCAT_TASK = 7


def dot_chain():
    """dot core（槽位 7）的任务链。

    归约结果与 7 段 concat 都是搬入任务，标 wait_wake；concat 每个上游 core 一
    项，各对一个 PID。行链出核是逐级 reduce 任务，由 Router 报完成。"""
    s = str(DOT_SLOT)
    items = [
        ChainItem(0, "DTE", "DSA", ("dte", "task_dte_user_init"),
                  path_id=IN_PATH, wait_wake=True),
        ChainItem(1, "MU", "RV_ONLY", ("mu", "task_mu_part_s" + s)),
        ChainItem(2, "DTE", "DSA", ("dte", "task_dte_send_part"),
                  path_id=CHIP_RED_PATH, task_type="REDUCE", credit_en=True),
        ChainItem(3, "DTE", "DSA", ("dte", "task_dte_user_init"),
                  path_id=CHIP_RED_PATH, wait_wake=True),
        ChainItem(4, "VU", "RV_ONLY", ("vu", "task_vu_gate")),
        ChainItem(5, "DTE", "DSA", ("dte", "task_dte_send_fc2in"),
                  path_id=FC2_BCAST_PATH),
        ChainItem(6, "MU", "RV_ONLY", ("mu", "task_mu_fc2_s" + s)),
    ]
    for k in range(DOT_SLOT):
        items.append(ChainItem(DOT_CONCAT_TASK + k, "DTE", "DSA",
                               ("dte", "task_dte_user_init"),
                               path_id=CONCAT_PATH[k], wait_wake=True))
    items.append(ChainItem(DOT_CONCAT_TASK + DOT_SLOT, "DTE", "DSA",
                           ("dte", "task_dte_send_row"), path_id=ROW_PATH,
                           task_type="REDUCE", credit_en=True, end=True))
    return items


def path_task_of(slot):
    """DTE 的 path_task_map：进核那一笔的 PID 对任务链上哪一项搬入任务。"""
    if slot != DOT_SLOT:
        return {IN_PATH: 0, FC2_BCAST_PATH: 3}
    table = {IN_PATH: 0, CHIP_RED_PATH: 3}
    for k in range(DOT_SLOT):
        table[CONCAT_PATH[k]] = DOT_CONCAT_TASK + k
    return table


def bcore_chain(out_path, next_path=0):
    """B core 的链二：等一格、广播出去，不是最后一组还要往下一组转一笔。"""
    items = [
        ChainItem(0, "MU", "RV_ONLY", ("mu", "task_bc_wait")),
        ChainItem(1, "DTE", "DSA", ("dte", "task_dte_bc_send"),
                  path_id=out_path, end=next_path == 0),
    ]
    if next_path:
        items.append(ChainItem(2, "DTE", "DSA", ("dte", "task_dte_bc_relay"),
                               path_id=next_path, end=True))
    return items


def rcore_chain(out_path):
    """R core 的链二：扫标志表、搬进 Core Mem、两半求和、送下一行。"""
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

# 业务模式下进核那一笔（DTEIN）：(route, no_ack, 标志表基址, 一个槽位多大)。
# route 0 落 Core Mem、1 落 Matrix Mem。计算 core 落 Core Mem、回 Ack、不置标志；
# B core 与 R core 落 Matrix Mem、不回 Ack，搬完置标志。标志表几何与 kernel/bach.h
# 同源：B core 是 BC_FLAG_OFF 与 BC_TOKEN_BYTES，R core 是 RC_FLAG_OFF 与
# RC_HALF_BYTES。
ROUTE_CM, ROUTE_MM = 0, 1
DTEIN_OF_ROLE = {
    "NORMAL": (ROUTE_CM, 0, 0x0000, 0),
    "BROADCAST": (ROUTE_MM, 1, 0x0500, 6144),
    "REDUCTION": (ROUTE_MM, 1, 0x0000, 0x3080),
}

# 角色分配：组头 chip 的 core0 是 B core，最后一列 chip 的 core9 是 R core。
BCORE_ID = 0
RCORE_ID = 9


def roles_of(col, head):
    """一颗 chip 上 10 个 core 的角色。head 表示本 chip 是有 B core 的组头 chip。

    8 个计算 core 是 NORMAL；第一列组头 chip 的 core0 是 BROADCAST，最后一列
    chip 的 core9 是 REDUCTION；其余的是 SPARE，坏 core 也在其中。与
    `lpu_grid.h` 的 `RoleOf` 同源。
    """
    role = {c: "SPARE" for c in range(CHIP_CORES)}
    for slot in range(CORE_PER_CHIP):
        role[core_of_slot(col, slot)] = "NORMAL"
    if col == COL_FIRST and head:
        role[BCORE_ID] = "BROADCAST"
    elif col == COL_LAST:
        role[RCORE_ID] = "REDUCTION"
    return role


class Plan:
    """一份拓扑展开出来的全部配置。

    `bad_mask` 是每颗 chip 的 core_bad_mask，`entries` 是逐 core 逐 path 的路由
    表，`chains` 是逐 core 的任务链，`role` 是每颗 chip 10 个 core 各派的角色，
    `bcast_dirs` 只有 B core 有，`dtein` 是业务模式下进核那一笔的配置。core 一律
    按 (chip 号, 片内 core 号) 定位。
    """

    def __init__(self):
        self.chip_num = 0
        self.cols = []
        self.bad_mask = []
        self.entries = {}
        self.dte_rtab = {}
        self.path_task = {}
        self.release_route = {}   # {(chip, core): {入口方向: 出方向掩码}}
        self.chains = {}
        self.role = {}
        self.datain = {}          # {(chip, core): (镜像, 函数名)}
        self.datain_pc = {}       # {(chip, core): 入口地址}，由上层查符号表填
        self.dtein = {}           # {(chip, core): (route, no_ack, 基址, 槽位大小)}
        self.bcast_dirs = {}
        self.stream_num = 1
        self.name = ""

    def self_start(self, key):
        """B core 与 R core 是自启动模式：Task 0 不等 Router trigger 就启动。"""
        return self.role.get(key) in ("BROADCAST", "REDUCTION")

    def ts_routes(self):
        """TS 的 ROUTER_TABLE：每个配了任务链的 core，按它用到的 PID 各一项
        {pid: (TASK_DIR, TASK_VCID)}。方向取自这个 core 上那条 path 的路由表项，
        bit0 上下（mid）、bit1 左、bit2 右、bit3 进本 core；VCID 取这条 path 走的
        那个 VC。"""
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
                one[pid] = (d, VC_OF_PATH.get(pid, pid % VC_NUM))
            out[key] = one
        return out


def col_kinds(desc, cols):
    """每一列 chip 是哪一种。描述里没给就按位置：第 0 列是第一列，最后一列是最后
    一列，其余是中间列；只有一列时是第一列。"""
    given = desc.get("col")
    if given is not None:
        if len(given) != cols:
            raise ValueError("col 的项数要等于列数")
        return [COL_NAME[c] for c in given]
    return [COL_FIRST if gx == 0 else COL_LAST if gx == cols - 1
            else COL_MIDDLE for gx in range(cols)]


def build_plan(desc):
    """把一份拓扑描述展开成 Plan。

    chip 摆成 rows × cols，编号 gy × cols + gx，层内左右相接（E 对 W）、层间上下
    相接（S 对 N）。`groups` 个 EP 组平分这几层。`bcore` 为真时 token 只送进第一
    个组左上角那颗 chip 的 B core，它一边广播给本组各计算 core，一边把同一份往下
    一组的 B core 转；为假时 token 从 chip 0 的 W 口直接进计算 core 的广播树。

    每颗 chip 各自做 chip 内归约、FC2 输入广播与 concat。每行 chip 的结果沿行链
    归约：最后一列是“最后一列”那种 chip 时落进它的 R core，各行 R core 串成一
    条链，最后一行 R core 的结果经 core4 从 E 口出去；否则行尾那颗 chip 直接从 E
    口出去。
    """
    rows, cols = int(desc["rows"]), int(desc["cols"])
    groups = int(desc.get("groups", 1))
    if rows % groups:
        raise ValueError("层数要能被组数整除")
    rows_per_group = rows // groups
    use_bcore = bool(desc.get("bcore", True))
    kinds = col_kinds(desc, cols)
    has_rcore = kinds[-1] == COL_LAST

    plan = Plan()
    plan.name = desc.get("name", "moe")
    plan.stream_num = int(desc.get("stream_num", 1))
    plan.cols = [kinds[gx] for gy in range(rows) for gx in range(cols)]
    given_mask = desc.get("core_bad_mask")
    plan.bad_mask = [bad_mask_of(c, given_mask[i] if given_mask else None)
                     for i, c in enumerate(plan.cols)]
    plan.chip_num = len(plan.cols)

    def bcore_chip(g):
        return rows_per_group * g * cols

    fab = Fabric(plan.cols)
    # token 广播树：一组几层，起点是本组左上角那颗 chip。每层往右铺满，第 0 层
    # 的头一颗往下带一层。
    bcores = set()
    for chip in range(plan.chip_num):
        gx, gy = chip % cols, chip // cols
        enter = CHIP_N if (gx == 0 and gy % rows_per_group != 0) else CHIP_W
        outs = []
        if gx + 1 < cols:
            outs.append(CHIP_E)
        if gx == 0 and gy % rows_per_group + 1 < rows_per_group:
            outs.append(CHIP_S)
        if use_bcore and chip == bcore_chip(gy // rows_per_group):
            bcores.add(chip)
            wire_bcore_broadcast(fab, chip, enter, outs)
        else:
            wire_broadcast(fab, chip, enter, outs)

    for chip in range(plan.chip_num):
        wire_chip_reduce(fab, chip)
        wire_fc2_broadcast(fab, chip)
        wire_concat(fab, chip)
    for gy in range(rows):
        wire_row(fab, [gy * cols + gx for gx in range(cols)], has_rcore)
    if has_rcore:
        for gy in range(rows):
            here = gy * cols + cols - 1
            below = here + cols if gy + 1 < rows else None
            wire_rcore_hop(fab, here, below, R_PATH[gy % 2])
    if use_bcore:
        for g in range(groups - 1):
            wire_relay_hop(fab, bcore_chip(g), bcore_chip(g) + cols,
                           bcore_chip(g + 1), RELAY_PATH[g % 2])
    plan.entries = fab.entries
    plan.dte_rtab = fab.dte
    plan.release_route = fab.release_route

    for chip, col in enumerate(plan.cols):
        gy = chip // cols
        g = gy // rows_per_group
        dot = dot_core_of(col)
        for core, role in sorted(roles_of(col, chip in bcores).items()):
            if role == "REDUCTION" and not has_rcore:
                role = "SPARE"
            plan.role[(chip, core)] = role
            if role == "NORMAL":
                slot = DOT_SLOT if core == dot else next(
                    s for s in range(CORE_PER_CHIP)
                    if core_of_slot(col, s) == core)
                plan.chains[(chip, core)] = (dot_chain() if slot == DOT_SLOT
                                             else compute_chain(slot))
                plan.path_task[(chip, core)] = path_task_of(slot)
            elif role == "BROADCAST":
                nxt = RELAY_PATH[g % 2] if g + 1 < groups else 0
                plan.chains[(chip, core)] = bcore_chain(IN_PATH, nxt)
                # B core 搬出之前要查这几个方向的下游资源。
                plan.bcast_dirs[(chip, core)] = FLOW_RIGHT | FLOW_MID
            elif role == "REDUCTION":
                plan.chains[(chip, core)] = rcore_chain(R_PATH[gy % 2])
            else:
                continue
            plan.datain[(chip, core)] = DATAIN_SYM[role]
            plan.dtein[(chip, core)] = DTEIN_OF_ROLE[role]

    fill_stream_need(plan, rows, cols)
    fill_credit_en(plan)
    return plan
