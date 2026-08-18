import random
import warnings
from collections import defaultdict
from dataclasses import dataclass
from config import GLOBAL_CONFIG, resolve_skew_factor
from monitor import GLOBAL_MONITOR, LogLevel, SNAPSHOT_CORE_MOE_DISPATCHER
from hardware_config import CommInstPacket
from map_phaser import get_host_info


@dataclass(frozen=True)
class MoEEPGroup:
    name_id: str
    group_id: object


class MoEDispatcher:
    """
    MoE 大堂经理，透明拦截并调度 Token 到具体的 EP Group
    """

    def __init__(self, env, all_devices, monitor=None, config=None):
        self.env = env
        self.config = config or GLOBAL_CONFIG
        self.monitor = monitor or GLOBAL_MONITOR
        self.dense_hosts = []
        self.moe_hosts = []

        # Routing resources contain logical EPGroups only.
        self.shared_experts = []
        self.routed_pool = []
        self.shared_by_ep_group = defaultdict(list)
        self.routed_by_ep_group = defaultdict(list)
        self.group_to_host = {}

        hosts = [dev for dev in all_devices if type(dev).__name__ == "Host"]
        ep_groups = getattr(self.config, "MOE_EP_GROUPS", [])
        if ep_groups:
            bindings = getattr(self.config, "MOE_HOST_BINDINGS", {})
            self._build_pools_from_ep_groups(hosts, ep_groups, bindings)
        else:
            self._build_legacy_pools(hosts)

        self.moe_profile = getattr(self.config, "MOE_PROFILE", {})
        self.strategy = self.moe_profile.get("strategy", {})
        self.routing_policy = self._resolve_routing_policy()
        if self.moe_hosts:
            self._validate_active_expert_config()
        self.rng = random.Random(self.config.RANDOM_SEED)

        self.hotspot_group_keys = list(self.routed_by_ep_group.keys())
        self.rng.shuffle(self.hotspot_group_keys)
        self._hotspot_ep_idx = 0

        self.skew_factor = resolve_skew_factor(self.config)

    def _resolve_routing_policy(self):
        # The only tolerated fallback is a missing policy, because old maps can
        # omit it while still carrying enough expert-count information to run.
        # A present but unknown policy is malformed configuration.
        policy = self.strategy.get("routing_policy")
        if policy is None or not str(policy).strip():
            if self.moe_hosts:
                warnings.warn(
                    "MoE routing_policy is missing; falling back to RANDOM.",
                    RuntimeWarning,
                    stacklevel=2,
                )
            return "RANDOM"

        policy = str(policy).strip().upper()
        if policy not in {"RANDOM", "HOTSPOT", "BALANCED", "SKEWED"}:
            raise RuntimeError(f"Unknown MoE routing_policy '{policy}'.")
        return policy

    def _validate_active_expert_config(self):
        # Active expert counts shape the generated HitMap; treating them as 0/1
        # by default would silently change model semantics.
        for field in ("active_shared_experts", "active_routed_experts"):
            if field not in self.strategy or self.strategy[field] is None:
                raise RuntimeError(f"MoE strategy is missing required {field}.")
            value = self.strategy[field]
            if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                raise RuntimeError(f"MoE strategy {field} must be a non-negative integer.")

    @staticmethod
    def _device_host_id(host):
        return str(getattr(host, "map_host_id", host.name_id)).strip().upper()

    @staticmethod
    def _group_key(group_id):
        return str(group_id).strip().upper()

    def _build_pools_from_ep_groups(self, hosts, ep_groups, bindings):
        """Build logical routing pools, then attach groups to physical Hosts."""
        groups_by_key = {}
        for config in ep_groups:
            group = MoEEPGroup(str(config["id"]), config["group_id"])
            key = self._group_key(group.group_id)
            if key in groups_by_key:
                raise RuntimeError(f"Duplicate runtime EPGroup ID: {group.group_id}")
            groups_by_key[key] = group

            if config["shared_ep_id"] is not None:
                self.shared_experts.append(group)
                self.shared_by_ep_group[group.name_id].append(group)
            for _ in range(config["routed_ep_num"]):
                self.routed_pool.append(group)
                self.routed_by_ep_group[group.name_id].append(group)

        hosts_by_id = {self._device_host_id(host): host for host in hosts}
        moe_host_set = set()
        for host_id, group_ids in bindings.items():
            host = hosts_by_id.get(str(host_id).strip().upper())
            if host is None:
                raise RuntimeError(f"MoE binding references missing runtime Host '{host_id}'.")
            for group_id in group_ids:
                group = groups_by_key.get(self._group_key(group_id))
                if group is None:
                    raise RuntimeError(
                        f"Host '{host_id}' references unknown runtime EPGroup '{group_id}'."
                    )
                previous = self.group_to_host.get(group)
                if previous is not None and previous is not host:
                    raise RuntimeError(
                        f"EPGroup '{group_id}' is bound to multiple runtime Hosts."
                    )
                self.group_to_host[group] = host
            if group_ids and host not in moe_host_set:
                self.moe_hosts.append(host)
                moe_host_set.add(host)

        unbound = [
            group.name_id for group in groups_by_key.values() if group not in self.group_to_host
        ]
        if unbound:
            raise RuntimeError(
                "MoE EPGroups have no Host binding: " + ", ".join(unbound)
            )
        self.dense_hosts.extend(host for host in hosts if host not in moe_host_set)

    def _build_legacy_pools(self, hosts):
        """Keep maps with Host-local expert metadata working."""
        seen_groups = set()
        for host in hosts:
            info = self._get_host_info(getattr(host, "map_host_id", host.name_id))
            if not info or not info.get("is_moe"):
                self.dense_hosts.append(host)
                continue

            group_id = self._host_group_id(host)
            if group_id is None:
                raise RuntimeError(f"Legacy MoE Host '{host.name_id}' has no group_id.")
            key = self._group_key(group_id)
            if key in seen_groups:
                raise RuntimeError(f"Legacy EPGroup '{group_id}' has multiple Hosts.")
            seen_groups.add(key)

            group = MoEEPGroup(str(host.name_id), group_id)
            self.moe_hosts.append(host)
            self.group_to_host[group] = host
            if info.get("shared_ep_id") is not None:
                self.shared_experts.append(group)
                self.shared_by_ep_group[group.name_id].append(group)
            for _ in range(info.get("routed_ep_num", 0)):
                self.routed_pool.append(group)
                self.routed_by_ep_group[group.name_id].append(group)

    def _get_host_info(self, name_id):
        target = str(name_id).strip()
        target_norm = target.upper()

        decoded_info = get_host_info(target, runtime_config=self.config)
        if not isinstance(decoded_info, dict):
            self.monitor.take_snapshot(
                event_note=f"🔍 [MoE Dispatcher] get_host_info({target}) 返回 {decoded_info}",
                level=LogLevel.NOTE,
                core_id=SNAPSHOT_CORE_MOE_DISPATCHER,
            )
            decoded_info = {}

        external_info = None
        for coords, info in self.config.EXTERNAL_NODES.items():
            raw_id = info.get("name_id", "")
            cur_id = str(raw_id).strip().upper()
            if cur_id == target_norm:
                external_info = dict(info)
                external_info["external_coords"] = coords
                break

        if external_info is None and not decoded_info:
            self.monitor.take_snapshot(
                event_note=f"🔍 [MoE Dispatcher] 未找到 Host {target} 的配置",
                level=LogLevel.NOTE,
                core_id=SNAPSHOT_CORE_MOE_DISPATCHER,
            )
            return None

        merged_info = {}
        if external_info:
            merged_info.update(external_info)
        merged_info.update(decoded_info)
        return merged_info

    def _host_group_id(self, host):
        parser = getattr(self.config, "MAP_PARSER", None)
        if parser is None:
            return None
        target_id = getattr(host, "target_id", None)
        if target_id is None:
            return None
        return parser.get_core_group_id(int(target_id))

    def _build_hit_map(self, group_hits):
        return tuple(group.group_id for group in group_hits)

    def _build_moe_bitmap(self, group_hits):
        # Extended Comm Header vector: each active EPG carries its local expert count.
        return tuple(
            (group.group_id, int(multiplier))
            for group, multiplier in group_hits.items()
        )

    def _expected_output_fragments(self, group_hits):
        parser = getattr(self.config, "MAP_PARSER", None)
        if parser is not None and "REDUCTION" in parser.get_timeline():
            return max(1, len(parser.get_all_outs()))
        return len(group_hits)

    # FIXME: 两个工作流的UserID不应该冲突
    def start(self, cores, *, enable_moe_dispatch=True):
        # 1. 放行 Dense 节点的传统工作流
        for h in self.dense_hosts:
            self.env.process(h.user_generator(cores))

        # 2. 启动 MoE 全局发牌机
        if enable_moe_dispatch and self.moe_hosts:
            self.env.process(self.dispatch_tokens())

    def _route_random(self, act_shared, act_routed):
        """
        Random 路由策略实现
        """
        self.monitor.take_snapshot(
            event_note=f"命中Random策略",
            level=LogLevel.NOTE,
            core_id=SNAPSHOT_CORE_MOE_DISPATCHER,
        )
        group_hits = defaultdict(int)

        # 1. Shared 路由：在拥有 Shared 副本的节点池中，随机挑选节点执行
        # (通常 act_shared 为 1，即只挑 1 个物理节点干活)
        if self.shared_experts and act_shared > 0:
            k = min(act_shared, len(self.shared_experts))
            chosen_shared = self.rng.sample(self.shared_experts, k)
            for h in chosen_shared:
                group_hits[h] += 1

        # 2. Routed 路由：从摊平的专家池中随机抽卡
        if self.routed_pool and act_routed > 0:
            k = min(act_routed, len(self.routed_pool))
            chosen_indices = self.rng.sample(range(len(self.routed_pool)), k)
            for idx in chosen_indices:
                hit_group = self.routed_pool[idx]
                group_hits[hit_group] += 1

        return group_hits

    def _route_hotspot(self, act_shared, act_routed):
        """
        Hotspot (Corner Case 1): 极致热点分发。
        尽力压入一个 EP Group，如果满了，则剩下的“抱团”转场到下一个随机 Group。
        """
        group_hits = defaultdict(int)
        groups = list(self.hotspot_group_keys)
        if not groups:
            return group_hits

        self.monitor.take_snapshot(
            event_note=f"命中Hotspot策略",
            level=LogLevel.NOTE,
            core_id=SNAPSHOT_CORE_MOE_DISPATCHER,
        )

        # 1. 确定初始中心并步进指针
        target_idx = self._hotspot_ep_idx
        self._hotspot_ep_idx = (self._hotspot_ep_idx + 1) % len(groups)

        # 2. 构造转场路径：[当前目标, 随机打乱的其他组...]
        first_group = groups[target_idx]
        other_groups = [g for g in groups if g != first_group]
        self.rng.shuffle(other_groups)
        search_path = [first_group] + other_groups

        # 3. 逐个组进行“饱和填充”
        remaining = act_routed
        for g in search_path:
            if remaining <= 0:
                break

            slots = self.routed_by_ep_group[g]
            if not slots:
                continue

            # 如果是路径上的最后一个组，或者剩下的够放了，就全吞掉
            # 否则，填满当前组，剩下的转场
            if len(slots) >= remaining or g == search_path[-1]:
                take = remaining
            else:
                take = len(slots)

            # 组内尽量平均分配这部分 take
            for i in range(take):
                h = slots[i % len(slots)]
                group_hits[h] += 1

            remaining -= take

        # 4. 共享专家的地域亲和性 (Locality Priority)
        if act_shared > 0 and self.shared_experts:
            # 始终优先考虑第一个命中的热点中心 (first_group)
            shared_in_group = self.shared_by_ep_group.get(first_group, [])
            if shared_in_group:
                chosen_shared = self.rng.sample(
                    shared_in_group, min(act_shared, len(shared_in_group))
                )
            else:
                chosen_shared = self.rng.sample(
                    self.shared_experts, min(act_shared, len(self.shared_experts))
                )

            for h in chosen_shared:
                group_hits[h] += 1

        return group_hits

    def _route_balanced(self, act_shared, act_routed):
        """
        Balanced (Unified): 全局负载均衡。
        将 Shared 和 Routed 专家视为一个整体池子，统一计算配额，
        通过让 Shared 优先“占座”，强制 Routed 避开已占用组，实现真正的无碰撞均衡。
        """
        self.monitor.take_snapshot(
            event_note=f"命中Balanced策略 (Unified Pool)",
            level=LogLevel.NOTE,
            core_id=SNAPSHOT_CORE_MOE_DISPATCHER,
        )
        group_hits = defaultdict(int)

        # 1. 建立物理组索引
        r_groups = list(self.routed_by_ep_group.keys())
        s_hosts_map = self.shared_by_ep_group
        s_groups = list(s_hosts_map.keys())

        # 2. 确定全局物理资源池 (所有能干活的组)
        union_groups = list(set(r_groups) | set(s_groups))
        if not union_groups:
            return group_hits

        # 3. 计算全局配额 (Base + Extra)
        total_needed = act_shared + act_routed
        n = len(union_groups)
        base = total_needed // n
        extra = total_needed % n
        extra_targets = (
            set(self.rng.sample(union_groups, extra)) if extra > 0 else set()
        )

        # 每个组在大平层分配中分到的“席位”
        group_quotas = {
            g: (base + (1 if g in extra_targets else 0)) for g in union_groups
        }

        # 4. 【第一阶段】共享专家优先占座
        # 共享专家副本有限，必须优先满足。由于它占用了配额，后续 Routed 就会避开这些组
        remaining_shared = act_shared
        # 找出有 Shared 能力且分到了配额的组
        available_s_groups = [g for g in s_groups if group_quotas[g] > 0]
        self.rng.shuffle(available_s_groups)

        for g in available_s_groups:
            if remaining_shared <= 0:
                break
            # 在配额允许范围内，尽量多塞点（通常 act_shared 只有 1，所以这里多为 1）
            take = min(remaining_shared, group_quotas[g], len(s_hosts_map[g]))
            for i in range(take):
                h = s_hosts_map[g][i]
                group_hits[h] += 1
                group_quotas[g] -= 1
                remaining_shared -= 1

        # 5. 【第二阶段】路由专家填补剩余配额
        # 此时 group_quotas 中剩下的席位，就是避开了 Shared 之后最理想的空位
        remaining_routed = act_routed
        # 优先填补还有配额的 Routed 组
        for g in r_groups:
            if remaining_routed <= 0:
                break
            if group_quotas.get(g, 0) <= 0:
                continue

            slots = self.routed_by_ep_group[g]
            take = min(remaining_routed, group_quotas[g])
            for i in range(take):
                h = slots[i % len(slots)]
                group_hits[h] += 1
                remaining_routed -= 1
                group_quotas[g] -= 1

        # 6. 【兜底阶段】处理溢出
        # 如果因为某些组不支持某种能力导致没分完，直接强行分发，放弃配额约束
        if remaining_shared > 0:
            for g in s_groups:
                if remaining_shared <= 0:
                    break
                h = s_hosts_map[g][0]
                group_hits[h] += 1
                remaining_shared -= 1

        if remaining_routed > 0:
            for g in r_groups:
                if remaining_routed <= 0:
                    break
                h = self.routed_by_ep_group[g][0]
                group_hits[h] += 1
                remaining_routed -= 1

        return group_hits

    def _route_skewed(self, act_shared, act_routed, alpha):
        """
        Skewed (Tunable): 通过 alpha [0, 1] 调节负载的集中度。
        alpha=0: 各 routed group 等概率、带放回抽样（单个 Token 不保证均衡）
        alpha=1: 极致热点 (HotSpot)
        中间值: 概率向目标组倾斜
        """
        group_hits = defaultdict(int)
        groups = list(self.routed_by_ep_group.keys())
        if not groups:
            return self._route_random(act_shared, act_routed)

        # 1. 选定本次 Token 的目标组 (轮询保证宏观公平)
        target_group = groups[self._hotspot_ep_idx]
        self._hotspot_ep_idx = (self._hotspot_ep_idx + 1) % len(groups)

        # 2. 计算权重分布 (基于 alpha 的线性插值)
        # Target Weight = 1/N + alpha * (1 - 1/N)
        # Other Weight = (1 - Target Weight) / (N - 1)
        n = len(groups)
        if n > 1:
            w_target = (1.0 / n) + alpha * (1.0 - (1.0 / n))
            w_other = (1.0 - w_target) / (n - 1)
            weights = [w_target if g == target_group else w_other for g in groups]
        else:
            weights = [1.0]

        self.monitor.take_snapshot(
            event_note=f"命中Skewed策略 (alpha={alpha:.2f}, Target={target_group}, TargetWeight={weights[groups.index(target_group)]:.2f})",
            level=LogLevel.NOTE,
            core_id=SNAPSHOT_CORE_MOE_DISPATCHER,
        )

        # 3. 抽样分配
        # 注意：choices 是带权重的放回抽样，适合模拟概率分布
        chosen_groups = self.rng.choices(groups, weights=weights, k=act_routed)

        # 4. 落地到具体物理节点
        for g in chosen_groups:
            slots = self.routed_by_ep_group[g]
            if slots:
                # 组内随机挑一个核心
                h = self.rng.choice(slots)
                group_hits[h] += 1
            else:
                # 该组没槽位？回退到全局池子捞一个，防止崩盘
                if self.routed_pool:
                    h = self.rng.choice(self.routed_pool)
                    group_hits[h] += 1

        # 5. 共享专家处理 (跟随地域亲和性逻辑：优先选目标组内的)
        if act_shared > 0 and self.shared_experts:
            shared_in_group = self.shared_by_ep_group.get(target_group, [])
            # 这里的概率也受 alpha 影响：alpha 越大，越倾向于留在组内
            if shared_in_group and self.rng.random() < alpha:
                chosen_shared = self.rng.sample(
                    shared_in_group, min(act_shared, len(shared_in_group))
                )
            else:
                chosen_shared = self.rng.sample(
                    self.shared_experts, min(act_shared, len(self.shared_experts))
                )

            for h in chosen_shared:
                group_hits[h] += 1

        return group_hits

    def _group_hits_by_host(self, group_hits):
        groups_by_host = defaultdict(dict)
        for group, multiplier in group_hits.items():
            host = self.group_to_host.get(group)
            if host is None:
                raise RuntimeError(
                    f"Routed EPGroup '{group.name_id}' has no Host binding."
                )
            groups_by_host[host][group] = multiplier
        return groups_by_host

    def _target_for_host_group(self, host, group):
        host_targets = getattr(self.config, "PCIE_HOST_GROUP_TARGETS", {}).get(
            self._device_host_id(host)
        )
        if not host_targets:
            return None
        group_key = self._group_key(group.group_id)
        try:
            return host_targets[group_key]
        except KeyError as exc:
            raise RuntimeError(
                f"Host {host.name_id} is bound to EPGroup {group.group_id}, "
                "but no PCIE flow target was derived for that group."
            ) from exc

    def _group_hits_by_host_target(self, groups_by_host):
        groups_by_target = defaultdict(dict)
        for host, hosted_groups in groups_by_host.items():
            for group, multiplier in hosted_groups.items():
                target_id = self._target_for_host_group(host, group)
                groups_by_target[(host, target_id)][group] = multiplier
        return groups_by_target

    def dispatch_tokens(self):
        """Route logical EPGroups, then inject through their bound Hosts."""
        act_shared = self.strategy["active_shared_experts"]
        act_routed = self.strategy["active_routed_experts"]

        if self.env.now == 0:
            self.monitor.take_snapshot(
                event_note=" [MoE Dispatcher] logical routing pools\n"
                f" strategy: {self.strategy}"
                f" active: Shared={act_shared}, Routed={act_routed}\n"
                f" pool: Shared={len(self.shared_experts)}, Routed={len(self.routed_pool)}\n",
                level=LogLevel.NOTE,
                core_id=SNAPSHOT_CORE_MOE_DISPATCHER,
            )

        for uid in range(self.config.NUM_USERS):
            if self.routing_policy == "RANDOM":
                group_hits = self._route_random(act_shared, act_routed)
            elif self.routing_policy == "HOTSPOT":
                group_hits = self._route_hotspot(act_shared, act_routed)
            elif self.routing_policy == "BALANCED":
                group_hits = self._route_balanced(act_shared, act_routed)
            elif self.routing_policy == "SKEWED":
                group_hits = self._route_skewed(
                    act_shared, act_routed, self.skew_factor
                )
            else:
                self.monitor.take_snapshot(
                    event_note="Unknown MoE policy; falling back to RANDOM",
                    level=LogLevel.NOTE,
                    core_id=SNAPSHOT_CORE_MOE_DISPATCHER,
                )
                group_hits = self._route_random(act_shared, act_routed)

            if not group_hits:
                continue

            sorted_group_items = sorted(
                group_hits.items(), key=lambda item: item[0].name_id
            )
            token_group_hits = dict(sorted_group_items)
            token_hit_map = self._build_hit_map(token_group_hits)
            token_moe_bitmap = self._build_moe_bitmap(token_group_hits)
            groups_by_host = self._group_hits_by_host(token_group_hits)

            groups_by_target = self._group_hits_by_host_target(groups_by_host)
            sorted_host_items = sorted(
                groups_by_target.items(),
                key=lambda item: (
                    item[0][0].name_id,
                    -1 if item[0][1] is None else int(item[0][1]),
                ),
            )
            hit_details = [
                f"{group.name_id}(GID:{group.group_id}) x{multiplier}"
                for group, multiplier in sorted_group_items
            ]
            host_details = [
                f"{host.name_id}->{self._build_hit_map(hosted_groups)}"
                + ("" if target_id is None else f"@Core{target_id}")
                for (host, target_id), hosted_groups in sorted_host_items
            ]
            self.monitor.take_snapshot(
                event_note=f"[{self.env.now:07.1f}] [MoE Dispatcher] Token {uid} | "
                f"policy={self.routing_policy} | EPG hits={hit_details} | "
                f"Host bindings={host_details}",
                level=LogLevel.EVENT,
                user_id=uid,
                core_id=SNAPSHOT_CORE_MOE_DISPATCHER,
            )

            credit_reqs = [host.credits.get(1) for (host, _), _ in sorted_host_items]
            yield self.env.all_of(credit_reqs)
            yield self.env.timeout(self.config.HOST_PUSH_DELAY)

            expected_frags = self._expected_output_fragments(group_hits)
            self.monitor.record_global_start(uid, expected_fragments=expected_frags)

            for (host, target_id), hosted_groups in sorted_host_items:
                multiplier = sum(hosted_groups.values())
                host.comm(
                    CommInstPacket(
                        uid=uid,
                        tag=multiplier,
                        HitMap=token_hit_map,
                        MoEBitMap=token_moe_bitmap,
                    ),
                    target_id=target_id,
                )

            self.monitor.take_snapshot(
                event_note=f"[MoE Dispatcher] Token {uid} activated "
                f"{len(group_hits)} EPGroups through {len(groups_by_host)} Hosts / "
                f"{len(groups_by_target)} injections",
                level=LogLevel.EVENT,
                user_id=uid,
                core_id=SNAPSHOT_CORE_MOE_DISPATCHER,
            )
