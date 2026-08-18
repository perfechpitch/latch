import math
from numbers import Real

from map_tools.topology_mapper import TopologyMapper
from topology_parser import TopologyParser
from map_tools.chip_rule_utils import parse_chip_local_core_rule


PCIE_PORT_NAMES = {
    "PCIE_UP",
    "PCIE_DOWN",
    "PCIE_NORTH",
    "PCIE_SOUTH",
    "PCIE_EAST",
    "PCIE_WEST",
}


def resolve_skew_factor(config) -> float:
    """Resolve the effective MoE skew factor from one run's immutable inputs.

    ``SKEW_FACTOR`` is the raw CLI/API override.  ``None`` means that the map's
    strategy value should be used; numeric zero is an explicit override.
    """
    override = getattr(config, "SKEW_FACTOR", None)
    profile = getattr(config, "MOE_PROFILE", {}) or {}
    strategy = profile.get("strategy", {}) or {}

    if override is not None:
        raw_value = override
        source = "override"
    elif "skew_factor" in strategy:
        raw_value = strategy["skew_factor"]
        source = "map strategy"
    else:
        raw_value = 0.0
        source = "default"

    if isinstance(raw_value, bool) or not isinstance(raw_value, Real):
        raise ValueError(
            f"MoE skew_factor from {source} must be a number in [0.0, 1.0], "
            f"got {raw_value!r}."
        )

    value = float(raw_value)
    if not math.isfinite(value) or not 0.0 <= value <= 1.0:
        raise ValueError(
            f"MoE skew_factor from {source} must be finite and in [0.0, 1.0], "
            f"got {raw_value!r}."
        )
    return value


class _SimulationConfig:
    """全局配置单例，专治层层传参综合征"""

    def __init__(self):

        # ==========================================
        # Topology Map 动态参数区
        # ==========================================

        self.RAW_MAP_DATA = {}

        self.MAP_FILE_PATH = "bach_topology.json"  # 默认与 topology_tui.py 保持一致
        self.CHIP_ROWS = 0  # 默认芯片规模 (行)
        self.CHIP_COLS = 0  # 默认芯片规模 (列)
        self.CORE_ROWS_PER_CHIP = None
        self.CORE_COLS_PER_CHIP = None
        self.NODE_CHIP_ROWS = None
        self.NODE_CHIP_COLS = None
        self.HOSTS_CONFIG = []  # Host 列表
        self.OUTS_CONFIG = []  # Out 列表
        self.PHASE1_LANES_CONFIG = []  # Phase1 SmartNIC-side lanes
        self.PHASE1_SINKS_CONFIG = []  # Mock phase boundary endpoints
        self.PHASE1_ENDPOINTS_BY_ID = {}
        self.PHASE1_CHIPS_PER_GROUP = 4
        self.PHASE1_EXPECTED_CORES_PER_CHIP = 8
        self.PHASE1_CHIP_GROUPS = []
        self.EXTERNAL_NODES = {}  # Device 区
        self.MOE_PROFILE = {}  # MoE Profile
        self.MOE_EP_GROUPS = []  # Logical EPGroup metadata
        self.MOE_HOST_BINDINGS = {}  # Host ID -> group_id list
        self.CHIP_RULES = {}  # Store dynamically in GLOBAL_CONFIG for instant software lookup
        self.PCIE_SWITCHES_CONFIG = []
        self.PCIE_LINKS_CONFIG = []
        self.PCIE_ROUTES_CONFIG = []
        self.PCIE_FLOWS_CONFIG = []
        self.PCIE_ROUTE_TABLES = {}
        self.PCIE_INPUT_ROUTE_TABLES = {}
        self.PCIE_SWITCH_ATTACHED_EXTERNALS = set()
        self.PCIE_HOST_GROUP_TARGETS = {}

        # ==========================================
        # 终端传递动态参数区
        # ==========================================

        # 等待加载的用户数
        self.NUM_USERS = 17
        # TASK_SCHEDULER_STREAM_NUMB
        self.STREAM_COUNT = 8
        # 数据完成DTE计算的全部时间
        # FIXME: 会与Router Delay 产生 Overlap, 为了弥补这个问题, 默认为BASIC_ROUTER_DELAY的一半
        self.DTE_REDUCE_TIME = 32
        # DTE action-engine topology. ``split`` gives Comm and HandleComm one
        # capacity-1 execution lane each; ``shared`` preserves the legacy
        # single-lane model for same-version A/B comparisons.
        self.DTE_EXECUTION_MODE = "split"
        # Route-aware DSA arbitration is opt-in.  ``five_route`` requires the
        # split physical topology and preserves legacy timing when disabled.
        self.DTE_DSA_MODE = "off"
        self.NOC_BASIC_ROUTER_DELAY = None

        # --- NoC/PCIE ---
        # PCIE的延迟
        self.CROSS_CHIP_DELAY = 400
        # To Host/Out PCIE_DELAY
        self.CROSS_NODE_DELAY = 5000
        # Band width, Byte/T
        self.NOC_BANDWIDTH = 128
        self.PCIE_BANDWIDTH = 128
        self.DTE_BANDWIDTH = 512
        self.ROUTER_BUFFER_SIZE = self.NOC_BANDWIDTH
        self.PCIE_BUFFER_SIZE = self.PCIE_BANDWIDTH

        # --- External Device ---
        self.HOST_PUSH_DELAY = 100

        # --- MoE ---
        # Raw CLI/API override.  None falls back to MOE_PROFILE.strategy;
        # numeric 0.0 is a real uniform-with-replacement SKEWED setting.
        self.SKEW_FACTOR = None
        self.RANDOM_SEED = 42  # 🌌 宇宙的终极答案
        self.RUN_ID = None

        # --- Monitor ---
        # 看门狗延迟
        self.WATCHDOG_LIFESPAN_LIMIT = 10000000
        # TTL 检测器
        self.HOP_COUNT_ERR = 12800
        self.HOP_COUNT_WAR = 1024

        # ==========================================
        # 模式开关
        # ==========================================
        self.RELEASE_MODE = False
        self.DUMP_SOFTWARE_MODE = False
        self.DUMP_CREDIT_MODE = False
        self.ENABLE_TRACE = False
        # Packet-route evidence is intentionally independent from release mode.
        # ``auto`` follows ENABLE_TRACE; ``on`` and ``off`` are explicit.
        self.PACKET_ROUTE_TRACE = "auto"
        self.NO_PIPELINE_PRINT = False
        self.SWEEP_MODE = False
        self.DISABLE_MOE_DISPATCHER = False
        self.HARDWARE_LINK_FLOW_RECORDER = None
        self.ENABLE_ETH_SWITCH = False
        self.ENABLE_ETH_SWITCH_PHASE2_INGRESS = False
        self.ETH_SWITCH_PHASE2_INGRESS_INJECT_HOST = False
        self.ENABLE_ETH_SWITCH_PHASE2_OUTPUT_RESULT_BRIDGE = False
        self.ENABLE_ETH_SWITCH_PHASE2_SYNTHETIC_RESULT = False
        self.ETH_SWITCH_COMPLETE_PHASE1_BOUNDARY = True

        # ==========================================
        # 拓扑映射
        # ==========================================
        self.MAPPER = None
        self.MAP_PARSER = None

        self.CORE_LAYOUT = None

        # ==========================================
        # NOTE: 硬件指定, 根据"性能需求规格说明书"
        # ==========================================

        self._DTE_SETUP_TIME = 85
        self._MU_SETUP_TIME = 40
        self._VU_SETUP_TIME = 40
        self.SETUP_AHEAD_DEPTH = 1
        # Optional top-level compute fallbacks. They are intentionally None by
        # default: a map that omits compute time is invalid unless CLI/config
        # explicitly provides the corresponding Matrix/Vector time.
        self.MATRIX_TIME = None
        self.VECTOR_TIME = None
        self._TS_LOGIC_TIME = 16

        # --- EthSwitch Phase1 -> Phase2/3 boundary model ---
        self.ETH_SWITCH_EXPERT_GROUP_SIZE = 16
        self.ETH_SWITCH_EXPERT_GROUP_MAP = None
        self.ETH_SWITCH_SWITCH_PROCESSING_DELAY_NS = 200
        self.ETH_SWITCH_HEADER_BYTES = 128
        self.ETH_SWITCH_PAYLOAD_NUMEL = 6144
        self.ETH_SWITCH_PAYLOAD_BYTES_PER_ELEM = 2
        self.ETH_SWITCH_DEFAULT_EGRESS_QUEUE_CAPACITY_PACKETS = 64
        self.ETH_SWITCH_DEFAULT_PENDING_CAPACITY_PACKETS = 64
        self.ETH_SWITCH_PROPAGATION_DELAY_NS = 0
        self.ETH_SWITCH_RES_INGRESS_BANDWIDTH_GBps = 60
        self.ETH_SWITCH_PHASE1_MOE_INGRESS_BANDWIDTH_GBps = 30
        self.ETH_SWITCH_PHASE2_MOE_RESULT_INGRESS_BANDWIDTH_GBps = 60
        self.ETH_SWITCH_PHASE2_MOE_REQUEST_EGRESS_BANDWIDTH_GBps = 30
        self.ETH_SWITCH_PHASE3_RES_JOIN_EGRESS_BANDWIDTH_GBps = 60

        self._NOC_WIRE_DELAY = 40
        self._NOC_ACCESS_DELAY = 10

        # --- Memory System ---
        self._CM_ARB_DELAY = 15  # Core Mem Arbiter Logic Delay
        self._MM_ARB_DELAY = 50  # Matrix Mem Arbiter Logic Delay
        self._IF_DTE_CM = 128  # DTE <==> CoreMem
        # DTE <==> MatrixMem      WARNING: Just a random number I came up with. Fix it if u need.
        self._IF_DTE_MM = 128
        self._IF_VU_CM = 64  # Vector Unit <==> CoreMem
        self._IF_MU_CM = 256  # Matrix Unit <==> CoreMem
        self._IF_MU_MM = 8192  # Matrix Unit <==> MatrixMem

        # --- Broadcast Core ---
        self.MATRIX_FIFO_CREDIT = 4096

    def get_num_cores(self):
        """
        辅助计算函数
        获得 Core 数量
        """
        self._require_topology_dimensions()
        return (
            self.CHIP_ROWS
            * self.CHIP_COLS
            * self.CORE_ROWS_PER_CHIP
            * self.CORE_COLS_PER_CHIP
        )

    def get_num_chips(self):
        """
        辅助计算函数
        获得 Chip 数量
        """
        if self.CHIP_ROWS <= 0 or self.CHIP_COLS <= 0:
            raise RuntimeError("🚨 Topology Map 尚未加载有效 chip_rows/chip_cols。")
        return self.CHIP_COLS * self.CHIP_ROWS

    def get_cores_per_chip(self):
        self._require_topology_dimensions()
        return self.CORE_ROWS_PER_CHIP * self.CORE_COLS_PER_CHIP

    def _require_topology_dimensions(self):
        missing = [
            name
            for name in (
                "CHIP_ROWS",
                "CHIP_COLS",
                "CORE_ROWS_PER_CHIP",
                "CORE_COLS_PER_CHIP",
                "NODE_CHIP_ROWS",
                "NODE_CHIP_COLS",
            )
            if getattr(self, name) in (None, 0)
        ]
        if missing:
            raise RuntimeError(
                "🚨 Topology Map 尚未加载完整尺寸字段: "
                + ", ".join(missing)
            )

    def get_node_coords_by_chip_coords(self, chip_row: int, chip_col: int) -> tuple[int, int]:
        self._require_topology_dimensions()
        return chip_row // self.NODE_CHIP_ROWS, chip_col // self.NODE_CHIP_COLS

    @staticmethod
    def _read_positive_meta_int(meta: dict, key: str) -> int:
        if key not in meta:
            raise ValueError(f"🚨 Topology Map meta 缺少必要尺寸字段: {key}。")

        value = meta[key]
        if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
            raise ValueError(
                f"🚨 Topology Map meta.{key} 必须是正整数，当前为 {value!r}。"
            )
        return value

    @staticmethod
    def _read_optional_positive_int(data: dict, key: str, default: int) -> int:
        value = data.get(key, default)
        if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
            raise ValueError(
                f"🚨 Topology Map phase1.{key} 必须是正整数，当前为 {value!r}。"
            )
        return value

    def get_coords_by_id(self, core_id: int) -> tuple[int, int]:
        """
        根据硬件配置推导坐标, 防范越界与特殊标记
        """
        if not self.MAPPER:
            raise ImportError(f"🚨 Mapper没有初始化")
        if core_id < 0:
            raise ValueError(f"🚨 抓到一个非法的 CoreID: {core_id}.")

        return self.MAPPER.id_to_global_coords(core_id)

    def get_id_by_coords(self, row: int, col: int) -> int:
        """
        根据硬件配置推导 ID, 防范越界与特殊标记
        """
        if not self.MAPPER:
            raise ImportError(f"🚨 Mapper没有初始化")
        if row < 0 or col < 0:
            raise ValueError(f"🚨 抓到一个非法的 坐标: ({row, col}).")

        return self.MAPPER.global_coords_to_id(row, col)

    def load_topology_map(self, filepath):
        """
        预解码 map 文件, 将系统所需的参数写入全局单例
        """
        self.MAP_FILE_PATH = filepath
        self.MAP_PARSER = TopologyParser(filepath)
        raw_data = self.MAP_PARSER.get_raw_data()

        # 把真正的 CHIP_RULES 读取转移到这里, 严格跟随 map 文件
        self.CHIP_RULES = raw_data.get("chip_rules", {})

        # 2. 提取并校验拓扑规模。Chip/Node 尺寸是硬件拓扑语义，不能默认兜底。
        meta = self.MAP_PARSER.get_meta_info()
        self.CHIP_ROWS = self._read_positive_meta_int(meta, "chip_rows")
        self.CHIP_COLS = self._read_positive_meta_int(meta, "chip_cols")
        self.CORE_ROWS_PER_CHIP = self._read_positive_meta_int(meta, "core_rows_per_chip")
        self.CORE_COLS_PER_CHIP = self._read_positive_meta_int(meta, "core_cols_per_chip")
        self.NODE_CHIP_ROWS = self._read_positive_meta_int(meta, "node_chip_rows")
        self.NODE_CHIP_COLS = self._read_positive_meta_int(meta, "node_chip_cols")

        if self.CHIP_ROWS % self.NODE_CHIP_ROWS != 0:
            raise ValueError(
                "🚨 Topology Map meta.node_chip_rows 必须整除 chip_rows，"
                f"当前 chip_rows={self.CHIP_ROWS}, node_chip_rows={self.NODE_CHIP_ROWS}。"
            )
        if self.CHIP_COLS % self.NODE_CHIP_COLS != 0:
            raise ValueError(
                "🚨 Topology Map meta.node_chip_cols 必须整除 chip_cols，"
                f"当前 chip_cols={self.CHIP_COLS}, node_chip_cols={self.NODE_CHIP_COLS}。"
            )

        self.MAPPER = TopologyMapper(
            chip_rows=self.CHIP_ROWS,
            chip_cols=self.CHIP_COLS,
            core_rows_per_chip=self.CORE_ROWS_PER_CHIP,
            core_cols_per_chip=self.CORE_COLS_PER_CHIP,
        )

        def build_core_layout():
            layout = []
            for chip_r in range(self.CHIP_ROWS):
                for local_r in range(self.CORE_ROWS_PER_CHIP):
                    row = []
                    for chip_c in range(self.CHIP_COLS):
                        row_shard = [
                            self.MAPPER.assemble_id(
                                (chip_r, chip_c), (local_r, local_c)
                            )
                            for local_c in range(self.CORE_COLS_PER_CHIP)
                        ]
                        row.append(row_shard)
                    layout.append(row)
            return layout

        self.CORE_LAYOUT = build_core_layout()

        phase1_runtime = raw_data.get("phase1", {}) or {}
        self.PHASE1_CHIPS_PER_GROUP = self._read_optional_positive_int(
            phase1_runtime, "chips_per_group", 4
        )
        self.PHASE1_EXPECTED_CORES_PER_CHIP = self._read_optional_positive_int(
            phase1_runtime, "cores_per_chip", 8
        )
        self.PHASE1_CHIP_GROUPS = self._build_phase1_chip_groups(
            raw_data.get("phase1_groups", phase1_runtime.get("chip_groups"))
        )

        # 预加载 MoE 配置
        self.MOE_PROFILE = self.MAP_PARSER.get_moe_profile()
        self.MOE_EP_GROUPS = self.MAP_PARSER.get_moe_ep_groups()
        self.MOE_HOST_BINDINGS = self.MAP_PARSER.get_moe_host_bindings(
            self.MOE_EP_GROUPS
        )
        self.ETH_SWITCH_EXPERT_GROUP_MAP = (
            self.MAP_PARSER.get_eth_switch_expert_group_map()
        )
        # 顺便校验一下，如果读取到了策略，可以在控制台悄悄吱一声
        if self.MOE_PROFILE:
            strat = self.MOE_PROFILE.get("strategy", {})
            # print(
            #     f"🎩 [Config] 探测到 MoE 架构: {self.MOE_PROFILE.get('model_name')} | "
            #     f"策略: {strat.get('active_shared_experts')}S + {strat.get('active_routed_experts')}R"
            # )

        # 3. 提取 Host 和 Out 相关的配置信息
        self.HOSTS_CONFIG = self.MAP_PARSER.get_all_hosts()
        self.OUTS_CONFIG = self.MAP_PARSER.get_all_outs()
        self.PHASE1_LANES_CONFIG = list(raw_data.get("phase1_lanes", []))
        self.PHASE1_SINKS_CONFIG = list(raw_data.get("phase1_sinks", []))
        self._validate_phase1_lane_user_ranges()
        has_phase1_boundary = bool(
            self.PHASE1_LANES_CONFIG
            or self.PHASE1_SINKS_CONFIG
            or phase1_runtime
        )
        self.DISABLE_MOE_DISPATCHER = bool(
            has_phase1_boundary
            and phase1_runtime.get("disable_moe_dispatcher", True)
        )

        # 第二道防线: 绝不盲目信任配置文件，主动验证！
        # 这里是 Host/Out 的统一 Fail Fast 入口，替代旧代码里只检查 id、
        # 但 target/volume 仍然 get(default) 的局部保护。通过这一关后，
        # 后续代码会直接使用 h["target"] / h["volume"]，不再兜底。
        global_rows = self.CHIP_ROWS * self.CORE_ROWS_PER_CHIP
        global_cols = self.CHIP_COLS * self.CORE_COLS_PER_CHIP

        for ext in self.HOSTS_CONFIG + self.OUTS_CONFIG:
            self._validate_external_node(ext)
            r, c = ext["coord"]
            if 0 <= r < global_rows and 0 <= c < global_cols:
                raise ValueError(
                    f"🚨 拓扑解析致命错误: 外部节点 {ext.get('id')} 的坐标 ({r}, {c}) 位于芯片内部区域！"
                    f"当前阵列大小为 {global_rows}行 x {global_cols}列，请将它配置在边界外。"
                )
        for ext in self.PHASE1_LANES_CONFIG:
            self._validate_phase1_external_node(ext, "PHASE1_LANE")
            r, c = ext["coord"]
            if 0 <= r < global_rows and 0 <= c < global_cols:
                raise ValueError(
                    f"🚨 Phase1 外部节点 {ext.get('id')} 的坐标 ({r}, {c}) 位于芯片内部区域！"
                    f"当前阵列大小为 {global_rows}行 x {global_cols}列，请将它配置在边界外。"
                )
        for ext in self.PHASE1_SINKS_CONFIG:
            self._validate_phase1_external_node(ext, "PHASE1_SINK")
            r, c = ext["coord"]
            if 0 <= r < global_rows and 0 <= c < global_cols:
                raise ValueError(
                    f"🚨 Phase1 外部节点 {ext.get('id')} 的坐标 ({r}, {c}) 位于芯片内部区域！"
                    f"当前阵列大小为 {global_rows}行 x {global_cols}列，请将它配置在边界外。"
                )

        self.PHASE1_ENDPOINTS_BY_ID = {}
        for ext in self.PHASE1_LANES_CONFIG + self.PHASE1_SINKS_CONFIG:
            endpoint_id = str(ext["id"]).strip().upper()
            self.PHASE1_ENDPOINTS_BY_ID[endpoint_id] = {
                "id": endpoint_id,
                "coord": tuple(ext["coord"]),
                "target": ext["target"],
            }

        # 4. 动态推演 EXTERNAL_NODES
        self.EXTERNAL_NODES = {}

        # 遍历生成 Hosts
        for idx, h in enumerate(self.HOSTS_CONFIG):
            ext_coords = tuple(h["coord"])
            mapping = self._deduce_node_mapping(h, "HOST")
            mapping["host_idx"] = idx  # 记录这是第几个 Host
            # These fields are hardware-visible topology facts. Use direct
            # indexing so a missing field stays a parse-time error instead of
            # becoming an implicit target Core0 or zero-byte transfer.
            mapping["target"] = h["target"]
            mapping["name_id"] = h["id"]
            mapping["volume"] = h["volume"]
            mapping["user_ids"] = h.get("user_ids")
            mapping["pcie_bandwidth"] = h.get("pcie_bandwidth")
            mapping["pcie_delay"] = h.get("pcie_delay")

            # New maps expose only the physical Host -> EPG binding here.
            mapping["ep_groups"] = list(h.get("ep_groups", []))
            if not self.MOE_EP_GROUPS:
                mapping["is_moe"] = h.get("is_moe", False)
                mapping["shared_ep_id"] = h.get("shared_ep_id", 0)
                mapping["routed_ep_num"] = h.get("routed_ep_num", 0)

            self.EXTERNAL_NODES[ext_coords] = mapping

        for idx, o in enumerate(self.OUTS_CONFIG):
            ext_coords = tuple(o["coord"])
            mapping = self._deduce_node_mapping(o, "OUT")

            # Same contract as Host: an Out node without explicit target/volume
            # is malformed topology, not a dense-model shortcut.
            mapping["target"] = o["target"]
            mapping["name_id"] = o["id"]
            mapping["volume"] = o["volume"]
            mapping["pcie_bandwidth"] = o.get("pcie_bandwidth")
            mapping["pcie_delay"] = o.get("pcie_delay")
            self.EXTERNAL_NODES[ext_coords] = mapping

        self._load_pcie_switch_topology(global_rows, global_cols)

        for idx, lane in enumerate(self.PHASE1_LANES_CONFIG):
            ext_coords = tuple(lane["coord"])
            mapping = self._deduce_node_mapping(lane, "PHASE1_LANE")
            chip_group_id = self._phase1_group_id_for_chip(*mapping["chip_coords"])
            if lane.get("chip_group_id", lane.get("group_id")) is not None:
                chip_group_id = int(lane.get("chip_group_id", lane.get("group_id")))
            chip_group = self._phase1_group_by_id(chip_group_id)
            mapping["lane_idx"] = idx
            mapping["target"] = lane["target"]
            mapping["name_id"] = lane["id"]
            mapping["chip_group_id"] = chip_group_id
            mapping["chip_group"] = chip_group
            mapping["volume"] = lane.get("volume", 0)
            mapping["layer_id"] = lane.get("layer_id", 0)
            mapping["hit_map"] = (
                lane.get("hit_map", lane.get("hitmap", lane.get("HitMap", ())))
            )
            mapping["bypass_target"] = lane.get(
                "bypass_target", lane.get("bypass_sink")
            )
            mapping["moe_target"] = lane.get("moe_target", lane.get("moe_sink"))
            mapping["bypass_volume"] = lane.get("bypass_volume")
            mapping["moe_volume"] = lane.get("moe_volume")
            mapping["fc0_delay"] = lane.get("fc0_delay", 0)
            mapping["res_delay"] = lane.get("res_delay", 0)
            mapping["norm_delay"] = lane.get("norm_delay", 0)
            mapping["router_delay"] = lane.get("router_delay", 0)
            mapping["push_delay"] = lane.get("push_delay")
            mapping["buffer_capacity"] = lane.get("buffer_capacity")
            mapping["mapped_cores"] = lane.get(
                "mapped_cores", chip_group.get("core_ids", ())
            )
            mapping["task_sequences"] = lane.get("task_sequences", {})
            mapping["user_range"] = lane.get("user_range")
            mapping["user_ids"] = lane.get("user_ids")
            mapping["auto_start"] = lane.get("auto_start", True)
            mapping["pcie_bandwidth"] = lane.get("pcie_bandwidth")
            mapping["pcie_delay"] = lane.get("pcie_delay")
            self.EXTERNAL_NODES[ext_coords] = mapping

        for idx, sink in enumerate(self.PHASE1_SINKS_CONFIG):
            ext_coords = tuple(sink["coord"])
            mapping = self._deduce_node_mapping(sink, "PHASE1_SINK")
            chip_group_id = self._phase1_group_id_for_chip(*mapping["chip_coords"])
            if sink.get("chip_group_id", sink.get("group_id")) is not None:
                chip_group_id = int(sink.get("chip_group_id", sink.get("group_id")))
            mapping["sink_idx"] = idx
            mapping["target"] = sink["target"]
            mapping["name_id"] = sink["id"]
            mapping["chip_group_id"] = chip_group_id
            mapping["sink_role"] = sink.get("role", sink.get("sink_role", "GENERIC"))
            mapping["volume"] = sink.get("volume", 0)
            mapping["pcie_bandwidth"] = sink.get("pcie_bandwidth")
            mapping["pcie_delay"] = sink.get("pcie_delay")
            self.EXTERNAL_NODES[ext_coords] = mapping

    def _build_phase1_chip_groups(self, explicit_groups=None):
        cores_per_chip = self.get_cores_per_chip()
        chip_entries = []
        for chip_r in range(self.CHIP_ROWS):
            for chip_c in range(self.CHIP_COLS):
                chip_id = chip_r * self.CHIP_COLS + chip_c
                core_ids = [
                    self.MAPPER.assemble_id((chip_r, chip_c), (local_r, local_c))
                    for local_r in range(self.CORE_ROWS_PER_CHIP)
                    for local_c in range(self.CORE_COLS_PER_CHIP)
                ]
                chip_entries.append(
                    {
                        "chip_id": chip_id,
                        "chip_coords": [chip_r, chip_c],
                        "core_rows": self.CORE_ROWS_PER_CHIP,
                        "core_cols": self.CORE_COLS_PER_CHIP,
                        "core_count": len(core_ids),
                        "core_ids": core_ids,
                    }
                )

        chip_by_id = {chip["chip_id"]: chip for chip in chip_entries}
        chip_by_coords = {
            tuple(chip["chip_coords"]): chip
            for chip in chip_entries
        }

        def build_group(group_id, chips):
            core_ids = [
                core_id for chip in chips for core_id in chip["core_ids"]
            ]
            return {
                "group_id": group_id,
                "chips_per_group": self.PHASE1_CHIPS_PER_GROUP,
                "expected_cores_per_chip": self.PHASE1_EXPECTED_CORES_PER_CHIP,
                "cores_per_chip": cores_per_chip,
                "complete": len(chips) == self.PHASE1_CHIPS_PER_GROUP,
                "shape_valid": cores_per_chip == self.PHASE1_EXPECTED_CORES_PER_CHIP,
                "chip_ids": [chip["chip_id"] for chip in chips],
                "chip_count": len(chips),
                "core_ids": core_ids,
                "core_count": len(core_ids),
                "chips": chips,
            }

        if explicit_groups:
            groups = []
            for index, group in enumerate(explicit_groups):
                if not isinstance(group, dict):
                    raise ValueError("Phase1 group entries must be objects.")
                raw_group_id = group.get("group_id", group.get("id", index))
                if isinstance(raw_group_id, bool):
                    raise ValueError(f"Phase1 group id must be an integer, got {raw_group_id!r}.")
                group_id = int(raw_group_id)
                if "chip_ids" in group:
                    chips = [chip_by_id[int(chip_id)] for chip_id in group["chip_ids"]]
                else:
                    coords = group.get("chip_coords", [])
                    chips = [
                        chip_by_coords[tuple(coord)]
                        for coord in coords
                    ]
                if not chips:
                    raise ValueError(f"Phase1 group {group_id} must contain chips.")
                groups.append(build_group(group_id, chips))
            return groups

        groups = []
        for start in range(0, len(chip_entries), self.PHASE1_CHIPS_PER_GROUP):
            chips = chip_entries[start : start + self.PHASE1_CHIPS_PER_GROUP]
            group_id = start // self.PHASE1_CHIPS_PER_GROUP
            groups.append(build_group(group_id, chips))
        return groups

    def _phase1_group_id_for_chip(self, chip_row: int, chip_col: int) -> int:
        chip_id = chip_row * self.CHIP_COLS + chip_col
        return chip_id // self.PHASE1_CHIPS_PER_GROUP

    def _phase1_group_by_id(self, group_id: int) -> dict:
        for group in self.PHASE1_CHIP_GROUPS:
            if int(group["group_id"]) == int(group_id):
                return group
        raise ValueError(f"Phase1 chip_group_id {group_id} is outside topology.")

    def get_topology_snapshot(self):
        self._require_topology_dimensions()

        phase1_nodes = []
        for coords, info in sorted(
            self.EXTERNAL_NODES.items(), key=lambda item: (item[0][0], item[0][1])
        ):
            if info.get("device_type") not in ("PHASE1_LANE", "PHASE1_SINK"):
                continue
            phase1_nodes.append(
                {
                    "id": info.get("name_id"),
                    "device_type": info.get("device_type"),
                    "coord": list(coords),
                    "target": info.get("target"),
                    "chip_group_id": info.get("chip_group_id"),
                    "chip_coords": list(info.get("chip_coords", ())),
                    "router_coords": list(info.get("router_coords", ())),
                    "port": info.get("port"),
                    "role": info.get("sink_role"),
                }
            )

        return {
            "chip_rows": self.CHIP_ROWS,
            "chip_cols": self.CHIP_COLS,
            "core_rows_per_chip": self.CORE_ROWS_PER_CHIP,
            "core_cols_per_chip": self.CORE_COLS_PER_CHIP,
            "cores_per_chip": self.get_cores_per_chip(),
            "chip_count": self.get_num_chips(),
            "core_count": self.get_num_cores(),
            "node_chip_rows": self.NODE_CHIP_ROWS,
            "node_chip_cols": self.NODE_CHIP_COLS,
            "phase1": {
                "chips_per_group": self.PHASE1_CHIPS_PER_GROUP,
                "expected_cores_per_chip": self.PHASE1_EXPECTED_CORES_PER_CHIP,
                "chip_groups": self.PHASE1_CHIP_GROUPS,
                "external_nodes": phase1_nodes,
            },
        }

    def get_total_users(self):
        """
        计算全局总用户数。
        Dense 节点：各自为战，每个 Host 独立生成 NUM_USERS 个。
        MoE 节点：由 Dispatcher 统一发牌，所有 MoE 节点共享 NUM_USERS 个。
        """
        dense_explicit_uids: set[int] = set()
        dense_implicit_total = 0
        moe_hosts_count = 0

        moe_host_ids = self.get_moe_host_ids()
        for host in self.HOSTS_CONFIG:
            if str(host.get("id", "")).strip().upper() in moe_host_ids:
                moe_hosts_count += 1
            else:
                user_ids = host.get("user_ids")
                if isinstance(user_ids, (list, tuple)):
                    dense_explicit_uids.update(int(uid) for uid in user_ids)
                else:
                    user_range = host.get("user_range")
                    if isinstance(user_range, (list, tuple)) and len(user_range) == 2:
                        dense_implicit_total += max(0, int(user_range[1]) - int(user_range[0]))
                    else:
                        dense_implicit_total += self.NUM_USERS

        # 1. 传统 Dense 节点各自生成
        total = dense_implicit_total + len(dense_explicit_uids)
        for lane in self.PHASE1_LANES_CONFIG:
            if lane.get("auto_start", True) is False:
                continue
            user_ids = lane.get("user_ids")
            if isinstance(user_ids, (list, tuple)):
                total += len(set(int(uid) for uid in user_ids))
                continue
            user_range = lane.get("user_range")
            if isinstance(user_range, (list, tuple)) and len(user_range) == 2:
                total += max(0, int(user_range[1]) - int(user_range[0]))
            else:
                total += self.NUM_USERS

        # 2. Legacy MoE nodes are driven by MoEDispatcher.  In Phase1 ->
        # EthSwitch -> Phase2 mode these Hosts are consumers of external
        # requests, so they must not add a second synthetic user stream.
        if moe_hosts_count > 0 and not self.DISABLE_MOE_DISPATCHER:
            total += self.NUM_USERS

        if total == 0:
            raise RuntimeError("Topology Map must define at least one runnable Host.")

        return total

    def get_moe_host_ids(self) -> set[str]:
        if self.MOE_EP_GROUPS:
            return {
                str(host_id).strip().upper()
                for host_id in self.MOE_HOST_BINDINGS
            }
        return {
            str(host.get("id", "")).strip().upper()
            for host in self.HOSTS_CONFIG
            if host.get("is_moe")
        }

    def _validate_phase1_lane_user_ranges(self) -> None:
        seen: dict[int, str] = {}
        for lane in self.PHASE1_LANES_CONFIG:
            if lane.get("auto_start", True) is False:
                continue
            lane_id = str(lane.get("id", "PHASE1_LANE"))
            user_ids = lane.get("user_ids")
            user_range = lane.get("user_range")
            if user_ids is not None and user_range is not None:
                raise ValueError(
                    f"Phase1 lane '{lane_id}' must not define both user_ids and user_range."
                )
            if user_ids is not None:
                if not isinstance(user_ids, (list, tuple)):
                    raise ValueError(
                        f"Phase1 lane '{lane_id}' user_ids must be a list of integers."
                    )
                normalized_ids = []
                for raw_uid in user_ids:
                    if isinstance(raw_uid, bool):
                        raise ValueError(
                            f"Phase1 lane '{lane_id}' user_ids must contain integers."
                        )
                    uid = int(raw_uid)
                    if uid < 0:
                        raise ValueError(
                            f"Phase1 lane '{lane_id}' user_ids must be non-negative."
                        )
                    normalized_ids.append(uid)
            elif user_range is None:
                start, end = 0, int(self.NUM_USERS)
                normalized_ids = range(start, end)
            elif isinstance(user_range, (list, tuple)) and len(user_range) == 2:
                start, end = int(user_range[0]), int(user_range[1])
                if end < start:
                    raise ValueError(
                        f"Phase1 lane '{lane_id}' user_range end must be >= start."
                    )
                normalized_ids = range(start, end)
            else:
                raise ValueError(
                    f"Phase1 lane '{lane_id}' user_range must be [start, end)."
                )
            local_seen: set[int] = set()
            for uid in normalized_ids:
                if uid in local_seen:
                    raise ValueError(
                        f"Phase1 lane '{lane_id}' user_ids must not contain duplicate uid {uid}."
                    )
                local_seen.add(uid)
                previous = seen.get(uid)
                if previous is not None:
                    raise ValueError(
                        "Auto-start Phase1 lanes must use non-overlapping uid ranges/user_ids; "
                        f"uid {uid} appears in both '{previous}' and '{lane_id}'."
                    )
                seen[uid] = lane_id

    # 几何映射与寻路推断引擎
    @staticmethod
    def _validate_external_node(item):
        """Validate the minimum runnable Host/Out schema before mapping.

        Keep this check centralized so old-map edit compatibility does not
        leak into runtime. TUI/GUI may open incomplete maps for editing, but
        the simulator requires these fields to map PCIe entry/exit behavior to
        concrete hardware-visible endpoints.
        """
        node_id = item.get("id")
        if not str(node_id).strip():
            raise ValueError("External node must explicitly define non-empty 'id'.")
        for field in ("coord", "target", "volume"):
            if field not in item or item[field] is None:
                raise ValueError(f"External node '{node_id}' is missing required '{field}'.")
        coord = item["coord"]
        if not isinstance(coord, (list, tuple)) or len(coord) != 2:
            raise ValueError(f"External node '{node_id}'.coord must be a 2-item list.")

    def _deduce_node_mapping(self, item, device_type):
        ext_r, ext_c = item["coord"]
        target_core = item["target"]
        if not self.MAPPER:
            raise ImportError(f"🚨 Mapper没有初始化")

        #  先调用现成方法, 拿到目标 Core 的全局绝对坐标 (Row, Col)
        abs_r, abs_c = self.get_coords_by_id(target_core)

        chip_r, chip_c = self.MAPPER.global_coords_to_chip_coords(abs_r, abs_c)
        router_r, router_c = self.MAPPER.global_coords_to_local_coords(
            abs_r, abs_c)

        # 计算网关 Router 的全局绝对坐标 (其实也就是目标 Core 的绝对坐标)
        gateway_r = abs_r
        gateway_c = abs_c

        # 5. 几何推断：外部节点相对于网关在哪个方向？
        if ext_c < gateway_c:
            port = "PCIE_WEST"
        elif ext_c > gateway_c:
            port = "PCIE_EAST"
        elif ext_r < gateway_r:
            port = "PCIE_NORTH"
        elif ext_r > gateway_r:
            port = "PCIE_SOUTH"
        else:
            port = "UNKNOWN"

        return {
            "device_type": device_type,
            "chip_coords": (chip_r, chip_c),
            "router_coords": (router_r, router_c),
            "port": port,
        }

    @staticmethod
    def _normalize_pcie_id(value, where: str) -> str:
        if value is None:
            raise ValueError(f"{where} is required.")
        text = str(value).strip().upper()
        if not text:
            raise ValueError(f"{where} must be non-empty.")
        return text

    @staticmethod
    def _read_pcie_int(value, where: str, *, positive: bool) -> int:
        if isinstance(value, bool) or not isinstance(value, int):
            raise ValueError(f"{where} must be an integer.")
        if positive and value <= 0:
            raise ValueError(f"{where} must be positive.")
        if not positive and value < 0:
            raise ValueError(f"{where} must be non-negative.")
        return int(value)

    @staticmethod
    def _read_pcie_coord(value, where: str) -> tuple[int, int]:
        if not isinstance(value, (list, tuple)) or len(value) != 2:
            raise ValueError(f"{where}.coord must be a 2-item list.")
        row, col = value
        if any(isinstance(v, bool) or not isinstance(v, int) for v in (row, col)):
            raise ValueError(f"{where}.coord values must be integers.")
        return int(row), int(col)

    def _load_pcie_switch_topology(self, global_rows: int, global_cols: int) -> None:
        raw_switches = self.MAP_PARSER.get_pcie_switches()
        raw_links = self.MAP_PARSER.get_pcie_links()
        raw_routes = self.MAP_PARSER.get_pcie_routes()
        raw_flows = list(self.MAP_PARSER.get_pcie_flows())

        self.PCIE_SWITCHES_CONFIG = []
        self.PCIE_LINKS_CONFIG = []
        self.PCIE_ROUTES_CONFIG = []
        self.PCIE_FLOWS_CONFIG = []
        self.PCIE_ROUTE_TABLES = {}
        self.PCIE_INPUT_ROUTE_TABLES = {}
        self.PCIE_SWITCH_ATTACHED_EXTERNALS = set()
        self.PCIE_HOST_GROUP_TARGETS = {}

        if not raw_switches and not raw_links and not raw_routes and not raw_flows:
            return
        if not raw_switches:
            raise ValueError("pcie_switches is required when pcie_links/routes/flows are present.")

        switches_by_key = {}
        for index, item in enumerate(raw_switches):
            where = f"pcie_switches[{index}]"
            switch_id = self._normalize_pcie_id(item.get("id"), f"{where}.id")
            if switch_id in switches_by_key:
                raise ValueError(f"Duplicate PCIE_SW id: {item.get('id')}")

            chip_value = item.get("chip", item.get("chip_id"))
            chip_id = None
            if chip_value is not None:
                chip_id = self._read_pcie_int(chip_value, f"{where}.chip", positive=False)
                if chip_id >= self.CHIP_ROWS * self.CHIP_COLS:
                    raise ValueError(
                        f"{where}.chip {chip_id} is outside chip range 0~{self.CHIP_ROWS * self.CHIP_COLS - 1}."
                    )

            switch_bandwidth = None
            switch_delay = None
            if "bandwidth" in item and item.get("bandwidth") is not None:
                switch_bandwidth = self._read_pcie_int(item.get("bandwidth"), f"{where}.bandwidth", positive=True)
            if "delay" in item and item.get("delay") is not None:
                switch_delay = self._read_pcie_int(item.get("delay"), f"{where}.delay", positive=False)

            if item.get("coord") is None:
                # Internal simulator-only anchor; map authors do not need to place PCIe fabric in 2D space.
                coord = (-(index + 1), -1)
            else:
                coord = self._read_pcie_coord(item.get("coord"), where)
                row, col = coord
                if 0 <= row < global_rows and 0 <= col < global_cols:
                    raise ValueError(
                        f"PCIE_SW '{item.get('id')}' coord {coord} overlaps the core array."
                    )

            normalized = {"id": str(item.get("id")).strip(), "key": switch_id, "coord": coord}
            if chip_id is not None:
                normalized["chip"] = chip_id
            if switch_bandwidth is not None:
                normalized["bandwidth"] = switch_bandwidth
            if switch_delay is not None:
                normalized["delay"] = switch_delay
            switches_by_key[switch_id] = normalized
            self.PCIE_SWITCHES_CONFIG.append(normalized)

            nested_flows = item.get("flows", [])
            if nested_flows is None:
                nested_flows = []
            if not isinstance(nested_flows, list):
                raise ValueError(f"{where}.flows must be a list.")
            for flow_index, flow in enumerate(nested_flows):
                flow_where = f"{where}.flows[{flow_index}]"
                if not isinstance(flow, dict):
                    raise ValueError(f"{flow_where} must be an object.")
                if "bandwidth" in flow or "delay" in flow:
                    raise ValueError(f"{flow_where} must not carry bandwidth/delay; define them on {where}.")
                if switch_bandwidth is None:
                    raise ValueError(f"{where}.bandwidth is required when flows are nested.")
                if switch_delay is None:
                    raise ValueError(f"{where}.delay is required when flows are nested.")
                normalized_flow = dict(flow)
                flow_switch = normalized_flow.get("switch", switch_id)
                if self._normalize_pcie_id(flow_switch, f"{flow_where}.switch") != switch_id:
                    raise ValueError(f"{flow_where}.switch must match parent PCIE_SW '{item.get('id')}'.")
                normalized_flow["switch"] = switch_id
                normalized_flow["bandwidth"] = switch_bandwidth
                normalized_flow["delay"] = switch_delay
                normalized_flow["__where"] = flow_where
                raw_flows.append(normalized_flow)

        hosts_by_key = {
            self._normalize_pcie_id(host.get("id"), "Host.id"): host
            for host in self.HOSTS_CONFIG
        }
        outs_by_key = {
            self._normalize_pcie_id(out.get("id"), "Out.id"): out
            for out in self.OUTS_CONFIG
        }
        num_cores = self.get_num_cores()

        def parse_link_endpoint(endpoint, where: str) -> dict:
            if not isinstance(endpoint, dict):
                raise ValueError(f"{where} must be an object.")
            endpoint_type = self._normalize_pcie_id(endpoint.get("type"), f"{where}.type")
            if endpoint_type in ("PCIE_SWITCH", "SWITCH"):
                endpoint_type = "PCIE_SW"

            if endpoint_type == "PCIE_SW":
                switch_id = self._normalize_pcie_id(endpoint.get("id"), f"{where}.id")
                if switch_id not in switches_by_key:
                    raise ValueError(f"{where} references unknown PCIE_SW '{endpoint.get('id')}'.")
                port = self._normalize_pcie_id(endpoint.get("port"), f"{where}.port")
                return {
                    "type": "PCIE_SW",
                    "id": switch_id,
                    "port": port,
                    "coord": switches_by_key[switch_id]["coord"],
                }

            if endpoint_type == "CORE":
                core_id = endpoint.get("id")
                if isinstance(core_id, bool) or not isinstance(core_id, int):
                    raise ValueError(f"{where}.id must be an integer Core id.")
                if core_id < 0 or core_id >= num_cores:
                    raise ValueError(f"{where} references unknown Core {core_id}.")
                port = self._normalize_pcie_id(endpoint.get("port"), f"{where}.port")
                if port not in PCIE_PORT_NAMES:
                    raise ValueError(f"{where}.port must be a PCIE_* Router port.")
                return {
                    "type": "CORE",
                    "id": int(core_id),
                    "port": port,
                    "coord": self.get_coords_by_id(int(core_id)),
                }

            if endpoint_type == "HOST":
                host_id = self._normalize_pcie_id(endpoint.get("id"), f"{where}.id")
                if host_id not in hosts_by_key:
                    raise ValueError(f"{where} references unknown Host '{endpoint.get('id')}'.")
                host = hosts_by_key[host_id]
                return {
                    "type": "HOST",
                    "id": host_id,
                    "map_id": host["id"],
                    "coord": tuple(host["coord"]),
                }

            if endpoint_type == "OUT":
                out_id = self._normalize_pcie_id(endpoint.get("id"), f"{where}.id")
                if out_id not in outs_by_key:
                    raise ValueError(f"{where} references unknown Out '{endpoint.get('id')}'.")
                out = outs_by_key[out_id]
                return {
                    "type": "OUT",
                    "id": out_id,
                    "map_id": out["id"],
                    "coord": tuple(out["coord"]),
                }

            raise ValueError(f"{where}.type must be CORE, HOST, OUT, or PCIE_SW.")

        def parse_route_destination(destination, where: str) -> dict:
            if not isinstance(destination, dict):
                raise ValueError(f"{where} must be an object.")
            endpoint_type = self._normalize_pcie_id(destination.get("type"), f"{where}.type")
            if endpoint_type in ("PCIE_SW", "PCIE_SWITCH", "SWITCH"):
                raise ValueError(f"{where} cannot route to another PCIE_SW as a final destination.")
            if endpoint_type == "CORE":
                core_id = destination.get("id")
                if isinstance(core_id, bool) or not isinstance(core_id, int):
                    raise ValueError(f"{where}.id must be an integer Core id.")
                if core_id < 0 or core_id >= num_cores:
                    raise ValueError(f"{where} references unknown Core {core_id}.")
                return {
                    "type": "CORE",
                    "id": int(core_id),
                    "coord": self.get_coords_by_id(int(core_id)),
                }
            if endpoint_type == "HOST":
                host_id = self._normalize_pcie_id(destination.get("id"), f"{where}.id")
                if host_id not in hosts_by_key:
                    raise ValueError(f"{where} references unknown Host '{destination.get('id')}'.")
                host = hosts_by_key[host_id]
                return {
                    "type": "HOST",
                    "id": host_id,
                    "map_id": host["id"],
                    "coord": tuple(host["coord"]),
                }
            if endpoint_type == "OUT":
                out_id = self._normalize_pcie_id(destination.get("id"), f"{where}.id")
                if out_id not in outs_by_key:
                    raise ValueError(f"{where} references unknown Out '{destination.get('id')}'.")
                out = outs_by_key[out_id]
                return {
                    "type": "OUT",
                    "id": out_id,
                    "map_id": out["id"],
                    "coord": tuple(out["coord"]),
                }
            raise ValueError(f"{where}.type must be CORE, HOST, or OUT.")

        def port_to_chip_rule_direction(port: str) -> str:
            mapping = {
                "PCIE_EAST": "RIGHT",
                "PCIE_WEST": "LEFT",
                "PCIE_SOUTH": "BOTTOM",
                "PCIE_NORTH": "TOP",
            }
            try:
                return mapping[port]
            except KeyError as exc:
                raise ValueError(f"Only PCIE_NORTH/SOUTH/EAST/WEST can be used as chip PCIe interfaces, got {port}.") from exc

        def validate_core_endpoint_for_switch(switch_id: str, core_ep: dict, where: str) -> None:
            switch_cfg = switches_by_key[switch_id]
            switch_chip = switch_cfg.get("chip")
            abs_r, abs_c = core_ep["coord"]
            core_chip_coords = self.MAPPER.global_coords_to_chip_coords(abs_r, abs_c)
            core_chip = core_chip_coords[0] * self.CHIP_COLS + core_chip_coords[1]
            if switch_chip is not None and core_chip != switch_chip:
                raise ValueError(
                    f"{where} Core {core_ep['id']} belongs to chip {core_chip}, but PCIE_SW {switch_id} is bound to chip {switch_chip}."
                )
            local_r, local_c = self.MAPPER.global_coords_to_local_coords(abs_r, abs_c)
            local_core = local_r * self.CORE_COLS_PER_CHIP + local_c
            direction = port_to_chip_rule_direction(core_ep["port"])
            rules = self.CHIP_RULES.get(str(core_chip), self.CHIP_RULES.get(core_chip, {}))
            rule_value = rules.get(direction)
            if rule_value is None or str(rule_value).strip().upper() == "NONE":
                raise ValueError(
                    f"{where} Core {core_ep['id']} uses {core_ep['port']}, but chip {core_chip} {direction} has no explicit PCIe interface rule."
                )
            local_cores = parse_chip_local_core_rule(
                rule_value,
                self.CORE_ROWS_PER_CHIP * self.CORE_COLS_PER_CHIP,
                f"chip {core_chip} {direction} rule",
            )
            if local_core not in local_cores:
                raise ValueError(
                    f"{where} Core {core_ep['id']} local core {local_core} is not listed in chip {core_chip} {direction} rule {rule_value!r}."
                )

        generated_links = []
        generated_routes = []
        generated_input_routes = []
        normalized_flows = []
        flow_edges = {}
        generated_link_by_port = {}

        def endpoint_identity(endpoint: dict) -> tuple:
            return (endpoint["type"], endpoint.get("id"), endpoint.get("port"))

        def add_generated_link(link_id: str, a: dict, b: dict, bandwidth: int, delay: int, where: str) -> None:
            switch_eps = [ep for ep in (a, b) if ep["type"] == "PCIE_SW"]
            if not switch_eps:
                raise ValueError(f"{where} generated link must include a PCIE_SW endpoint.")
            for ep in switch_eps:
                key = (ep["id"], ep["port"])
                peer = b if ep is a else a
                peer_key = (peer["type"], peer.get("id"), peer.get("port"))
                previous = generated_link_by_port.get(key)
                if previous is not None:
                    if previous != (peer_key, bandwidth, delay):
                        raise ValueError(
                            f"{where} conflicts with another PCIE flow on {ep['id']}:{ep['port']}."
                        )
                    return
            generated_links.append({"id": link_id, "a": a, "b": b, "bandwidth": bandwidth, "delay": delay})
            for ep in switch_eps:
                peer = b if ep is a else a
                generated_link_by_port[(ep["id"], ep["port"])] = (
                    (peer["type"], peer.get("id"), peer.get("port")),
                    bandwidth,
                    delay,
                )

        for index, flow in enumerate(raw_flows):
            where = flow.get("__where", f"pcie_flows[{index}]")
            flow_id = self._normalize_pcie_id(flow.get("id", f"F{index}"), f"{where}.id")
            switch_id = self._normalize_pcie_id(flow.get("switch"), f"{where}.switch")
            if switch_id not in switches_by_key:
                raise ValueError(f"{where} references unknown PCIE_SW '{flow.get('switch')}'.")
            in_port = self._normalize_pcie_id(flow.get("in_port", flow.get("input_port")), f"{where}.in_port")
            out_port = self._normalize_pcie_id(flow.get("out_port", flow.get("output_port")), f"{where}.out_port")
            if in_port == out_port:
                raise ValueError(f"{where} in_port and out_port must be different.")
            switch_cfg = switches_by_key[switch_id]
            bandwidth_value = flow.get("bandwidth", switch_cfg.get("bandwidth"))
            delay_value = flow.get("delay", switch_cfg.get("delay"))
            if bandwidth_value is None:
                raise ValueError(f"{where}.bandwidth is required, either on the flow or parent PCIE_SW.")
            if delay_value is None:
                raise ValueError(f"{where}.delay is required, either on the flow or parent PCIE_SW.")
            bandwidth = self._read_pcie_int(bandwidth_value, f"{where}.bandwidth", positive=True)
            delay = self._read_pcie_int(delay_value, f"{where}.delay", positive=False)
            source = parse_link_endpoint(flow.get("source"), f"{where}.source")
            destination = parse_link_endpoint(flow.get("destination"), f"{where}.destination")
            current_in = {"type": "PCIE_SW", "id": switch_id, "port": in_port, "coord": switches_by_key[switch_id]["coord"]}
            current_out = {"type": "PCIE_SW", "id": switch_id, "port": out_port, "coord": switches_by_key[switch_id]["coord"]}

            if source["type"] == "CORE":
                validate_core_endpoint_for_switch(switch_id, source, f"{where}.source")
            if destination["type"] == "CORE":
                validate_core_endpoint_for_switch(switch_id, destination, f"{where}.destination")

            add_generated_link(f"{flow_id}_IN", source, current_in, bandwidth, delay, f"{where}.source")
            add_generated_link(f"{flow_id}_OUT", current_out, destination, bandwidth, delay, f"{where}.destination")

            edge_key = (switch_id, in_port)
            edge = {
                "out_port": out_port,
                "source": source,
                "destination": destination,
                "where": where,
            }
            existing_edges = flow_edges.setdefault(edge_key, [])
            for previous in existing_edges:
                if endpoint_identity(previous["source"]) != endpoint_identity(source):
                    raise ValueError(
                        f"{where} reuses PCIE_SW {switch_id}:{in_port} with a different source peer."
                    )
                if endpoint_identity(previous["destination"]) == endpoint_identity(destination):
                    raise ValueError(
                        f"{where} duplicates destination on PCIE_SW {switch_id}:{in_port}."
                    )
            existing_edges.append(edge)
            normalized = {
                "id": flow_id,
                "switch": switch_id,
                "in_port": in_port,
                "source": source,
                "out_port": out_port,
                "destination": destination,
                "bandwidth": bandwidth,
                "delay": delay,
            }
            normalized_flows.append(normalized)

        resolving = set()
        resolved = {}

        def reachable_finals(edge_key):
            if edge_key in resolved:
                return resolved[edge_key]
            if edge_key in resolving:
                raise ValueError(f"PCIE flow cycle detected at {edge_key[0]}:{edge_key[1]}.")
            edges = flow_edges.get(edge_key)
            if not edges:
                raise ValueError(
                    f"PCIE flow reaches {edge_key[0]}:{edge_key[1]}, but that input port has no forwarding rule."
                )
            resolving.add(edge_key)
            finals = []
            seen = set()
            for edge in edges:
                dest = edge["destination"]
                if dest["type"] == "PCIE_SW":
                    edge_finals = reachable_finals((dest["id"], dest["port"]))
                else:
                    edge_finals = [dest]
                for final in edge_finals:
                    final_key = endpoint_identity(final)
                    if final_key not in seen:
                        seen.add(final_key)
                        finals.append(final)
            resolving.remove(edge_key)
            resolved[edge_key] = finals
            return finals

        def finals_for_edge(edge):
            dest = edge["destination"]
            if dest["type"] == "PCIE_SW":
                return reachable_finals((dest["id"], dest["port"]))
            return [dest]

        for edge_key, edges in flow_edges.items():
            for edge in edges:
                for final in finals_for_edge(edge):
                    generated_input_routes.append(
                        {
                            "switch": edge_key[0],
                            "ingress_port": edge_key[1],
                            "port": edge["out_port"],
                            "destination": final,
                        }
                    )

        host_group_targets = {}
        for edge_key, edges in flow_edges.items():
            for edge in edges:
                source = edge["source"]
                if source["type"] != "HOST":
                    continue
                host_id = source["id"]
                for final in finals_for_edge(edge):
                    if final["type"] != "CORE":
                        continue
                    group_id = self.MAP_PARSER.get_core_group_id(final["id"])
                    if group_id is None:
                        continue
                    group_key = str(group_id).strip().upper()
                    if not group_key:
                        raise ValueError(f"Core {final["id"]} has an empty group_id for PCIE Host routing.")
                    host_targets = host_group_targets.setdefault(host_id, {})
                    previous = host_targets.get(group_key)
                    if previous is not None and previous != final["id"]:
                        raise ValueError(
                            f"Host {host_id} PCIE flows map EPGroup {group_id} to multiple Core targets: "
                            f"{previous} and {final["id"]}."
                        )
                    host_targets[group_key] = final["id"]


        self.PCIE_HOST_GROUP_TARGETS = host_group_targets
        self.PCIE_FLOWS_CONFIG = normalized_flows
        all_raw_links = list(raw_links) + generated_links
        all_raw_routes = list(raw_routes) + generated_routes

        seen_link_ids = set()
        seen_switch_ports = set()
        seen_final_endpoints = set()
        switch_ports = {key: set() for key in switches_by_key}
        switch_port_peers = {}
        core_switch_links_by_core = {}
        final_destinations = {}
        attached_external = set()

        for index, link in enumerate(all_raw_links):
            where = f"pcie_links[{index}]"
            link_id = self._normalize_pcie_id(link.get("id"), f"{where}.id")
            if link_id in seen_link_ids:
                raise ValueError(f"Duplicate pcie_link id: {link.get('id')}")
            seen_link_ids.add(link_id)
            for field in ("bandwidth", "delay"):
                if field not in link or link[field] is None:
                    raise ValueError(f"{where}.{field} is required.")
            bandwidth = self._read_pcie_int(link["bandwidth"], f"{where}.bandwidth", positive=True)
            delay = self._read_pcie_int(link["delay"], f"{where}.delay", positive=False)
            a = parse_link_endpoint(link.get("a"), f"{where}.a")
            b = parse_link_endpoint(link.get("b"), f"{where}.b")
            switch_endpoints = [ep for ep in (a, b) if ep["type"] == "PCIE_SW"]
            if not switch_endpoints:
                raise ValueError(f"{where} must connect at least one PCIE_SW endpoint.")
            if len(switch_endpoints) > 2:
                raise ValueError(f"{where} has too many PCIE_SW endpoints.")

            for ep in switch_endpoints:
                switch_key = (ep["id"], ep["port"])
                if switch_key in seen_switch_ports:
                    raise ValueError(
                        f"PCIE_SW {ep['id']} port {ep['port']} is connected by multiple links."
                    )
                seen_switch_ports.add(switch_key)
                switch_ports[ep["id"]].add(ep["port"])
                switch_port_peers[switch_key] = b if ep is a else a

            for ep in (a, b):
                if ep["type"] == "PCIE_SW":
                    continue
                if ep["type"] == "CORE":
                    final_key = ("CORE", ep["id"], ep["port"])
                    core_switch_links_by_core.setdefault(ep["id"], []).append(ep)
                    label = f"Core {ep['id']}"
                    for sw_ep in switch_endpoints:
                        validate_core_endpoint_for_switch(sw_ep["id"], ep, where)
                else:
                    final_key = (ep["type"], ep["id"])
                    attached_external.add(final_key)
                    label = f"{ep['type']} {ep['map_id']}"
                if final_key in seen_final_endpoints:
                    raise ValueError(f"{where} connects duplicate endpoint {final_key}.")
                seen_final_endpoints.add(final_key)
                coord = tuple(ep["coord"])
                previous = final_destinations.get(coord)
                if previous is not None and previous != label:
                    raise ValueError(
                        f"PCIE_SW destinations {previous} and {label} share coord {coord}."
                    )
                final_destinations[coord] = label

            self.PCIE_LINKS_CONFIG.append(
                {"id": link_id, "a": a, "b": b, "bandwidth": bandwidth, "delay": delay}
            )

        route_tables = {key: {} for key in switches_by_key}
        input_route_tables = {key: {} for key in switches_by_key}
        normalized_routes = []
        for index, route in enumerate(all_raw_routes):
            where = f"pcie_routes[{index}]"
            switch_id = self._normalize_pcie_id(route.get("switch"), f"{where}.switch")
            if switch_id not in switches_by_key:
                raise ValueError(f"{where} references unknown PCIE_SW '{route.get('switch')}'.")
            port = self._normalize_pcie_id(route.get("port"), f"{where}.port")
            if port not in switch_ports[switch_id]:
                raise ValueError(f"{where}.port {port} is not connected on PCIE_SW {switch_id}.")
            destinations = route.get("destinations")
            if not isinstance(destinations, list) or not destinations:
                raise ValueError(f"{where}.destinations must be a non-empty list.")
            egress_peer = switch_port_peers[(switch_id, port)]
            normalized_destinations = []
            for dest_index, destination in enumerate(destinations):
                dest = parse_route_destination(destination, f"{where}.destinations[{dest_index}]")
                coord = tuple(dest["coord"])
                if egress_peer["type"] != "PCIE_SW" and tuple(egress_peer["coord"]) != coord:
                    raise ValueError(
                        f"{where}.port {port} exits to {egress_peer['type']} "
                        f"endpoint at {tuple(egress_peer['coord'])}, but destination "
                        f"{coord} would re-enter Router/NoC after PCIe. PCIE_SW routes "
                        "must exit directly at the destination endpoint or continue to another PCIE_SW."
                    )
                previous = route_tables[switch_id].get(coord)
                if previous is not None:
                    raise ValueError(
                        f"PCIE_SW {switch_id} has duplicate route for destination {coord}."
                    )
                route_tables[switch_id][coord] = port
                normalized_destinations.append(dest)
            normalized_routes.append(
                {"switch": switch_id, "port": port, "destinations": normalized_destinations}
            )

        for index, route in enumerate(generated_input_routes):
            where = f"pcie_flows route[{index}]"
            switch_id = route["switch"]
            ingress_port = route["ingress_port"]
            port = route["port"]
            dest = route["destination"]
            coord = tuple(dest["coord"])
            if ingress_port not in switch_ports[switch_id]:
                raise ValueError(f"{where}.ingress_port {ingress_port} is not connected on PCIE_SW {switch_id}.")
            if port not in switch_ports[switch_id]:
                raise ValueError(f"{where}.port {port} is not connected on PCIE_SW {switch_id}.")
            egress_peer = switch_port_peers[(switch_id, port)]
            if egress_peer["type"] != "PCIE_SW" and tuple(egress_peer["coord"]) != coord:
                raise ValueError(
                    f"{where}.port {port} exits to {egress_peer['type']} endpoint at "
                    f"{tuple(egress_peer['coord'])}, but destination {coord} would re-enter Router/NoC after PCIe."
                )
            route_key = (ingress_port, coord)
            previous = input_route_tables[switch_id].get(route_key)
            if previous is not None and previous != port:
                raise ValueError(
                    f"PCIE_SW {switch_id} has duplicate input route {ingress_port}->{coord}."
                )
            input_route_tables[switch_id][route_key] = port

        if not all_raw_routes and not generated_input_routes:
            raise ValueError("pcie_routes or pcie_flows are required when pcie_links are present.")
        for switch_id in switches_by_key:
            if not switch_ports[switch_id]:
                raise ValueError(f"PCIE_SW {switch_id} has no connected ports.")
            if not raw_flows:
                missing = [
                    label
                    for coord, label in sorted(final_destinations.items())
                    if coord not in route_tables[switch_id]
                ]
                if missing:
                    raise ValueError(
                        f"PCIE_SW {switch_id} missing route for: " + ", ".join(missing)
                    )

        for host_id, group_ids in self.MOE_HOST_BINDINGS.items():
            host_key = self._normalize_pcie_id(host_id, "MoE Host binding id")
            if ("HOST", host_key) not in attached_external:
                continue
            host_targets = host_group_targets.get(host_key, {})
            missing = [
                group_id
                for group_id in group_ids
                if str(group_id).strip().upper() not in host_targets
            ]
            if missing:
                raise ValueError(
                    f"Host {host_id} is attached to PCIE_SW but has no PCIE flow target for EPGroups: "
                    + ", ".join(str(group_id) for group_id in missing)
                )

        for ext_type, ext_id in attached_external:
            node = hosts_by_key[ext_id] if ext_type == "HOST" else outs_by_key[ext_id]
            target_core = int(node["target"])
            core_links = core_switch_links_by_core.get(target_core, [])
            if len(core_links) != 1:
                raise ValueError(
                    f"{ext_type} {node['id']} is attached to PCIE_SW and target Core "
                    f"{target_core} must have exactly one PCIE_SW link; found {len(core_links)}."
                )
            ext_coords = tuple(node["coord"])
            mapping = self.EXTERNAL_NODES.get(ext_coords)
            if mapping is None:
                raise RuntimeError(f"External node {node['id']} was not mapped before PCIE_SW validation.")
            mapping["port"] = core_links[0]["port"]
            mapping["pcie_sw_attached"] = True

        self.PCIE_SWITCH_ATTACHED_EXTERNALS = attached_external
        self.PCIE_ROUTES_CONFIG = normalized_routes
        self.PCIE_ROUTE_TABLES = route_tables
        self.PCIE_INPUT_ROUTE_TABLES = input_route_tables

    @staticmethod
    def _validate_phase1_external_node(item, node_type: str):
        node_id = item.get("id")
        if not str(node_id).strip():
            raise ValueError("Phase1 external node must define non-empty 'id'.")
        for field in ("coord", "target"):
            if field not in item or item[field] is None:
                raise ValueError(
                    f"Phase1 external node '{node_id}' is missing required '{field}'."
                )
        coord = item["coord"]
        if not isinstance(coord, (list, tuple)) or len(coord) != 2:
            raise ValueError(f"Phase1 external node '{node_id}'.coord must be a 2-item list.")
        if node_type == "PHASE1_LANE":
            bypass_target = item.get("bypass_target", item.get("bypass_sink"))
            moe_target = item.get("moe_target", item.get("moe_sink"))
            if bypass_target is None or moe_target is None:
                raise ValueError(
                    f"Phase1 lane '{node_id}' must define both bypass_target and moe_target."
                )
            if str(bypass_target).strip().upper() == str(moe_target).strip().upper():
                raise ValueError(
                    f"Phase1 lane '{node_id}' must use separate bypass and moe targets."
                )
        elif node_type == "PHASE1_SINK":
            role = str(item.get("role", item.get("sink_role", "GENERIC"))).strip().upper()
            valid_roles = {"GENERIC", "MOE", "MOE_MOCK", "BYPASS", "BYPASS_MOCK"}
            if role not in valid_roles:
                raise ValueError(
                    f"Phase1 sink '{node_id}' has invalid role {role!r}; "
                    f"expected one of {sorted(valid_roles)}."
                )

    def update_from_args(self, args):
        """核心魔法：把命令行的参数拍进全局配置里"""
        if args.streams is not None:
            self.STREAM_COUNT = args.streams
        if args.users is not None:
            self.NUM_USERS = args.users
        if args.noc_basic_router_delay is not None:
            self.NOC_BASIC_ROUTER_DELAY = args.noc_basic_router_delay
        if args.crosschip_delay is not None:
            self.CROSS_CHIP_DELAY = args.crosschip_delay
        if args.crossnode_delay is not None:
            self.CROSS_NODE_DELAY = args.crossnode_delay
        if args.dte_reduce_time is not None:
            self.DTE_REDUCE_TIME = args.dte_reduce_time
        dte_execution_mode = getattr(args, "dte_execution_mode", None)
        if dte_execution_mode is not None:
            self.DTE_EXECUTION_MODE = dte_execution_mode
        dte_dsa_mode = getattr(args, "dte_dsa_mode", None)
        if dte_dsa_mode is not None:
            self.DTE_DSA_MODE = dte_dsa_mode
        if args.watchdog_lifespan_limit is not None:
            self.WATCHDOG_LIFESPAN_LIMIT = args.watchdog_lifespan_limit
        if getattr(args, "release", False):
            self.RELEASE_MODE = args.release
        if getattr(args, "dump_software", False):
            self.DUMP_SOFTWARE_MODE = args.dump_software
        if getattr(args, "dump_credit", False):
            self.DUMP_CREDIT_MODE = args.dump_credit
        if args.host_push_delay is not None:
            self.HOST_PUSH_DELAY = args.host_push_delay
        if getattr(args, "skew_factor", None) is not None:
            self.SKEW_FACTOR = args.skew_factor
        if args.hop_cout_err is not None:
            self.HOP_COUNT_ERR = args.hop_cout_err
        if args.hop_cout_war is not None:
            self.HOP_COUNT_WAR = args.hop_cout_war
        if getattr(args, "enable_trace", False):
            self.ENABLE_TRACE = args.enable_trace
        packet_route_trace = getattr(args, "packet_route_trace", None)
        if packet_route_trace is not None:
            if packet_route_trace not in {"auto", "on", "off"}:
                raise ValueError(
                    "packet_route_trace must be one of: auto, on, off"
                )
            self.PACKET_ROUTE_TRACE = packet_route_trace
        if getattr(args, "sweep_mode", False):
            self.SWEEP_MODE = bool(args.sweep_mode)
        # 🐾 新增种子接管
        if getattr(args, "seed", None) is not None:
            self.RANDOM_SEED = args.seed
        if args.no_pipeline_print is not None:
            self.NO_PIPELINE_PRINT = args.no_pipeline_print
        if args.noc_bandwidth is not None:
            self.NOC_BANDWIDTH = args.noc_bandwidth
            self.ROUTER_BUFFER_SIZE = self.NOC_BANDWIDTH
        if getattr(args, "pcie_bandwidth", None) is not None:
            self.PCIE_BANDWIDTH = args.pcie_bandwidth
            self.PCIE_BUFFER_SIZE = self.PCIE_BANDWIDTH
        if args.dte_bandwidth is not None:
            self.DTE_BANDWIDTH = args.dte_bandwidth
        if args.dte_setup_time is not None:
            self._DTE_SETUP_TIME = args.dte_setup_time
        setup_ahead_depth = getattr(args, "setup_ahead_depth", None)
        if setup_ahead_depth is not None:
            self.SETUP_AHEAD_DEPTH = setup_ahead_depth
        if getattr(args, "matrix_time", None) is not None:
            self.MATRIX_TIME = args.matrix_time
        if getattr(args, "vector_time", None) is not None:
            self.VECTOR_TIME = args.vector_time
        if getattr(args, "run_id", None) is not None:
            self.RUN_ID = args.run_id
        if getattr(args, "enable_eth_switch", None) is not None:
            self.ENABLE_ETH_SWITCH = bool(args.enable_eth_switch)
        if getattr(args, "enable_eth_switch_phase2_ingress", None) is not None:
            self.ENABLE_ETH_SWITCH_PHASE2_INGRESS = bool(
                args.enable_eth_switch_phase2_ingress
            )
        if getattr(args, "enable_eth_switch_phase2_host_injection", None) is not None:
            self.ETH_SWITCH_PHASE2_INGRESS_INJECT_HOST = bool(
                args.enable_eth_switch_phase2_host_injection
            )
        if getattr(args, "enable_eth_switch_phase2_output_result_bridge", None) is not None:
            self.ENABLE_ETH_SWITCH_PHASE2_OUTPUT_RESULT_BRIDGE = bool(
                args.enable_eth_switch_phase2_output_result_bridge
            )
        if getattr(args, "enable_eth_switch_phase2_synthetic_result", None) is not None:
            self.ENABLE_ETH_SWITCH_PHASE2_SYNTHETIC_RESULT = bool(
                args.enable_eth_switch_phase2_synthetic_result
            )
            if args.enable_eth_switch_phase2_synthetic_result:
                self.ENABLE_ETH_SWITCH_PHASE2_OUTPUT_RESULT_BRIDGE = True
        if getattr(args, "eth_switch_complete_phase1_boundary", None) is not None:
            self.ETH_SWITCH_COMPLETE_PHASE1_BOUNDARY = bool(
                args.eth_switch_complete_phase1_boundary
            )
        eth_switch_fields = {
            "eth_switch_expert_group_size": "ETH_SWITCH_EXPERT_GROUP_SIZE",
            "eth_switch_expert_group_map": "ETH_SWITCH_EXPERT_GROUP_MAP",
            "eth_switch_switch_processing_delay_ns": "ETH_SWITCH_SWITCH_PROCESSING_DELAY_NS",
            "eth_switch_header_bytes": "ETH_SWITCH_HEADER_BYTES",
            "eth_switch_payload_numel": "ETH_SWITCH_PAYLOAD_NUMEL",
            "eth_switch_payload_bytes_per_elem": "ETH_SWITCH_PAYLOAD_BYTES_PER_ELEM",
            "eth_switch_default_egress_queue_capacity_packets": "ETH_SWITCH_DEFAULT_EGRESS_QUEUE_CAPACITY_PACKETS",
            "eth_switch_default_pending_capacity_packets": "ETH_SWITCH_DEFAULT_PENDING_CAPACITY_PACKETS",
            "eth_switch_propagation_delay_ns": "ETH_SWITCH_PROPAGATION_DELAY_NS",
            "eth_switch_res_ingress_bandwidth_GBps": "ETH_SWITCH_RES_INGRESS_BANDWIDTH_GBps",
            "eth_switch_phase1_moe_ingress_bandwidth_GBps": "ETH_SWITCH_PHASE1_MOE_INGRESS_BANDWIDTH_GBps",
            "eth_switch_phase2_moe_result_ingress_bandwidth_GBps": "ETH_SWITCH_PHASE2_MOE_RESULT_INGRESS_BANDWIDTH_GBps",
            "eth_switch_phase2_moe_request_egress_bandwidth_GBps": "ETH_SWITCH_PHASE2_MOE_REQUEST_EGRESS_BANDWIDTH_GBps",
            "eth_switch_phase3_res_join_egress_bandwidth_GBps": "ETH_SWITCH_PHASE3_RES_JOIN_EGRESS_BANDWIDTH_GBps",
        }
        for arg_field, config_field in eth_switch_fields.items():
            value = getattr(args, arg_field, None)
            if value is not None:
                setattr(self, config_field, value)


# 实例化一个唯一的全局对象，大家以后都从它这儿拿数据
GLOBAL_CONFIG = _SimulationConfig()
