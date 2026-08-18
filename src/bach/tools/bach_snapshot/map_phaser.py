from typing import Any, Dict, Tuple
from hardware_config import EPGroupType, Opcode, UnitType
from config import GLOBAL_CONFIG
from monitor import GLOBAL_MONITOR, LogLevel, SNAPSHOT_CORE_SOFTWARE_QUEUE


def _should_snapshot_software_queue() -> bool:
    return (
        not GLOBAL_CONFIG.RELEASE_MODE
        and not getattr(GLOBAL_CONFIG, "DUMP_SOFTWARE_MODE", False)
        and not getattr(GLOBAL_CONFIG, "DUMP_CREDIT_MODE", False)
        and getattr(GLOBAL_MONITOR, "_is_setup", False)
        and bool(getattr(GLOBAL_MONITOR, "registered_cores", []))
    )


TaskTableEntry = Tuple[UnitType, Opcode, int, int, Any, Tuple[int, int], int]
CreditTargets = Tuple[int, ...]

_task_send_metadata_cached_parser = None
_task_send_metadata_table: Dict[int, Dict[int, dict]] = {}


def build_software_tables():
    global _task_send_metadata_cached_parser, _task_send_metadata_table

    table = {}
    credit_table = {}
    task_send_metadata: Dict[int, Dict[int, dict]] = {}
    parent_map = {}
    host_map = {}  # Store host-specific coordinates here
    out_map: Dict[int, Tuple[Tuple[int, int], Any, int]] = {}

    parser = GLOBAL_CONFIG.MAP_PARSER
    if parser is None:
        raise RuntimeError(
            "🚨 致命错误：TopologyParser 未初始化！必须先调用 GLOBAL_CONFIG.load_topology_map()！"
        )

    raw_data = parser.get_raw_data()
    cores_data = parser.get_all_cores_config()
    meta = parser.get_meta_info()
    timeline = parser.get_timeline()
    timeline_volumes = parser.get_timeline_volumes()
    # 彻底干掉自己找 incoming_edges 的 for 循环，直接获取缓存！
    incoming_edges_per_phase = parser.get_incoming_edges_per_phase()
    reduce_phase_types = {"REDUCE", "REDUCTION", "RES_SUM"}
    res_phase_types = {"RES"}
    bypass_phase_types = {"BYPASS"}

    def has_explicit_phase_entry(config: dict | None) -> bool:
        if not config:
            return False
        timeline_data = config.get("timeline_data", {})
        if timeline_data is None:
            return False
        if not isinstance(timeline_data, dict):
            raise ValueError("Core timeline_data must be an object when present.")
        return any(str(phase_idx) in timeline_data for phase_idx in range(len(timeline)))

    def has_fifo_fanout_entry(config: dict | None) -> bool:
        if not config:
            return False
        timeline_data = config.get("timeline_data", {})
        if timeline_data is None:
            return False
        if not isinstance(timeline_data, dict):
            raise ValueError("Core timeline_data must be an object when present.")

        for phase_idx, phase_type in enumerate(timeline):
            if phase_type != "FIFO_IN":
                continue
            phase_data = timeline_data.get(str(phase_idx))
            if not phase_data:
                continue
            if phase_data.get("targets", []):
                return True
        return False

    def reduce_opcode_for_phase(phase_type: str) -> Opcode:
        return Opcode.REDUCTION if phase_type == "REDUCTION" else Opcode.REDUCE

    # 获取外部节点列表
    hosts_list = parser.get_all_hosts()
    outs_list = parser.get_all_outs()

    volume_phase_types = {
        "INIT",
        "FIFO_IN",
        "CONCAT",
        "REDUCE",
        "REDUCTION",
        "RES_SUM",
        "RES",
        "MOVE",
        "BYPASS",
    }

    def require_phase_volume(phase_idx: int, phase_type: str) -> int:
        # Only phases that physically move bytes require timeline volume.
        # COMPUTE and other control-only phases stay volume-free.
        if phase_type not in volume_phase_types:
            return 0
        if phase_idx >= len(timeline_volumes) or timeline_volumes[phase_idx] is None:
            raise ValueError(
                f"Map meta.timeline_volumes[{phase_idx}] is required for {phase_type} phase."
            )
        return int(timeline_volumes[phase_idx])

    def compute_time_default(op_type: str) -> int:
        # Missing per-op time is allowed only when the map or top-level CLI
        # explicitly provides the matching Matrix/Vector time. There is no
        # hidden zero-cycle compute fallback.
        defaults = meta.get("default_compute_times", {}) or {}
        value = defaults.get(op_type)
        if value is None:
            attr_name = "MATRIX_TIME" if op_type == "M" else "VECTOR_TIME"
            value = getattr(GLOBAL_CONFIG, attr_name, None)
        if value is None:
            raise ValueError(
                f"Compute op {op_type} omits time and no top-level default is configured."
            )
        return int(value)

    def normalize_send_metadata(raw_metadata: Any) -> dict:
        if not isinstance(raw_metadata, dict):
            return {}

        metadata = dict(raw_metadata)
        if "HitMap" in metadata and "hit_map" not in metadata:
            metadata["hit_map"] = metadata["HitMap"]
        if "hitmap" in metadata and "hit_map" not in metadata:
            metadata["hit_map"] = metadata["hitmap"]
        if "lane_id" in metadata and "phase1_lane_id" not in metadata:
            metadata["phase1_lane_id"] = metadata["lane_id"]
        if "group_id" in metadata and "phase1_group_id" not in metadata:
            metadata["phase1_group_id"] = metadata["group_id"]
        return metadata

    def normalize_dte_phase_metadata(phase_type: str, phase_data: Any) -> dict:
        if not isinstance(phase_data, dict):
            phase_data = {}

        metadata = normalize_send_metadata(
            phase_data.get(
                "send_metadata",
                phase_data.get(
                    "packet",
                    phase_data.get("metadata", {}),
                ),
            )
        )
        if phase_data.get("role") and "role" not in metadata:
            metadata["role"] = phase_data.get("role")
        if phase_data.get("packet_type") and "packet_type" not in metadata:
            metadata["packet_type"] = phase_data.get("packet_type")
        if phase_data.get("operand_role") and "operand_role" not in metadata:
            metadata["operand_role"] = phase_data.get("operand_role")
        if phase_data.get("output_role") and "output_role" not in metadata:
            metadata["output_role"] = phase_data.get("output_role")

        if phase_type == "RES_SUM":
            metadata.setdefault("task_name", "res_sum")
            metadata.setdefault("stage", "phase1_res_sum")
            metadata.setdefault("semantic_op", "res_sum")
            metadata.setdefault("task_kind", "VECTOR_ADD_JOIN")
            metadata.setdefault("impl_opcode", "RES")
            metadata.setdefault("expected_inputs", 2)
            metadata.setdefault("path", "RES_SUM")
            metadata.setdefault("numeric_verified", False)
        elif phase_type == "RES":
            metadata.setdefault("task_name", "res")
            metadata.setdefault("stage", "phase1_res")
            metadata.setdefault("semantic_op", "res")
            metadata.setdefault("impl_opcode", "RES")
            metadata.setdefault("path", "RES")
            metadata.setdefault("hit_map", ())
        elif phase_type == "BYPASS":
            metadata.setdefault("task_name", "bypass")
            metadata.setdefault("stage", "phase1_bypass")
            metadata.setdefault("semantic_op", "bypass")
            metadata.setdefault("task_kind", "X1_BYPASS")
            metadata.setdefault("impl_opcode", "BYPASS")
            metadata.setdefault("path", "BYPASS")
            metadata.setdefault("payload_role", "X1")
            metadata.setdefault("input_role", "X1")
            metadata.setdefault("output_role", "X1")
            metadata.setdefault("hit_map", ())
        return metadata

    def resolve_external_endpoint(target):
        if isinstance(target, dict):
            if "coord" not in target:
                raise ValueError(f"Explicit DTE target {target!r} must define coord.")
            dst_coords = tuple(int(v) for v in target["coord"])
            return dst_coords, target.get("id", target.get("target", 0))

        if isinstance(target, (list, tuple)) and len(target) == 2:
            return (int(target[0]), int(target[1])), target

        if isinstance(target, int) or (
            isinstance(target, str) and str(target).strip().isdigit()
        ):
            core_id = int(target)
            return GLOBAL_CONFIG.get_coords_by_id(core_id), core_id

        target_key = str(target).strip().upper()
        endpoint = getattr(GLOBAL_CONFIG, "PHASE1_ENDPOINTS_BY_ID", {}).get(target_key)
        if endpoint is not None:
            return tuple(endpoint["coord"]), endpoint.get("id", target_key)

        for out_entry in getattr(GLOBAL_CONFIG, "OUTS_CONFIG", []):
            if str(out_entry.get("id", "")).strip().upper() == target_key:
                return tuple(out_entry["coord"]), out_entry.get("id", target_key)

        raise ValueError(f"Cannot resolve DTE target {target!r}.")

    base_init_volume = None
    for idx, p_type in enumerate(timeline):
        if p_type in ["INIT", "FIFO_IN"]:
            base_init_volume = require_phase_volume(idx, p_type)
            break

    core_initial_volumes: Dict[int, int] = {}
    res_entry_targets: Dict[int, dict] = {}

    for phase_idx_int, phase_type in enumerate(timeline):
        if phase_type not in res_phase_types:
            continue

        phase_idx = str(phase_idx_int)
        phase_vol = require_phase_volume(phase_idx_int, phase_type)
        edges: Dict[int, list[int]] = {}

        for core_id_str, config in cores_data.items():
            target = config.get("timeline_data", {}).get(phase_idx, {}).get("target")
            if target is None:
                continue
            edges.setdefault(int(target), []).append(int(core_id_str))

        for target, senders in edges.items():
            senders.sort()
            target_phase_data = (
                cores_data.get(str(target), {})
                .get("timeline_data", {})
                .get(phase_idx, {})
            )
            target_metadata = normalize_dte_phase_metadata(
                phase_type, target_phase_data
            )
            recv_init = bool(target_metadata.get("recv_init", False))
            if not recv_init:
                continue
            if len(senders) != 1:
                raise ValueError(
                    f"RES recv_init target Core {target} must have exactly one "
                    f"sender in phase {phase_idx}; got {senders}."
                )
            target_metadata.setdefault("recv_init", True)
            target_metadata.setdefault("task_kind", "RES_RECV_INIT")
            target_metadata.setdefault("input_role", "FC0_OUT")
            target_metadata.setdefault("source_core", senders[0])
            target_metadata.setdefault("target_core", target)
            target_metadata.setdefault("phase_type", phase_type)
            target_metadata.setdefault("phase_idx", phase_idx_int)
            target_metadata.setdefault("target_task_id", 0)
            res_entry_targets[target] = {
                "phase_idx": phase_idx_int,
                "sender": senders[0],
                "volume": phase_vol,
                "metadata": target_metadata,
            }

    # ==========================================
    # 1. MAP PARENTS BY SCANNING ALL INIT & FIFO_IN PHASES
    # ==========================================
    for phase_idx_int, phase_type in enumerate(timeline):
        if phase_type in ["INIT", "FIFO_IN"]:
            phase_idx = str(phase_idx_int)

            phase_vol = require_phase_volume(phase_idx_int, phase_type)

            for p_str, conf in cores_data.items():
                p_id = int(p_str)
                init_targets = (
                    conf.get("timeline_data", {}).get(phase_idx, {}).get("targets", [])
                )
                for child_id in init_targets:
                    parent_map[child_id] = p_id
                    # Record the correct initialization volume for the child core
                    core_initial_volumes[child_id] = phase_vol

    for target, entry in res_entry_targets.items():
        parent_map[target] = int(entry["sender"])
        core_initial_volumes[target] = int(entry["volume"])

    # REDUCTION is also a user-activation edge for ReductionCore. It is not an
    # INIT dependency, but a terminal reduction core should RETIRE to the core
    # that activated it instead of falling back to the physical neighbor rule.
    for phase_idx_int, phase_type in enumerate(timeline):
        if phase_type != "REDUCTION":
            continue
        phase_idx = str(phase_idx_int)
        for p_str, conf in cores_data.items():
            p_id = int(p_str)
            target = conf.get("timeline_data", {}).get(phase_idx, {}).get("target")
            if target is None:
                continue
            parent_map.setdefault(int(target), p_id)
    # ==========================================
    # BULLETPROOF HOST MAPPING & DEBUGGING
    # ==========================================
    for host_entry in hosts_list:
        host_id = host_entry.get("id", "<unknown>")
        if "target" not in host_entry or host_entry["target"] is None:
            raise ValueError(f"Host '{host_id}' is missing required target.")
        if "volume" not in host_entry or host_entry["volume"] is None:
            raise ValueError(f"Host '{host_id}' is missing required volume.")
        target_val = host_entry["target"]
        coords_val = host_entry.get("coord")
        if not isinstance(coords_val, list) or len(coords_val) < 2:
            raise ValueError(f"Host '{host_id}' must define coord as [row, col].")
        if _should_snapshot_software_queue():
            GLOBAL_MONITOR.take_snapshot(
                event_note=f"🛠️  DEBUG: Processing Host -> Extracted Target: {target_val}, Coord: {coords_val}",
                level=LogLevel.DEBUG,
                core_id=SNAPSHOT_CORE_SOFTWARE_QUEUE,
            )

        if target_val is not None:
            t_id = int(target_val)
            parent_map[t_id] = -1  # Assign Host ID as -1

            core_initial_volumes[t_id] = int(host_entry["volume"])

            host_map[t_id] = (int(coords_val[0]), int(coords_val[1]))
            if _should_snapshot_software_queue():
                GLOBAL_MONITOR.take_snapshot(
                    event_note=f"🛠️  DEBUG: Successfully mapped Core {t_id} to Host at {host_map[t_id]}",
                    level=LogLevel.DEBUG,
                    core_id=SNAPSHOT_CORE_SOFTWARE_QUEUE,
                )

    for out_entry in outs_list:
        out_id = out_entry.get("id", "<unknown>")
        if "target" not in out_entry or out_entry["target"] is None:
            raise ValueError(f"Out '{out_id}' is missing required target.")
        if "volume" not in out_entry or out_entry["volume"] is None:
            raise ValueError(f"Out '{out_id}' is missing required volume.")
        coords_val = out_entry.get("coord")
        if not isinstance(coords_val, list) or len(coords_val) < 2:
            raise ValueError(f"Out '{out_id}' must define coord as [row, col].")

        target_core = int(out_entry["target"])
        out_map[target_core] = (
            (int(coords_val[0]), int(coords_val[1])),
            out_id,
            int(out_entry["volume"]),
        )

    hosts_by_key = {
        str(host.get("id", "")).strip().upper(): host
        for host in hosts_list
        if str(host.get("id", "")).strip()
    }

    def apply_host_parent(core_id: int, host_endpoint: dict, where: str) -> None:
        host_key = str(host_endpoint.get("id", host_endpoint.get("map_id", ""))).strip().upper()
        host = hosts_by_key.get(host_key)
        if host is None:
            map_id_key = str(host_endpoint.get("map_id", "")).strip().upper()
            host = hosts_by_key.get(map_id_key)
        if host is None:
            raise RuntimeError(f"{where}: references unknown Host '{host_endpoint.get('id')}'.")

        coords_val = host_endpoint.get("coord", host.get("coord"))
        if not isinstance(coords_val, (list, tuple)) or len(coords_val) < 2:
            raise RuntimeError(f"{where}: Host '{host.get('id')}' must define coord as [row, col].")
        host_coords = (int(coords_val[0]), int(coords_val[1]))
        host_volume = int(host["volume"])

        previous_parent = parent_map.get(core_id)
        if previous_parent is not None and previous_parent != -1:
            raise RuntimeError(
                f"{where}: Core {core_id} already retires to Core {previous_parent}; "
                f"cannot also be activated by Host '{host.get('id')}' through PCIE_SW."
            )
        previous_host_coords = host_map.get(core_id)
        if previous_parent == -1 and previous_host_coords is not None and previous_host_coords != host_coords:
            raise RuntimeError(
                f"{where}: Core {core_id} has multiple Host retire destinations: "
                f"{previous_host_coords} and {host_coords}."
            )
        previous_volume = core_initial_volumes.get(core_id)
        if previous_volume is not None and previous_volume != host_volume:
            raise RuntimeError(
                f"{where}: Core {core_id} has conflicting initial volumes: "
                f"{previous_volume} and {host_volume}."
            )

        parent_map[core_id] = -1
        host_map[core_id] = host_coords
        core_initial_volumes[core_id] = host_volume

    def apply_pcie_host_flow_parents() -> None:
        pcie_flows = getattr(GLOBAL_CONFIG, "PCIE_FLOWS_CONFIG", []) or []
        if not pcie_flows:
            return

        edges_by_ingress = {}
        roots_by_ingress = {}
        pending = []

        def host_key(endpoint: dict) -> str:
            return str(endpoint.get("id", endpoint.get("map_id", ""))).strip().upper()

        def add_root(ingress_key: tuple[str, str], host_endpoint: dict) -> None:
            key = host_key(host_endpoint)
            if not key:
                raise RuntimeError("PCIE flow Host source is missing id.")
            roots = roots_by_ingress.setdefault(ingress_key, {})
            if key not in roots:
                roots[key] = host_endpoint
                pending.append((ingress_key, key))

        for flow in pcie_flows:
            ingress_key = (flow["switch"], flow["in_port"])
            edges_by_ingress.setdefault(ingress_key, []).append(flow)
            source = flow.get("source", {})
            if source.get("type") == "HOST":
                add_root(ingress_key, source)

        processed = set()
        while pending:
            ingress_key, root_key = pending.pop(0)
            process_key = (ingress_key, root_key)
            if process_key in processed:
                continue
            processed.add(process_key)
            host_endpoint = roots_by_ingress[ingress_key][root_key]
            for flow in edges_by_ingress.get(ingress_key, []):
                destination = flow.get("destination", {})
                where = f"PCIE flow {flow.get('id', '<unknown>')}"
                if destination.get("type") == "CORE":
                    apply_host_parent(int(destination["id"]), host_endpoint, where)
                elif destination.get("type") == "PCIE_SW":
                    add_root((destination["id"], destination["port"]), host_endpoint)

    apply_pcie_host_flow_parents()

    # ==========================================
    # 2. MAP INCOMING EDGES PER PHASE
    # ==========================================
    incoming_edges_per_phase = {}
    for phase_idx_int, phase_type in enumerate(timeline):
        phase_idx = str(phase_idx_int)
        if (
            phase_type == "CONCAT"
            or phase_type in reduce_phase_types
            or phase_type in res_phase_types
        ):
            edges = {}
            for core_id_str, config in cores_data.items():
                target = (
                    config.get("timeline_data", {}).get(phase_idx, {}).get("target")
                )
                if target is not None:
                    edges.setdefault(target, []).append(int(core_id_str))

            for target in edges:
                edges[target].sort()  # Ensure deterministic order

            incoming_edges_per_phase[phase_idx] = edges

    def to_int_or_none(value):
        try:
            if isinstance(value, bool):
                return None
            return int(value)
        except (TypeError, ValueError):
            return None

    implicit_collector_cores: set[int] = set()
    implicit_collector_concat_phases: set[int] = set()

    # Final collectors do not need an extra init/credit packet: accepting the
    # branch packets is the readiness signal. Multi-lane real Phase1 maps copy
    # the base lane by core offset, so collect both the base collector and every
    # materialized lane topk/collector core.
    for section_name in ("phase1", "hardware_smoke"):
        section = raw_data.get(section_name, {}) or {}
        lane = section.get("lane", {})
        if lane.get("router_final_collector_ready") != "implicit_from_branch_receive":
            continue

        base_collector = to_int_or_none(lane.get("router_final_collector"))
        if base_collector is not None:
            implicit_collector_cores.add(base_collector)

        concat_phase = to_int_or_none(lane.get("router_concat_phase"))
        if concat_phase is not None:
            implicit_collector_concat_phases.add(concat_phase)

        for lane_layout in section.get("lanes", []) or []:
            if not isinstance(lane_layout, dict):
                continue
            topk_core = to_int_or_none(lane_layout.get("topk_core"))
            if topk_core is not None:
                implicit_collector_cores.add(topk_core)
                continue
            core_id_offset = to_int_or_none(lane_layout.get("core_id_offset"))
            if base_collector is not None and core_id_offset is not None:
                implicit_collector_cores.add(base_collector + core_id_offset)

    if implicit_collector_cores:
        for sink in raw_data.get("phase1_sinks", []) or []:
            if not isinstance(sink, dict):
                continue
            target = to_int_or_none(sink.get("target"))
            if target is not None:
                implicit_collector_cores.add(target)

        for cid_text, core_config in cores_data.items():
            cid = to_int_or_none(cid_text)
            if cid is None:
                continue
            for phase_data in (core_config.get("timeline_data", {}) or {}).values():
                if not isinstance(phase_data, dict):
                    continue
                for seq in phase_data.get("compute_seqs", []) or []:
                    if not isinstance(seq, dict):
                        continue
                    metadata = seq.get("metadata", {}) if isinstance(seq.get("metadata"), dict) else {}
                    if metadata.get("semantic_op") != "router_softmax_topk":
                        continue
                    collector_core = to_int_or_none(metadata.get("collector_core"))
                    physical_topk = to_int_or_none(metadata.get("physical_topk_core_id"))
                    if collector_core == cid or physical_topk == cid:
                        implicit_collector_cores.add(cid)

    # RETIRE tuple construction still needs a parent destination. Derive it
    # from the actual incoming branch edges. The DTE uses no_credit_return for
    # these collectors, so this parent is not used to send a credit-return packet.
    for final_collector in sorted(implicit_collector_cores):
        for concat_phase in sorted(implicit_collector_concat_phases):
            branch_sources = incoming_edges_per_phase.get(str(concat_phase), {}).get(
                final_collector, []
            )
            if branch_sources:
                parent_map.setdefault(final_collector, branch_sources[0])
                break

    # ==========================================
    # 3. HELPER TO SIMULATE TARGET TASK INDEX
    # ==========================================
    def compute_task_length(c_id: int, target_phase_idx: int) -> int:
        """Calculates how many tasks exist on a target core BEFORE a given phase."""
        length = 0 if c_id in implicit_collector_cores else 1
        c_config = cores_data.get(str(c_id), {})

        for p_idx in range(target_phase_idx):
            p_str = str(p_idx)
            p_type = timeline[p_idx]
            p_data = c_config.get("timeline_data", {}).get(p_str, {})

            if p_type in ["INIT", "FIFO_IN"]:
                targets = p_data.get("targets", [])
                if targets:
                    length += 1 + len(targets)  # Credit + one init per target
            elif p_type == "COMPUTE":
                length += len(p_data.get("compute_seqs", []))
            elif p_type == "MOVE" or p_type in bypass_phase_types:
                if p_data.get("target", p_data.get("sink", p_data.get("dst"))) is not None:
                    length += 1
            elif p_type == "CONCAT" or p_type in reduce_phase_types:
                # Skips for incoming edges
                incoming = incoming_edges_per_phase.get(p_str, {}).get(c_id, [])
                length += len(incoming)
                if bool(
                    p_data.get("local")
                    or p_data.get("local_compute")
                    or p_data.get("res_sum_local")
                ):
                    length += 1
                # Outgoing DTE reduce task
                if p_data.get("target") is not None:
                    length += 1
            elif p_type in res_phase_types:
                entry = res_entry_targets.get(c_id)
                if entry is None or int(entry["phase_idx"]) != p_idx:
                    incoming = incoming_edges_per_phase.get(p_str, {}).get(c_id, [])
                    length += len(incoming)
                if p_data.get("target") is not None:
                    length += 2

        return length

    # ==========================================
    # 4. BUILD THE TABLE LOOPING THE TIMELINE
    # ==========================================
    for core_id in range(GLOBAL_CONFIG.get_num_cores()):
        tasks = []
        credit_entries = {}

        # --- Base Instructions Helpers ---
        def make_init(dst=None, is_skip=False, time_val=0, opcode=None):
            if dst is None:
                if not is_skip:
                    raise RuntimeError(f"Core {core_id}: INIT/FIFO_IN task is missing destination.")
                # SKIP rows are local bookkeeping placeholders. Real DTE work
                # must name a destination in the map instead of using topology
                # neighbor guesses.
                dst = core_id
            coord = GLOBAL_CONFIG.get_coords_by_id(dst)
            if opcode is None:
                opcode = Opcode.USER_INIT
            return (
                UnitType.SKIP if is_skip else UnitType.DTE,
                opcode,
                0,
                0,
                dst,
                coord,
                time_val,
            )

        def make_credit():
            return (UnitType.CU, Opcode._DONTCARE, 0, 0, 0, (0, 0), 0)

        def make_reduce(
            dst=None, is_skip=False, task_id=-1, time_val=0, opcode=Opcode.REDUCE
        ):
            if dst is None:
                if not is_skip:
                    raise RuntimeError(f"Core {core_id}: {opcode.name} task is missing destination.")
                # Inactive incoming reductions still need a local SKIP task,
                # but active traffic must carry an explicit reduce target.
                dst = core_id
            coord = GLOBAL_CONFIG.get_coords_by_id(dst)
            if is_skip:
                return (UnitType.SKIP, opcode, 0, 0, dst, coord, time_val)
            else:
                return (UnitType.DTE, opcode, task_id, 0, dst, coord, time_val)

        def make_res(dst=None, is_skip=False, task_id=0, time_val=0):
            if dst is None:
                if not is_skip:
                    raise RuntimeError(f"Core {core_id}: RES task is missing destination.")
                dst = core_id
            coord = GLOBAL_CONFIG.get_coords_by_id(dst)
            if is_skip:
                return (UnitType.SKIP, Opcode.RES, 0, 0, dst, coord, time_val)
            return (UnitType.DTE, Opcode.RES, task_id, 0, dst, coord, time_val)

        def make_concat(dst=None, is_skip=False, task_id=-1, time_val=0):
            if dst is None:
                if not is_skip:
                    raise RuntimeError(f"Core {core_id}: CONCAT task is missing destination.")
                # Same rule as REDUCE: SKIP can be local, active CONCAT must
                # be routed by an explicit map destination.
                dst = core_id
            coord = GLOBAL_CONFIG.get_coords_by_id(dst)
            if is_skip:
                return (UnitType.SKIP, Opcode.CONCAT, 0, 0, dst, coord, time_val)
            else:
                return (UnitType.DTE, Opcode.CONCAT, task_id, 0, dst, coord, time_val)

        def make_move(
            coords: Tuple[int, int],
            *,
            dst_id=0,
            tag=0,
            time_val=0,
            opcode=Opcode.MOVE,
        ):
            return (UnitType.DTE, opcode, tag, 0, dst_id, coords, time_val)

        def remember_task_metadata(task_id: int, metadata: dict):
            if metadata:
                task_send_metadata.setdefault(core_id, {})[task_id] = metadata

        def make_retire(dst=None):
            if dst is None:
                if core_id in parent_map:
                    dst = parent_map[core_id]
                else:
                    # RETIRE acknowledges the parent that activated this core
                    # through Host/INIT/REDUCTION metadata. If no parent exists,
                    # the map is missing routing intent and should fail here.
                    raise RuntimeError(f"Core {core_id}: RETIRE task has no explicit parent destination.")

            if core_id in host_map:
                coord = host_map[core_id]
            else:
                coord = GLOBAL_CONFIG.get_coords_by_id(dst)
            return (UnitType.DTE, Opcode.RETIRE, 0, dst, 0, coord, 0)

        def make_MU(time_val=0):
            return (UnitType.MC, Opcode._DONTCARE, 0, 0, 0, (0, 0), time_val)

        def make_VU(time_val=0):
            return (UnitType.VC, Opcode._DONTCARE, 0, 0, 0, (0, 0), time_val)

        # --- Dynamic Generation ---
        config = cores_data.get(str(core_id))
        # A core is "active" (needs a generated task table) not only when it has an
        # explicit phase entry, but also when it merely receives/sends work: an
        # incoming reduce/concat/res edge, an out target, an initial volume, or a
        # RES entry. Without this, a full-result / multi-lane map can route a comm
        # to a core whose task table was never populated -> TaskTable.lookup()
        # IndexError at runtime. (Ported from codex fusion; fixes 8x4 4-lane e2e.)
        has_incoming_edges = any(
            core_id in phase_edges
            for phase_edges in incoming_edges_per_phase.values()
        )
        is_active_core = (
            has_explicit_phase_entry(config)
            or has_incoming_edges
            or core_id in out_map
            or core_id in core_initial_volumes
            or core_id in res_entry_targets
        )

        if is_active_core:
            # A core becomes a FIFO/Broadcast ingress only when its FIFO_IN
            # phase actually fans out. Empty FIFO_IN entries are receiver-side
            # bookkeeping and must still accept USER_INIT.
            my_init_opcode = (
                Opcode.FIFO_IN if has_fifo_fanout_entry(config) else Opcode.USER_INIT
            )

            res_entry = res_entry_targets.get(core_id)
            if res_entry is not None:
                my_init_opcode = Opcode.RES
                my_init_volume = int(res_entry["volume"])
            elif core_id in core_initial_volumes:
                my_init_volume = core_initial_volumes[core_id]
            else:
                if base_init_volume is None:
                    raise ValueError("Map must define an INIT/FIFO_IN volume before building base SKIP tasks.")
                my_init_volume = base_init_volume
            if core_id not in implicit_collector_cores:
                base_task_id = len(tasks)
                tasks += [
                    make_init(is_skip=True, time_val=my_init_volume, opcode=my_init_opcode)
                ]
                if res_entry is not None:
                    remember_task_metadata(base_task_id, dict(res_entry["metadata"]))

            # Iterate over the timeline array continuously
            for phase_idx_int, phase_type in enumerate(timeline):
                phase_idx = str(phase_idx_int)
                phase_data = config.get("timeline_data", {}).get(phase_idx, {})

                # Resolve the phase volume once per timeline entry so every
                # generated task in that phase observes the same byte count.
                current_volume = require_phase_volume(phase_idx_int, phase_type)

                if phase_type in ["INIT", "FIFO_IN"]:
                    init_targets = phase_data.get("targets", [])
                    if init_targets:
                        credit_entries[len(tasks)] = tuple(
                            int(target) for target in init_targets
                        )
                        tasks += [make_credit()]
                        for target in init_targets:
                            if phase_type == "FIFO_IN":
                                # A FIFO fanout task is local to the sender.
                                # FIFO_OUT pops MatrixMem FIFO and DTE emits
                                # USER_INIT downstream. Chained broadcast cores
                                # still receive FIFO_IN when their own FIFO_IN
                                # phase has real fanout.
                                target_config = cores_data.get(str(target), {})
                                current_opcode = (
                                    Opcode.FIFO_IN
                                    if has_fifo_fanout_entry(target_config)
                                    else Opcode.FIFO_OUT
                                )
                            else:
                                current_opcode = Opcode.USER_INIT

                            metadata = normalize_dte_phase_metadata(
                                phase_type, phase_data
                            )
                            if metadata:
                                metadata.setdefault("source_core", core_id)
                                metadata.setdefault("target_core", int(target))
                                metadata.setdefault("phase_type", phase_type)
                                metadata.setdefault("phase_idx", phase_idx_int)
                                metadata.setdefault("impl_opcode", current_opcode.name)

                            task_id = len(tasks)
                            tasks += [
                                make_init(
                                    dst=target,
                                    time_val=current_volume,
                                    opcode=current_opcode,
                                )
                            ]
                            remember_task_metadata(task_id, metadata)

                elif phase_type == "COMPUTE":
                    compute_seqs = phase_data.get("compute_seqs", [])
                    for compute_op in compute_seqs:
                        op_type = compute_op.get("op")
                        if op_type not in ("M", "V"):
                            raise ValueError(f"Core {core_id} phase {phase_idx}: unsupported compute op {op_type!r}.")
                        if "time" in compute_op and compute_op["time"] is not None:
                            op_time = int(compute_op["time"])
                        else:
                            # Explicit fallback path: map default first, then
                            # top-level Matrix/Vector CLI value. Missing both
                            # means the compute task is underspecified.
                            op_time = compute_time_default(op_type)
                        if op_type == "M":
                            task_id = len(tasks)
                            tasks += [make_MU(op_time)]
                        elif op_type == "V":
                            task_id = len(tasks)
                            tasks += [make_VU(op_time)]
                        metadata = normalize_send_metadata(compute_op.get("metadata", {}))
                        if compute_op.get("name") and "task_name" not in metadata:
                            metadata["task_name"] = compute_op.get("name")
                        if compute_op.get("stage") and "stage" not in metadata:
                            metadata["stage"] = compute_op.get("stage")
                        remember_task_metadata(task_id, metadata)

                elif phase_type == "MOVE" or phase_type in bypass_phase_types:
                    move_target = phase_data.get(
                        "target",
                        phase_data.get(
                            "sink",
                            phase_data.get(
                                "dst",
                                phase_data.get("endpoint"),
                            ),
                        ),
                    )
                    if move_target is not None:
                        dst_coords, dst_id = resolve_external_endpoint(move_target)
                        if phase_type in bypass_phase_types:
                            metadata = normalize_dte_phase_metadata(
                                phase_type, phase_data
                            )
                        else:
                            metadata = normalize_send_metadata(
                                phase_data.get(
                                    "send_metadata",
                                    phase_data.get(
                                        "packet",
                                        phase_data.get("metadata", {}),
                                    ),
                                )
                            )
                            if phase_data.get("role") and "role" not in metadata:
                                metadata["role"] = phase_data.get("role")
                            if phase_data.get("packet_type") and "packet_type" not in metadata:
                                metadata["packet_type"] = phase_data.get("packet_type")
                        task_id = len(tasks)
                        tasks += [
                            make_move(
                                dst_coords,
                                dst_id=dst_id,
                                tag=int(phase_data.get("tag", 0)),
                                time_val=current_volume,
                                opcode=Opcode.BYPASS
                                if phase_type in bypass_phase_types
                                else Opcode.MOVE,
                            )
                        ]
                        remember_task_metadata(task_id, metadata)

                elif phase_type == "CONCAT":
                    # 1. GENERATE SKIPS FOR INCOMING DATA IN THIS PHASE
                    incoming_senders = incoming_edges_per_phase.get(phase_idx, {}).get(
                        core_id, []
                    )

                    concat_target = phase_data.get("target")

                    for sender in incoming_senders:
                        sender_phase_data = (
                            cores_data.get(str(sender), {})
                            .get("timeline_data", {})
                            .get(phase_idx, {})
                        )
                        metadata = normalize_dte_phase_metadata(
                            phase_type, sender_phase_data
                        )
                        if metadata:
                            metadata.setdefault("source_core", sender)
                            metadata.setdefault("target_core", core_id)
                            metadata.setdefault("phase_type", phase_type)
                            metadata.setdefault("phase_idx", phase_idx_int)
                            metadata.setdefault("impl_opcode", Opcode.CONCAT.name)
                        if concat_target is not None:
                            task_id = len(tasks)
                            tasks += [
                                make_concat(
                                    dst=concat_target,
                                    is_skip=True,
                                    time_val=current_volume,
                                )
                            ]
                        else:
                            task_id = len(tasks)
                            tasks += [
                                make_concat(is_skip=True, time_val=current_volume)
                            ]
                        remember_task_metadata(task_id, metadata)

                    # 2. GENERATE THE DTE CONCAT (Calculate ID on the receiver core)
                    if concat_target is not None:
                        # Find exactly where this task lands on the target core
                        base_length = compute_task_length(concat_target, phase_idx_int)
                        target_senders = incoming_edges_per_phase[phase_idx][
                            concat_target
                        ]
                        offset = target_senders.index(core_id)
                        assigned_task_id = base_length + offset

                        metadata = normalize_dte_phase_metadata(
                            phase_type, phase_data
                        )
                        if metadata:
                            metadata.setdefault("source_core", core_id)
                            metadata.setdefault("target_core", concat_target)
                            metadata.setdefault("phase_type", phase_type)
                            metadata.setdefault("phase_idx", phase_idx_int)
                            metadata.setdefault("impl_opcode", Opcode.CONCAT.name)
                            metadata.setdefault("target_task_id", assigned_task_id)

                        task_id = len(tasks)
                        tasks += [
                            make_concat(
                                dst=concat_target,
                                task_id=assigned_task_id,
                                time_val=current_volume,
                            )
                        ]
                        remember_task_metadata(task_id, metadata)

                elif phase_type in res_phase_types:
                    incoming_senders = incoming_edges_per_phase.get(phase_idx, {}).get(
                        core_id, []
                    )
                    res_target = phase_data.get("target")
                    res_entry = res_entry_targets.get(core_id)
                    base_entry_handles_phase = (
                        res_entry is not None
                        and int(res_entry["phase_idx"]) == phase_idx_int
                    )

                    if not base_entry_handles_phase:
                        for sender in incoming_senders:
                            sender_phase_data = (
                                cores_data.get(str(sender), {})
                                .get("timeline_data", {})
                                .get(phase_idx, {})
                            )
                            metadata = normalize_dte_phase_metadata(
                                phase_type, sender_phase_data
                            )
                            if metadata:
                                metadata.setdefault("source_core", sender)
                                metadata.setdefault("target_core", core_id)
                                metadata.setdefault("phase_type", phase_type)
                                metadata.setdefault("phase_idx", phase_idx_int)
                                metadata.setdefault("impl_opcode", Opcode.RES.name)
                            task_id = len(tasks)
                            tasks += [
                                make_res(is_skip=True, time_val=current_volume)
                            ]
                            remember_task_metadata(task_id, metadata)

                    if res_target is not None:
                        res_target = int(res_target)
                        if (
                            res_target in res_entry_targets
                            and int(res_entry_targets[res_target]["phase_idx"])
                            == phase_idx_int
                        ):
                            assigned_task_id = 0
                        else:
                            base_length = compute_task_length(
                                res_target, phase_idx_int
                            )
                            target_senders = incoming_edges_per_phase[phase_idx][
                                res_target
                            ]
                            offset = target_senders.index(core_id)
                            assigned_task_id = base_length + offset

                        metadata = normalize_dte_phase_metadata(
                            phase_type, phase_data
                        )
                        if metadata:
                            metadata.setdefault("task_kind", "RES_SEND")
                            metadata.setdefault("source_core", core_id)
                            metadata.setdefault("target_core", res_target)
                            metadata.setdefault("phase_type", phase_type)
                            metadata.setdefault("phase_idx", phase_idx_int)
                            metadata.setdefault("impl_opcode", Opcode.RES.name)
                            metadata.setdefault("target_task_id", assigned_task_id)
                            metadata.setdefault("hit_map", ())

                        credit_entries[len(tasks)] = (res_target,)
                        tasks += [make_credit()]
                        task_id = len(tasks)
                        tasks += [
                            make_res(
                                dst=res_target,
                                task_id=assigned_task_id,
                                time_val=current_volume,
                            )
                        ]
                        remember_task_metadata(task_id, metadata)

                elif phase_type in reduce_phase_types:
                    # 1. GENERATE SKIPS FOR INCOMING DATA IN THIS PHASE
                    incoming_senders = incoming_edges_per_phase.get(phase_idx, {}).get(
                        core_id, []
                    )
                    num_incoming = len(incoming_senders)

                    reduce_target = phase_data.get("target")
                    local_reduce = bool(
                        phase_data.get("local")
                        or phase_data.get("local_compute")
                        or phase_data.get("res_sum_local")
                    )
                    reduce_opcode = reduce_opcode_for_phase(phase_type)
                    local_reduce_opcode = (
                        Opcode.RES if phase_type == "RES_SUM" else reduce_opcode
                    )

                    for sender in incoming_senders:
                        sender_phase_data = (
                            cores_data.get(str(sender), {})
                            .get("timeline_data", {})
                            .get(phase_idx, {})
                        )
                        metadata = normalize_dte_phase_metadata(
                            phase_type, sender_phase_data
                        )
                        if metadata:
                            metadata.setdefault("source_core", sender)
                            metadata.setdefault("target_core", core_id)
                            metadata.setdefault("phase_type", phase_type)
                            metadata.setdefault("phase_idx", phase_idx_int)
                            metadata.setdefault("impl_opcode", reduce_opcode.name)
                        if reduce_target is not None:
                            task_id = len(tasks)
                            tasks += [
                                make_reduce(
                                    dst=reduce_target,
                                    is_skip=True,
                                    time_val=current_volume,
                                    opcode=reduce_opcode,
                                )
                            ]
                        else:
                            task_id = len(tasks)
                            tasks += [
                                make_reduce(
                                    is_skip=True,
                                    time_val=current_volume,
                                    opcode=reduce_opcode,
                                )
                            ]
                        remember_task_metadata(task_id, metadata)

                    if local_reduce:
                        metadata = normalize_dte_phase_metadata(
                            phase_type, phase_data
                        )
                        if metadata:
                            metadata.setdefault("source_core", core_id)
                            metadata.setdefault("target_core", core_id)
                            metadata.setdefault("phase_type", phase_type)
                            metadata.setdefault("phase_idx", phase_idx_int)
                            metadata.setdefault("impl_opcode", local_reduce_opcode.name)
                            metadata.setdefault("local_compute", True)

                        task_id = len(tasks)
                        tasks += [
                            make_reduce(
                                dst=core_id,
                                task_id=task_id,
                                time_val=current_volume,
                                opcode=local_reduce_opcode,
                            )
                        ]
                        remember_task_metadata(task_id, metadata)

                    # 2. GENERATE THE DTE REDUCE (Calculate ID on the receiver core)
                    if reduce_target is not None:
                        # Find exactly where this task lands on the target core
                        base_length = compute_task_length(reduce_target, phase_idx_int)
                        target_senders = incoming_edges_per_phase[phase_idx][
                            reduce_target
                        ]
                        offset = target_senders.index(core_id)
                        assigned_task_id = base_length + offset

                        metadata = normalize_dte_phase_metadata(
                            phase_type, phase_data
                        )
                        if metadata:
                            metadata.setdefault("source_core", core_id)
                            metadata.setdefault("target_core", reduce_target)
                            metadata.setdefault("phase_type", phase_type)
                            metadata.setdefault("phase_idx", phase_idx_int)
                            metadata.setdefault("impl_opcode", reduce_opcode.name)

                        task_id = len(tasks)
                        tasks += [
                            make_reduce(
                                dst=reduce_target,
                                task_id=assigned_task_id,
                                time_val=current_volume,
                                opcode=reduce_opcode,
                            )
                        ]
                        remember_task_metadata(task_id, metadata)

            # Move and Retire appended at the END of the timeline
            if core_id in out_map:
                out_coords, out_id, out_volume = out_map[core_id]
                tasks += [
                    make_move(
                        out_coords,
                        dst_id=out_id,
                        time_val=out_volume,
                    )
                ]

            retire_task_id = len(tasks)
            tasks += [make_retire()]
            if core_id in implicit_collector_cores:
                remember_task_metadata(
                    retire_task_id,
                    {
                        "task_name": "implicit_collector_retire",
                        "semantic_op": "retire",
                        "no_credit_return": True,
                        "collector_ready": "implicit_from_branch_receive",
                    },
                )

        table[core_id] = tasks
        credit_table[core_id] = credit_entries

    _task_send_metadata_table = task_send_metadata
    _task_send_metadata_cached_parser = parser

    return table, credit_table


def build_lookup_table():
    """Build the legacy TaskTable view without exposing CreditTable details."""
    table, _ = build_software_tables()
    return table


class CoreTaskTable:
    """
    Per-core view of the software task table.

    The task sequence of one core is immutable during simulation, so hot-path
    modules can keep this local view instead of indexing the global table by
    core_id for every task lookup.
    """

    __slots__ = ("core_id", "_tasks")

    def __init__(self, core_id: int, tasks: Tuple[TaskTableEntry, ...]):
        self.core_id = core_id
        self._tasks = tasks

    def lookup(self, task_id: int) -> TaskTableEntry:
        ret = self._tasks[task_id]

        if _should_snapshot_software_queue():
            GLOBAL_MONITOR.take_snapshot(
                event_note=f" Software {self.core_id}: Task {task_id}. Return {ret}.",
                level=LogLevel.DEBUG,
                core_id=SNAPSHOT_CORE_SOFTWARE_QUEUE,
            )

        return ret

    def __len__(self) -> int:
        return len(self._tasks)


class CoreCreditTable:
    """Per-core mapping from CU task IDs to their downstream core IDs."""

    __slots__ = ("core_id", "_entries")

    def __init__(self, core_id: int, entries: Dict[int, CreditTargets]):
        self.core_id = core_id
        self._entries = entries

    def lookup(self, task_id: int) -> CreditTargets:
        try:
            return self._entries[task_id]
        except KeyError as exc:
            raise RuntimeError(
                f"CreditTable Core {self.core_id}: no targets for CU Task {task_id}."
            ) from exc


class SoftwareQueue:
    """
    Software Queue Mapping Table.
    The LUT is dynamically generated via build_lookup_table() and cached.

    Tuple format: (UnitType, Opcode, Tag, upstream_cid, downstream_cid, dst_coords, compute_time)
    """

    _cached_parser = None
    _cached_table: Dict[int, Tuple[TaskTableEntry, ...]] | None = None
    _cached_credit_table: Dict[int, Dict[int, CreditTargets]] | None = None
    _core_views: Dict[int, CoreTaskTable] = {}
    _credit_core_views: Dict[int, CoreCreditTable] = {}

    @classmethod
    def _get_table(cls) -> Dict[int, Tuple[TaskTableEntry, ...]]:
        current_parser = GLOBAL_CONFIG.MAP_PARSER
        if cls._cached_table is None or cls._cached_parser is not current_parser:
            task_table, credit_table = build_software_tables()
            cls._cached_table = {
                core_id: tuple(tasks)
                for core_id, tasks in task_table.items()
            }
            cls._cached_credit_table = credit_table
            cls._cached_parser = current_parser
            cls._core_views = {}
            cls._credit_core_views = {}

        return cls._cached_table

    @classmethod
    def _get_credit_table(cls) -> Dict[int, Dict[int, CreditTargets]]:
        cls._get_table()
        if cls._cached_credit_table is None:
            raise RuntimeError("CreditTable was not generated with TaskTable.")
        return cls._cached_credit_table

    @classmethod
    def bind_core(cls, core_id: int) -> CoreTaskTable:
        table = cls._get_table()
        view = cls._core_views.get(core_id)
        if view is None:
            view = CoreTaskTable(core_id, table[core_id])
            cls._core_views[core_id] = view

        return view

    @classmethod
    def bind_credit_core(cls, core_id: int) -> CoreCreditTable:
        credit_table = cls._get_credit_table()
        view = cls._credit_core_views.get(core_id)
        if view is None:
            view = CoreCreditTable(core_id, credit_table[core_id])
            cls._credit_core_views[core_id] = view
        return view

    @staticmethod
    def lookup(
        core_id: int, task_id: int
    ) -> TaskTableEntry:
        return SoftwareQueue.bind_core(core_id).lookup(task_id)


TASK_TABLE = SoftwareQueue()


def get_task_metadata(core_id: int, task_id: int) -> dict:
    global _task_send_metadata_cached_parser, _task_send_metadata_table

    parser = GLOBAL_CONFIG.MAP_PARSER
    if _task_send_metadata_cached_parser is not parser:
        SoftwareQueue._get_table()

    return dict(_task_send_metadata_table.get(core_id, {}).get(task_id, {}))


def get_task_send_metadata(core_id: int, task_id: int) -> dict:
    return get_task_metadata(core_id, task_id)


class CreditQueue:
    """Dedicated lookup table for CU task downstream targets."""

    @staticmethod
    def bind_core(core_id: int) -> CoreCreditTable:
        return SoftwareQueue.bind_credit_core(core_id)


CREDIT_TABLE = CreditQueue()


_skip_source_cached_parser = None
_skip_source_cache: Dict[int, Dict[int, dict]] | None = None


def _build_skip_source_table() -> Dict[int, Dict[int, dict]]:
    parser = GLOBAL_CONFIG.MAP_PARSER
    if parser is None:
        raise RuntimeError("TopologyParser must be initialized first.")

    cores_data = parser.get_all_cores_config()
    timeline = parser.get_timeline()
    incoming_edges_per_phase = {}
    reduce_phase_types = {"REDUCE", "REDUCTION", "RES_SUM"}
    res_phase_types = {"RES"}
    res_entry_targets = set()

    for phase_idx_int, phase_type in enumerate(timeline):
        if phase_type not in res_phase_types:
            continue
        phase_idx = str(phase_idx_int)
        edges = {}
        for core_id_str, config in cores_data.items():
            target = config.get("timeline_data", {}).get(phase_idx, {}).get("target")
            if target is not None:
                edges.setdefault(int(target), []).append(int(core_id_str))
        for target, senders in edges.items():
            target_phase_data = (
                cores_data.get(str(target), {})
                .get("timeline_data", {})
                .get(phase_idx, {})
            )
            raw_metadata = target_phase_data.get(
                "send_metadata",
                target_phase_data.get(
                    "packet",
                    target_phase_data.get("metadata", {}),
                ),
            )
            if isinstance(raw_metadata, dict) and raw_metadata.get("recv_init"):
                res_entry_targets.add((target, phase_idx_int))

    for phase_idx_int, phase_type in enumerate(timeline):
        phase_idx = str(phase_idx_int)
        if (
            phase_type == "CONCAT"
            or phase_type in reduce_phase_types
            or phase_type in res_phase_types
        ):
            edges = {}
            for core_id_str, config in cores_data.items():
                target = (
                    config.get("timeline_data", {}).get(phase_idx, {}).get("target")
                )
                if target is not None:
                    edges.setdefault(int(target), []).append(int(core_id_str))

            for target in edges:
                edges[target].sort()
            incoming_edges_per_phase[phase_idx] = edges

    branch_group_cache = {}

    def branch_group_ids(phase_idx: str, sender: int, visiting=None):
        key = (phase_idx, sender)
        if key in branch_group_cache:
            return branch_group_cache[key]

        visiting = set() if visiting is None else set(visiting)
        if key in visiting:
            return ()
        visiting.add(key)

        groups = []
        sender_group_id = parser.get_core_group_id(sender)
        if sender_group_id is not None:
            groups.append(sender_group_id)

        for child in incoming_edges_per_phase.get(phase_idx, {}).get(sender, []):
            groups.extend(branch_group_ids(phase_idx, child, visiting))

        deduped = []
        seen = set()
        for group_id in groups:
            group_key = str(group_id).strip().upper()
            if group_key in seen:
                continue
            seen.add(group_key)
            deduped.append(group_id)

        branch_group_cache[key] = tuple(deduped)
        return branch_group_cache[key]

    skip_source_table: Dict[int, Dict[int, dict]] = {}

    for core_id in range(GLOBAL_CONFIG.get_num_cores()):
        task_id = 1
        config = cores_data.get(str(core_id), {})
        if not config:
            skip_source_table[core_id] = {}
            continue

        for phase_idx_int, phase_type in enumerate(timeline):
            phase_idx = str(phase_idx_int)
            phase_data = config.get("timeline_data", {}).get(phase_idx, {})

            if phase_type in ["INIT", "FIFO_IN"]:
                init_targets = phase_data.get("targets", [])
                if init_targets:
                    task_id += 1 + len(init_targets)
            elif phase_type == "COMPUTE":
                task_id += len(phase_data.get("compute_seqs", []))
            elif phase_type == "MOVE" or phase_type == "BYPASS":
                if phase_data.get("target", phase_data.get("sink", phase_data.get("dst"))) is not None:
                    task_id += 1
            elif phase_type == "CONCAT" or phase_type in reduce_phase_types:
                incoming_senders = incoming_edges_per_phase.get(phase_idx, {}).get(
                    core_id, []
                )
                for sender in incoming_senders:
                    skip_source_table.setdefault(core_id, {})[task_id] = {
                        "sender": sender,
                        "phase_type": phase_type,
                        "phase_idx": phase_idx_int,
                        "sender_group_ids": branch_group_ids(phase_idx, sender),
                    }
                    task_id += 1

                if bool(
                    phase_data.get("local")
                    or phase_data.get("local_compute")
                    or phase_data.get("res_sum_local")
                ):
                    task_id += 1

                if phase_data.get("target") is not None:
                    task_id += 1
            elif phase_type in res_phase_types:
                if (core_id, phase_idx_int) not in res_entry_targets:
                    incoming_senders = incoming_edges_per_phase.get(phase_idx, {}).get(
                        core_id, []
                    )
                    task_id += len(incoming_senders)

                if phase_data.get("target") is not None:
                    task_id += 2

        skip_source_table.setdefault(core_id, {})

    return skip_source_table


def get_skip_source_metadata(core_id: int, task_id: int) -> dict | None:
    global _skip_source_cached_parser, _skip_source_cache

    parser = GLOBAL_CONFIG.MAP_PARSER
    if _skip_source_cache is None or _skip_source_cached_parser is not parser:
        _skip_source_cache = _build_skip_source_table()
        _skip_source_cached_parser = parser

    return _skip_source_cache.get(core_id, {}).get(task_id)


def dump_software_queue(table=None):
    if table is None:
        table = build_lookup_table()

    def display_task_labels(core_id: int, task_id: int, unit_type, opcode):
        metadata = get_task_metadata(core_id, task_id)
        if opcode is Opcode.RES:
            raw_semantic = (
                metadata.get("path")
                or metadata.get("semantic_op")
                or metadata.get("task_name")
            )
            if raw_semantic is not None:
                semantic = str(raw_semantic).strip().replace(" ", "_").upper()
                if semantic in {"RES_SUM", "PHASE1_RES_SUM"}:
                    return "RES_SUM", opcode.name
            task_kind = str(metadata.get("task_kind", "")).strip().upper()
            if bool(metadata.get("recv_init", False)) or task_kind == "RES_RECV_INIT":
                return "RES_RECV_INIT", opcode.name
            if task_kind == "RES_SEND":
                return "RES_SEND", opcode.name
            return unit_type.name, opcode.name
        raw_semantic = (
            metadata.get("path")
            or metadata.get("semantic_op")
            or metadata.get("task_name")
        )
        if raw_semantic is not None:
            semantic = str(raw_semantic).strip().replace(" ", "_").upper()
            if semantic in {"RES_SUM", "PHASE1_RES_SUM"}:
                return "RES_SUM", opcode.name
        return unit_type.name, opcode.name

    print("\n" + "=" * 50)
    print(" 📜 软件指令队列解码查验单 (Debug)")
    print("=" * 50)

    print("\n🌍 [外部节点 (External Nodes) 动态路由映射]")
    if getattr(GLOBAL_CONFIG, "EXTERNAL_NODES", None):
        for ext_coords, info in GLOBAL_CONFIG.EXTERNAL_NODES.items():
            print(
                f"  🔌 设备类型: {info['device_type']:<5} | "
                f"外部坐标: {str(ext_coords):<8} => "
                f"目标网关: Chip {info['chip_coords']} - Router {info['router_coords']} | "
                f"接入端口: {info['port']}"
            )
    else:
        print("  👻 哎呀，当前没有配置或读取到任何外部节点哦！")

    print("-" * 50)

    for core_id in sorted(table.keys()):
        print(f"\n=== Core {core_id} ===")
        for i, task in enumerate(table[core_id]):
            unit_type, opcode, tag, up, down, coord, compute_time = task
            unit_label, opcode_label = display_task_labels(
                core_id, i, unit_type, opcode
            )
            print(
                f"[{i:02d}] "
                f"{unit_label:<10} | "
                f"{opcode_label:<10} | "
                f"tag={tag:<5} | "
                f"up={up:<3} | "
                f"down={down:<3} | "
                f"coord={str(coord):<8} | "
                f"time={compute_time}"
            )
    print("\n" + "=" * 50 + "\n")


def dump_credit_queue(table=None):
    if table is None:
        _, table = build_software_tables()

    print("\n" + "=" * 50)
    print(" Credit Table 解码查验单 (Debug)")
    print("=" * 50)

    populated = False
    for core_id in sorted(table):
        entries = table[core_id]
        if not entries:
            continue
        populated = True
        print(f"\n=== Core {core_id} ===")
        for task_id, targets in sorted(entries.items()):
            print(f"[{task_id:02d}] targets={list(targets)}")

    if not populated:
        print("\n(no CU tasks)")
    print("\n" + "=" * 50 + "\n")


# def get_host_info(host_id: str) -> dict:
#     """
#     Parses the map data to find information about a specific host,
#     dynamically resolving timeline task types based on metadata.
#     """

#     map_data = GLOBAL_CONFIG.MAP_PARSER.get_raw_data()

#     # 1. Find the target host in the "hosts" list
#     target_host = None
#     for host in map_data.get("hosts", []):
#         if host.get("id") == host_id:
#             target_host = host
#             break

#     if not target_host:
#         return f"Host with id '{host_id}' not found."

#     # 2. Determine Current EP Group type
#     is_moe = target_host.get("is_moe", False)
#     ep_group_type = "MOE" if is_moe else "DENSE"

#     # 3. Retrieve Volume and Target Core ID
#     volume = target_host.get("volume")
#     target_core_id = target_host.get("target")

#     # 4. Determine Init_Task Entry dynamically from meta timeline arrays
#     init_task_entry = "UNKNOWN"
#     meta = map_data.get("meta", {})
#     timeline = meta.get("timeline", [])

#     # Find the positions of the target phases inside the timeline list
#     fifo_in_key = str(timeline.index("FIFO_IN")) if "FIFO_IN" in timeline else None
#     init_key = str(timeline.index("INIT")) if "INIT" in timeline else None

#     # Inspect the connected core's actual scheduled timeline data
#     cores_dict = map_data.get("cores", {})
#     target_core = cores_dict.get(str(target_core_id), {})
#     timeline_data = target_core.get("timeline_data", {})

#     # Match using the dynamic string keys found above
#     if fifo_in_key and fifo_in_key in timeline_data:
#         init_task_entry = "FIFO_IN"
#     elif init_key and init_key in timeline_data:
#         init_task_entry = "USER_INIT"

#     # 5. Calculate global coordinate of the target core
#     chip_cols = meta.get("chip_cols", 4)
#     core_cols_per_chip = 4
#     total_mesh_cols = chip_cols * core_cols_per_chip

#     if target_core_id is not None:
#         coord_of_target = [target_core_id // total_mesh_cols, target_core_id % total_mesh_cols]
#     else:
#         coord_of_target = None

#     return {
#         "Current EP Group type": ep_group_type,
#         "Init_Task Entry": init_task_entry,
#         "Volume": volume,
#         "coord of target": coord_of_target
#     }


def get_host_info(host_id: str, runtime_config=None) -> dict:
    """
    Parses the provided runtime configuration state and its active topology parser
    to retrieve information about a specific host.

    Args:
        host_id (str): The ID of the host to search for (e.g., 'dense0').

    Returns:
        dict: Target configuration metrics or an error string if uninitialized/not found.
    """
    config = runtime_config or GLOBAL_CONFIG

    # 1. Safety check to ensure a topology map has actually been loaded
    if not config.MAP_PARSER:
        raise RuntimeError("No topology map has been loaded into runtime config yet.")

    # 2. Find the target host inside the pre-parsed HOSTS_CONFIG list
    target_host = None
    target_id_norm = str(host_id).strip().upper()
    for host in config.HOSTS_CONFIG:
        current_id = str(host.get("id", "")).strip().upper()
        if current_id == target_id_norm:
            target_host = host
            break

    if not target_host:
        raise KeyError(f"Host with id '{host_id}' not found.")

    # 3. Determine Host role from the Host -> EPG binding in new maps.
    host_bindings = getattr(config, "MOE_HOST_BINDINGS", {})
    bound_group_ids = next(
        (
            values
            for key, values in host_bindings.items()
            if str(key).strip().upper() == target_id_norm
        ),
        [],
    )
    is_moe = bool(bound_group_ids) if config.MOE_EP_GROUPS else target_host.get("is_moe", False)
    ep_group_type = EPGroupType.MOE if is_moe else EPGroupType.DENSE

    # 4. Retrieve Volume and Target Core ID
    volume = target_host.get("volume")
    target_core_id = target_host.get("target")

    # 5. Extract raw maps directly from the parser for safe timeline evaluation
    raw_data = config.MAP_PARSER.get_raw_data()
    meta = raw_data.get("meta", {})
    timeline = meta.get("timeline", [])

    # Track down the dynamic index mappings for the keys
    fifo_in_key = str(timeline.index("FIFO_IN")) if "FIFO_IN" in timeline else None
    init_key = str(timeline.index("INIT")) if "INIT" in timeline else None

    # Inspect the connected core's scheduled timeline blocks
    cores_dict = raw_data.get("cores", {})
    target_core = cores_dict.get(str(target_core_id), {})
    timeline_data = target_core.get("timeline_data", {})

    init_task_entry = None
    if fifo_in_key and fifo_in_key in timeline_data:
        init_task_entry = Opcode.FIFO_IN
    elif init_key and init_key in timeline_data:
        init_task_entry = Opcode.USER_INIT

    if init_task_entry is None:
        raise RuntimeError(
            f"Host '{host_id}' target Core {target_core_id} has no INIT/FIFO_IN entry."
        )

    if target_core_id is None:
        raise RuntimeError(f"Host '{host_id}' is missing target Core ID.")
    if volume is None:
        raise RuntimeError(f"Host '{host_id}' is missing volume.")

    # 6. Use the configuration's native layout engine to resolve global grid coordinates.
    coord_of_target = list(config.get_coords_by_id(int(target_core_id)))

    result = {
        "Host ID": target_host.get("id"),
        "Current EP Group type": ep_group_type,
        "Init_Task Entry": init_task_entry,
        "Volume": volume,
        "Target Core ID": target_core_id,
        "coord of target": coord_of_target,
        "is_moe": is_moe,
        "ep_groups": list(bound_group_ids),
    }
    if not config.MOE_EP_GROUPS:
        result["shared_ep_id"] = target_host.get("shared_ep_id", 0)
        result["routed_ep_num"] = target_host.get("routed_ep_num", 0)
    return result


if __name__ == "__main__":
    table = build_lookup_table()

    def pretty_print_lookup_table(table):
        for core_id in sorted(table.keys()):
            print(f"\n=== Core {core_id} ===")
            for i, task in enumerate(table[core_id]):
                unit_type, opcode, tag, up, down, coord, compute_time = task
                print(
                    f"[{i:02d}] "
                    f"{unit_type.name:<10} | "
                    f"{opcode.name:<10} | "
                    f"tag={tag:<5} | "
                    f"up={up:<3} | "
                    f"down={down:<3} | "
                    f"coord={str(coord):<8} | "
                    f"time={compute_time}"
                )

    pretty_print_lookup_table(table)
    SoftwareQueue()
