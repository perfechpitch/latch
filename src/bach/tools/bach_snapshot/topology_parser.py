# Code/topology_parser.py
import json
from collections.abc import Mapping
from typing import List, Any
from hardware_config import CoreType

REDUCE_LIKE_PHASE_TYPES = {"REDUCE", "REDUCTION", "RES_SUM"}
RES_EDGE_PHASE_TYPES = {"RES"}
REDUCTION_CORE_PHASE_TYPES = {"REDUCTION"}
# REDUCE is a data-only input edge: it may satisfy a local SKIP dependency, but
# it does not activate the target TaskScheduler and should not consume stream
# credit. REDUCTION carries the activation/header semantics for reduction cores.
CREDIT_EDGE_PHASE_TYPES = {"INIT", "FIFO_IN", "REDUCTION", "RES"}


class TopologyParser:
    """
    拓扑地图的唯一解析中心。
    所有模块都只能通过本类的接口获取拓扑数据，严禁私自读取 JSON。
    """
    def __init__(self, filepath: str|None = None, raw_data: dict|None = None):
        if filepath:
            with open(filepath, "r", encoding="utf-8") as f:
                self.raw_data = json.load(f)
        elif raw_data is not None:
            self.raw_data = raw_data
        else:
            self.raw_data = {}

        # 缓存区，避免多次调用重复计算
        self._incoming_edges_cache = None
        self._init_graph_cache = None

    def get_raw_data(self) -> dict:
        return self.raw_data

    def get_meta_info(self) -> dict:
        return self.raw_data.get("meta", {})

    def get_timeline(self) -> List[str]:
        return self.get_meta_info().get("timeline", [])

    def get_timeline_volumes(self) -> List[Any]:
        return self.get_meta_info().get("timeline_volumes", [])

    def get_all_hosts(self) -> List[dict]:
        return self.raw_data.get("hosts", [])

    def get_all_outs(self) -> List[dict]:
        return self.raw_data.get("outs", [])

    def _get_optional_list(self, key: str) -> List[dict]:
        value = self.raw_data.get(key, [])
        if value is None:
            return []
        if not isinstance(value, list):
            raise ValueError(f"'{key}' must be a list.")
        for index, item in enumerate(value):
            if not isinstance(item, dict):
                raise ValueError(f"{key}[{index}] must be an object.")
        return value

    def get_pcie_switches(self) -> List[dict]:
        return self._get_optional_list("pcie_switches")

    def get_pcie_links(self) -> List[dict]:
        return self._get_optional_list("pcie_links")

    def get_pcie_routes(self) -> List[dict]:
        return self._get_optional_list("pcie_routes")

    def get_pcie_flows(self) -> List[dict]:
        return self._get_optional_list("pcie_flows")

    def get_all_cores_config(self) -> dict:
        return self.raw_data.get("cores", {})

    def get_moe_ep_groups(self) -> List[dict]:
        """Return validated logical EPGroup metadata, independent of Hosts."""
        groups = self.raw_data.get("moe_ep_groups", [])
        if groups is None:
            return []
        if not isinstance(groups, list):
            raise ValueError("'moe_ep_groups' must be a list.")

        seen_ids = set()
        seen_group_ids = set()
        validated = []
        for index, group in enumerate(groups):
            where = f"moe_ep_groups[{index}]"
            if not isinstance(group, dict):
                raise ValueError(f"{where} must be an object.")

            entry_id = str(group.get("id", "")).strip()
            if not entry_id:
                raise ValueError(f"{where}.id is required.")
            if entry_id in seen_ids:
                raise ValueError(f"Duplicate MoE EPGroup id: {entry_id}")

            if "group_id" not in group or group["group_id"] is None:
                raise ValueError(f"{where}.group_id is required.")
            group_id = group["group_id"]
            if isinstance(group_id, (list, dict, bool)):
                raise ValueError(f"{where}.group_id must be an integer or string.")
            group_key = str(group_id).strip().upper()
            if not group_key:
                raise ValueError(f"{where}.group_id cannot be empty.")
            if group_key in seen_group_ids:
                raise ValueError(f"Duplicate MoE group_id: {group_id}")

            if "shared_ep_id" not in group:
                raise ValueError(f"{where}.shared_ep_id is required.")
            shared_ep_id = group["shared_ep_id"]
            if shared_ep_id is not None and (
                isinstance(shared_ep_id, bool)
                or not isinstance(shared_ep_id, int)
                or shared_ep_id < 0
            ):
                raise ValueError(
                    f"{where}.shared_ep_id must be a non-negative integer or null."
                )

            routed_ep_num = group.get("routed_ep_num")
            if (
                isinstance(routed_ep_num, bool)
                or not isinstance(routed_ep_num, int)
                or routed_ep_num < 0
            ):
                raise ValueError(
                    f"{where}.routed_ep_num must be a non-negative integer."
                )

            seen_ids.add(entry_id)
            seen_group_ids.add(group_key)
            validated.append(dict(group))

        return validated

    def get_moe_host_bindings(self, ep_groups=None) -> dict:
        """Return Host ID -> group_id list, with compatibility for old maps."""
        groups = self.get_moe_ep_groups() if ep_groups is None else ep_groups
        known_groups = {str(group["group_id"]).strip().upper(): group["group_id"] for group in groups}
        hosts_by_key = {
            str(host.get("id", "")).strip().upper(): host
            for host in self.get_all_hosts()
            if str(host.get("id", "")).strip()
        }
        bindings = {key: [] for key in hosts_by_key}
        owners = {}

        for host_key, host in hosts_by_key.items():
            raw_bindings = host.get("ep_groups")
            if raw_bindings is None:
                continue
            if not isinstance(raw_bindings, list):
                raise ValueError(f"Host '{host.get('id')}'.ep_groups must be a list.")
            for value in raw_bindings:
                value_key = str(value).strip().upper()
                if value_key not in known_groups:
                    raise ValueError(
                        f"Host '{host.get('id')}' references unknown EPGroup '{value}'."
                    )
                if value_key in owners and owners[value_key] != host_key:
                    raise ValueError(
                        f"EPGroup '{value}' is bound to multiple Hosts: "
                        f"'{owners[value_key]}' and '{host_key}'."
                    )
                owners[value_key] = host_key
                canonical = known_groups[value_key]
                if canonical not in bindings[host_key]:
                    bindings[host_key].append(canonical)

        # Compatibility with the intermediate format where host_id lived in EPGroup.
        for group in groups:
            value_key = str(group["group_id"]).strip().upper()
            legacy_host_id = str(group.get("host_id", "")).strip()
            if not legacy_host_id or value_key in owners:
                continue
            host_key = legacy_host_id.upper()
            if host_key not in hosts_by_key:
                raise ValueError(
                    f"EPGroup '{group['id']}' references unknown legacy Host '{legacy_host_id}'."
                )
            owners[value_key] = host_key
            bindings[host_key].append(group["group_id"])

        return {
            hosts_by_key[key]["id"]: values
            for key, values in bindings.items()
            if values
        }

    def get_eth_switch_expert_group_map(self) -> dict[int, tuple[int, ...]] | None:
        """Return explicit or layout-derived EthSwitch expert -> group mapping.

        Supported map schema, in priority order:

        - ``eth_switch.expert_group_map``
        - ``meta.eth_switch_expert_group_map``
        - ``meta.expert_group_map``
        - ``phase2_expert_layout.groups[].routed_expert_range``

        JSON object keys are accepted for explicit maps, so ``{"77": 4}`` and
        ``{"77": [4, 5]}`` are both valid.
        """

        explicit = self._find_explicit_expert_group_map()
        if explicit is not None:
            return self._normalize_expert_group_map_schema(explicit, "expert_group_map")
        return self._derive_expert_group_map_from_phase2_layout()

    def _find_explicit_expert_group_map(self) -> Any | None:
        eth_switch = self.raw_data.get("eth_switch")
        if isinstance(eth_switch, Mapping) and "expert_group_map" in eth_switch:
            return eth_switch["expert_group_map"]

        meta = self.get_meta_info()
        for key in ("eth_switch_expert_group_map", "expert_group_map"):
            if key in meta:
                return meta[key]

        if "eth_switch_expert_group_map" in self.raw_data:
            return self.raw_data["eth_switch_expert_group_map"]
        return None

    @classmethod
    def _normalize_expert_group_map_schema(
        cls,
        value: Any,
        where: str,
    ) -> dict[int, tuple[int, ...]]:
        if value is None:
            return {}
        if isinstance(value, Mapping):
            items = list(value.items())
        elif isinstance(value, list):
            items = value
        else:
            raise ValueError(f"{where} must be an object or list of pairs.")

        normalized: dict[int, tuple[int, ...]] = {}
        for index, item in enumerate(items):
            entry_where = f"{where}[{index}]"
            if not isinstance(item, (list, tuple)) or len(item) != 2:
                raise ValueError(f"{entry_where} must be an expert/group pair.")
            expert_id = cls._non_negative_int(item[0], f"{entry_where}.expert_id")
            if expert_id in normalized:
                raise ValueError(f"Duplicate expert id in {where}: {expert_id}")
            groups = cls._normalize_group_spec(item[1], f"{entry_where}.groups")
            normalized[expert_id] = groups
        return normalized

    @classmethod
    def _normalize_group_spec(cls, value: Any, where: str) -> tuple[int, ...]:
        raw_values = value if isinstance(value, list) else [value]
        if not raw_values:
            raise ValueError(f"{where} must contain at least one group id.")
        groups: list[int] = []
        seen: set[int] = set()
        for raw_group in raw_values:
            group_id = cls._non_negative_int(raw_group, where)
            if group_id in seen:
                raise ValueError(f"{where} contains duplicate group id {group_id}.")
            seen.add(group_id)
            groups.append(group_id)
        return tuple(groups)

    @staticmethod
    def _non_negative_int(value: Any, where: str) -> int:
        if isinstance(value, bool):
            raise ValueError(f"{where} must be a non-negative integer.")
        try:
            normalized = int(value)
        except (TypeError, ValueError) as exc:
            raise ValueError(f"{where} must be a non-negative integer.") from exc
        if normalized < 0:
            raise ValueError(f"{where} must be a non-negative integer.")
        return normalized

    def _derive_expert_group_map_from_phase2_layout(self) -> dict[int, tuple[int, ...]] | None:
        layout = self.raw_data.get("phase2_expert_layout")
        if not isinstance(layout, Mapping):
            return None
        groups = layout.get("groups")
        if not isinstance(groups, list) or not groups:
            return None

        expert_to_groups: dict[int, list[int]] = {}
        saw_expert_spec = False
        for index, group in enumerate(groups):
            where = f"phase2_expert_layout.groups[{index}]"
            if not isinstance(group, Mapping):
                raise ValueError(f"{where} must be an object.")
            group_id = self._non_negative_int(group.get("group_id"), f"{where}.group_id")
            experts = self._layout_group_experts(group, where)
            if experts is None:
                continue
            saw_expert_spec = True
            for expert_id in experts:
                expert_to_groups.setdefault(expert_id, [])
                if group_id not in expert_to_groups[expert_id]:
                    expert_to_groups[expert_id].append(group_id)

        if not saw_expert_spec:
            return None
        return {
            expert_id: tuple(groups)
            for expert_id, groups in sorted(expert_to_groups.items())
        }

    def _layout_group_experts(
        self,
        group: Mapping[str, Any],
        where: str,
    ) -> tuple[int, ...] | None:
        if "routed_expert_range" in group:
            expert_range = group["routed_expert_range"]
            if not isinstance(expert_range, list) or len(expert_range) != 2:
                raise ValueError(f"{where}.routed_expert_range must be [first, last].")
            first = self._non_negative_int(expert_range[0], f"{where}.routed_expert_range[0]")
            last = self._non_negative_int(expert_range[1], f"{where}.routed_expert_range[1]")
            if last < first:
                raise ValueError(f"{where}.routed_expert_range must be ascending.")
            return tuple(range(first, last + 1))

        for key in ("routed_expert_ids", "routed_experts"):
            value = group.get(key)
            if value is None:
                continue
            if not isinstance(value, list):
                raise ValueError(f"{where}.{key} must be a list.")
            experts = tuple(
                self._non_negative_int(item, f"{where}.{key}") for item in value
            )
            if not experts:
                raise ValueError(f"{where}.{key} must not be empty.")
            if len(set(experts)) != len(experts):
                raise ValueError(f"{where}.{key} must not contain duplicates.")
            return experts

        return None

    def get_core_config(self, core_id: int) -> dict:
        return self.get_all_cores_config().get(str(core_id), {})

    def get_core_group_id(self, core_id: int) -> Any | None:
        return self.get_core_config(core_id).get("group_id")

    def core_has_init_parent(self, core_id: int) -> bool:
        for host in self.get_all_hosts():
            target = host.get("target")
            if target is None:
                targets = host.get("targets", [])
                target = targets[0] if targets else None
            if target is not None and int(target) == core_id:
                return True

        timeline = self.get_timeline()
        for info in self.get_all_cores_config().values():
            timeline_data = info.get("timeline_data", {})
            for phase_idx_str, phase_info in timeline_data.items():
                try:
                    phase_idx = int(phase_idx_str)
                except (TypeError, ValueError):
                    continue
                if phase_idx >= len(timeline):
                    continue
                if timeline[phase_idx] not in {"INIT", "FIFO_IN"}:
                    continue
                if core_id in [int(t) for t in phase_info.get("targets", [])]:
                    return True

        return False

    def get_edge_core_type(self, phase_type: str, target: int) -> CoreType:
        if phase_type == "REDUCTION":
            return CoreType.REDUCTION
        if self.core_uses_fifo(target):
            return CoreType.BROADCAST
        return CoreType.NORMAL

    def core_uses_fifo(self, core_id: int) -> bool:
        timeline = self.get_timeline()
        timeline_data = self.get_core_config(core_id).get("timeline_data", {})

        for phase_idx_str, phase_info in timeline_data.items():
            try:
                phase_idx = int(phase_idx_str)
            except (TypeError, ValueError):
                continue
            if phase_idx >= len(timeline):
                continue
            if timeline[phase_idx] == "FIFO_IN" and phase_info.get("targets", []):
                return True

        return False

    def core_uses_reduction(self, core_id: int) -> bool:
        """Return True only for cores that use the ReductionCore entry phase."""
        timeline = self.get_timeline()
        cores_data = self.get_all_cores_config()

        for cid_str, info in cores_data.items():
            timeline_data = info.get("timeline_data", {})
            for phase_idx_str, phase_info in timeline_data.items():
                try:
                    phase_idx = int(phase_idx_str)
                except (TypeError, ValueError):
                    continue
                if (
                    phase_idx >= len(timeline)
                    or timeline[phase_idx] not in REDUCTION_CORE_PHASE_TYPES
                ):
                    continue

                if int(cid_str) == core_id:
                    return True
                target = phase_info.get("target")
                if target is not None and int(target) == core_id:
                    return True

        return False

    def get_core_type(self, core_id: int) -> CoreType:
        uses_fifo = self.core_uses_fifo(core_id)
        uses_reduction = self.core_uses_reduction(core_id)

        if uses_fifo and uses_reduction:
            raise RuntimeError(
                f"Core {core_id} is classified as both BROADCAST and REDUCTION."
            )
        if uses_fifo:
            return CoreType.BROADCAST
        if uses_reduction:
            return CoreType.REDUCTION
        return CoreType.NORMAL

    def get_core_type_graph(self) -> dict:
        graph = {}
        timeline = self.get_timeline()
        cores_data = self.get_all_cores_config()

        for cid_str, info in cores_data.items():
            cid = int(cid_str)
            edges = {}
            timeline_data = info.get("timeline_data", {})

            for phase_idx_str, phase_info in timeline_data.items():
                phase_idx = int(phase_idx_str)
                if phase_idx >= len(timeline):
                    continue

                phase_type = timeline[phase_idx]
                if phase_type not in CREDIT_EDGE_PHASE_TYPES:
                    continue

                targets = []
                if "targets" in phase_info:
                    targets.extend(phase_info["targets"])
                if "target" in phase_info and phase_info["target"] is not None:
                    targets.append(phase_info["target"])

                for target in targets:
                    target = int(target)
                    if (
                        phase_type == "REDUCTION"
                        and self.core_has_init_parent(target)
                    ):
                        continue

                    core_type = self.get_edge_core_type(phase_type, target)
                    previous_type = edges.get(target)
                    if previous_type is not None and previous_type is not core_type:
                        raise RuntimeError(
                            f"Core {cid} registers downstream Core {target} with both "
                            f"{previous_type.name} and {core_type.name} core types."
                        )
                    edges[target] = core_type

            graph[cid] = [
                {"target": target, "core_type": core_type}
                for target, core_type in sorted(edges.items())
            ]

        for host in self.get_all_hosts():
            target = host.get("target")
            if target is None:
                targets = host.get("targets", [])
                target = targets[0] if targets else None
            if target is None:
                continue

            target = int(target)
            self.get_core_type(target)

        return graph

    # ==========================================
    # 高级业务逻辑：被收编的游击队
    # ==========================================
    
    def get_incoming_edges_per_phase(self) -> dict:
        """从 software.py 收编过来: 分析每个阶段的入度边"""
        if self._incoming_edges_cache is not None:
            return self._incoming_edges_cache
            
        timeline = self.get_timeline()
        cores_data = self.get_all_cores_config()
        incoming_edges = {}
        
        for phase_idx_int, phase_type in enumerate(timeline):
            phase_idx = str(phase_idx_int)
            if (
                phase_type == "CONCAT"
                or phase_type in REDUCE_LIKE_PHASE_TYPES
                or phase_type in RES_EDGE_PHASE_TYPES
            ):
                edges = {}
                for core_id_str, config in cores_data.items():
                    target = config.get("timeline_data", {}).get(phase_idx, {}).get("target")
                    if target is not None:
                        edges.setdefault(target, []).append(int(core_id_str))
                
                # 排序保证仿真确定性
                for target in edges:
                    edges[target].sort()
                
                incoming_edges[phase_idx] = edges
                
        self._incoming_edges_cache = incoming_edges
        return incoming_edges

    def get_init_graph(self) -> dict:
        """从 chip_connect.py 收编过来: 提取所有核心的 INIT 依赖图"""
        if self._init_graph_cache is not None:
            return self._init_graph_cache
            
        graph = {}
        timeline = self.get_timeline()
        cores_data = self.get_all_cores_config()
        
        for cid_str, info in cores_data.items():
            cid = int(cid_str)
            init_targets = set()
            timeline_data = info.get("timeline_data", {})
            
            for phase_idx_str, phase_info in timeline_data.items():
                phase_idx = int(phase_idx_str)
                if phase_idx >= len(timeline):
                    continue
                if timeline[phase_idx] in ["INIT", "FIFO_IN"]:
                    if "targets" in phase_info:
                        init_targets.update(phase_info["targets"])
                    if "target" in phase_info and phase_info["target"] is not None:
                        init_targets.add(phase_info["target"])
                        
            graph[cid] = sorted(list(init_targets))
        
        self._init_graph_cache = graph
        return graph

    def get_moe_profile(self) -> dict:
        """
        提取全局 MoE 架构配置字典
        """
        return self.raw_data.get("moe_profile", {})
