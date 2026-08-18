from abc import ABC, abstractmethod
from dataclasses import dataclass
from enum import Enum, auto
import inspect
import math
from functools import wraps
from typing import Any


def expose_to(module_name: str, is_process: bool = False):
    """
    检查接口是否属实, 并检查协程状态
    """

    def decorator(func):
        if not is_process:
            setattr(func, "_exposed_to", module_name)
            setattr(func, "_is_process", False)
            return func

        @wraps(func)
        def wrapper(*args, **kwargs):
            # 如果标记了必须是 process (需要 yield), 主动验证它是否是生成器
            if is_process and not inspect.isgeneratorfunction(func):
                raise TypeError(
                    f"💥 Process检查错误\n"
                    f"函数 `{func.__name__}` 被标记为暴露给 `{module_name}` 的 SimPy Process,"
                    f"但函数不是 Generator! 请添加 yield."
                )
            return func(*args, **kwargs)

        # 把我们的元数据塞进函数属性里，方便后续写监控脚本抓取
        setattr(wrapper, "_exposed_to", module_name)  # type: ignore
        setattr(wrapper, "_is_process", is_process)  # type: ignore

        return wrapper

    return decorator


class UnitType(Enum):
    DTE = auto()
    MC = auto()
    VC = auto()
    # TS = auto() # TODO: Remove
    CU = auto()
    SKIP = auto()


class RespType(Enum):
    OK = 1
    ERROR = 2
    BUSY = 3
    KILL = 4  # User Retire


class CoreType(Enum):
    NORMAL = auto()
    BROADCAST = auto()
    REDUCTION = auto()


class EPGroupType(Enum):
    DENSE = auto()
    MOE = auto()


# 封装访存请求, 留个默认值, 防止以后改需求的时候抓瞎
@dataclass(slots=True)
class MemReq:
    uid: int
    tid: int
    data_size: int = 8192  # Default: 1 * 8192 @FP8, 8KiB
    is_write: bool = False


# ==========================================
# Comm Inst
# 描述有意义的指令段
# 指令设计为:
# [Tag] [Opcode]
# ==========================================


class Opcode(Enum):
    """
    + Opcode是CommInst的操作码
    |
    + Opcode    |Tag/Non| Descrip
    |
    * USER_INIT | Tag   | 用户初始化
    |   Tag将作为区分MoE和Dense的标志以及MoE激活专家的BitMap.
    |   当Tag全为0时, 表示不激活任何一个RoutedExpert, 即Dense模型.
    |   当Tag不为0时, 其作为BitMap标志激活哪些专家.
    |   指令发出核:
    |       根据发送的下游核ID, 将对应的Credits减一.
    |       随后将当前数据包传递给下游, 并传递[UID, 0]的初始化包
    |       当Credits为0时, 这个Task不应该被下发, 逻辑错误!
    |   指令接受核:
    |       在CoreMem中分配用户.
    |
    * MOVE      | Non   | 数据搬运
    |   指令发出核:
    |       将数据搬运出去.
    |   指令接受核:
    |       将数据搬运进来.
    |
    * REDUCE    | Tag   | 向量Reduce操作
    |   当前Tag用于静态TS任务对齐: 发送端把Tag作为接收端TaskID下发.
    |   指令发出核:
    |       将待处理的向量搬运出去.
    |   指令接受核:
    |       会尝试将向量与内存中已有的向量进行Reduce.
    |       如果没有找到已有向量, 则放入内存中.
    |       如果找到已有向量, 则进行Reduce操作.
    |
    * REDUCTION | Non   | ReductionCore入口操作
    |   指令发出核:
    |       将待处理的向量搬运到ReductionCore.
    |   指令接受核:
    |       通过DTE维护ReductionCore用户状态, 必要时激活TS, 并执行Reduce.
    |
    * CONCAT    | Tag   | 向量拼接
    |   Tag将作为当前核心的CoreID, 用于寻找向量拼接位置.
    |   指令发出核:
    |       将等待拼接的向量搬出, 并附上当前核心ID.
    |   指令接受核:
    |       将传入的向量存入内存的对应区域.
    |
    * RETIRE    | Tag   | 用户退休.
    |   Tag将作为当前核心的CoreID, 用于传递给上游核.
    |   指令发出核:
    |       将当前的核心ID传递给上游核心.
    |   指令接受核:
    |       通过下游核心传入的ID, 给它的Credits加一
    |
    * FIFO_IN   | Tag   | 将MatrixMem作为FIFO, 并压入数据
    |   Tag作为区分MoE和Dense的标志以及MoE激活专家的BitMap, 需要将这个信息暂存到MatrixMem中.
    |   指令发出核:
    |       将基础信息打包为 (USER ID, Comm+Init Data) 发送.
    |   指令接受核:
    |       将 (USER ID, Comm+Init Data) 压入到 FIFO 中.
    |
    * FIFO_OUT  | Tag   | 将MatrixMem作为FIFO, 并弹出数据, 转义为USER_INIT任务
    |   Tag作为区分MoE和Dense的标志以及MoE激活专家的BitMap, 在转义过程中被需要
    |   指令发出核:
    |       从FIFO中Pop出数据, 重组数据为
    |       (Comm.UID, Comm.TID=0, Comm.Opcode=USER_INIT, Comm.TAG)
    |       并发送到下游核心, 格式为Comm+PacketData
    |   指令接受核:
    |       引发异常.
    |
    * RES       | Non   | Phase1 residual入口/承接操作
    |   指令发出核:
    |       将FC0Out作为Phase1 RES入口数据发送给下游核心.
    |       Tag不承载MoE语义, 下游TaskID通过tid_override指定.
    |   指令接受核:
    |       将该数据作为本核心的真实init/activation入口.
    |       当前仿真先验证handoff和资源占用, 不声明完成数值Residual Add.
    |
    * BYPASS    | Non   | Phase1 X1旁路输出
    |   指令发出核:
    |       将RES阶段产出的X1/ResidualX1通过DTE搬运到Phase1旁路出口.
    |       BYPASS是语义化MOVE, 仍占用DTE/router资源, 不代表免费直连通路.
    |   指令接受核:
    |       当前主要面向外部出口; 若未来接收端为Core, 行为等价MOVE接收.
    """

    USER_INIT = 0
    MOVE = 1
    REDUCE = 2
    CONCAT = 3
    RETIRE = 4
    # Specific Task
    # Broadcast Core
    FIFO_IN = 5
    FIFO_OUT = 6
    REDUCTION = 7
    RES = 8
    BYPASS = 9

    # 错误码
    _DONTCARE = 404


@dataclass(frozen=True, slots=True)
class Packet(ABC):
    uid: int
    tid: int = 0  # 默认 Task ID 为0


@dataclass(frozen=True, slots=True)
class CommInstPacket(Packet):
    """
    [5:3] | [2:0   ]
    [Tag] | [Opcode]
    Opcode可能会随着之后的探索去拓展.
    Tag是Opcode的补充位. 用于在特定的Opcode下提供辅助信息.
        * 当Opcode为RETIRE,

    HitMap 表示本次 Comm 激活的 EPGroup 集合。
    MoEBitMap 是扩展 Comm Header 向量，元素为 (group_id, local_expert_count)，
    用于让一个经 NoC 广播/转发的 Comm 同时携带各 EPG 的本地 MoEBitMap 写值。

    此外, 我们提供了一个beat_id的位, 标记当前传输的拍数
    这个不是CommInst的标准内容, 而是为了仿真方便提供的. 为了区分, 采用_xxx_进行包围
    """

    opcode: Opcode = Opcode.USER_INIT
    tag: int = 0
    layer_id: int = 0
    _beat_id_: int = 0
    total_fragments: int = 0
    HitMap: tuple[Any, ...] = ()
    MoEBitMap: tuple[Any, ...] = ()
    # Fusion Stage-1 (dec A): phase1 identity fields — dormant (None) for pure-MoE
    # traffic; carried through every packet-copy site alongside HitMap/MoEBitMap.
    phase1_lane_id: str | None = None
    phase1_group_id: int | None = None

    @staticmethod
    def _normalize_tuple(value):
        if value is None:
            return ()
        if isinstance(value, dict):
            return tuple(value.items())
        if isinstance(value, (list, tuple, set, frozenset)):
            return tuple(value)
        return (value,)

    def __post_init__(self):
        object.__setattr__(self, "HitMap", self._normalize_tuple(self.HitMap))
        object.__setattr__(
            self, "MoEBitMap", self._normalize_tuple(self.MoEBitMap)
        )


@dataclass(frozen=True, slots=True)
class RoutedPacket:
    payload: CommInstPacket
    size: int
    payload_size: int
    byte_offset: int = 0
    fragment_id: int = 0
    total_fragments: int = 1
    is_tail: bool = True


class CommDevice(ABC):
    """
    这是一个抽象基类. 所有采用了Comm通信模型的都应该继承自这个类.
    """

    def __init__(self, env, router=None, recv_timeout=12800):
        # 强制要求子类上交环境管辖权
        self.env = env
        self.router = router
        self.recv_buffer = {}
        self.recv_start_times = {}  # 记录每一包 (uid, tid, tag, opcode) 的首拍到达时间
        self.recv_timeout = recv_timeout
        self._recv_watchdogs = {}
        self._recv_watchdog_process = None
        self._recv_watchdog_wake_event = None

    def _calculate_cycles(self, data_size, bandwith):
        if bandwith <= 0:
            raise ValueError(f"Bandwidth must be positive, got {bandwith}.")
        if data_size <= 0:
            return 1
        return math.ceil(data_size / bandwith)

    def _calculate_chunk_size(self, data_size: int, bandwidth: int, beat_id: int) -> int:
        if bandwidth <= 0:
            raise ValueError(f"Bandwidth must be positive, got {bandwidth}.")
        if data_size <= 0:
            return 0
        return max(0, min(bandwidth, data_size - beat_id * bandwidth))

    def _expected_burst_len(
        self,
        total_size: int,
        comm: CommInstPacket,
        fallback_bandwidth: int,
    ) -> int:
        total_fragments = getattr(comm, "total_fragments", 0)
        if total_fragments > 0:
            return total_fragments

        return self._calculate_cycles(total_size, fallback_bandwidth)

    # ==========================================
    # 抽象的公共看门狗
    # ==========================================
    def _ensure_recv_watchdog_manager(self):
        if self.env is None or self._recv_watchdog_process is not None:
            return
        self._recv_watchdog_process = self.env.process(self._recv_watchdog_loop())

    def _arm_recv_watchdog(self, key, expected_len, device_name="UnknownDevice"):
        if self.env is None:
            return
        was_empty = not self._recv_watchdogs
        self._recv_watchdogs[key] = (
            self.env.now + self.recv_timeout,
            expected_len,
            device_name,
        )
        self._ensure_recv_watchdog_manager()
        if was_empty and self._recv_watchdog_wake_event is not None:
            if not self._recv_watchdog_wake_event.triggered:
                self._recv_watchdog_wake_event.succeed()

    def _clear_recv_watchdog(self, key):
        self._recv_watchdogs.pop(key, None)

    def _recv_watchdog_loop(self):
        while True:
            if not self._recv_watchdogs:
                self._recv_watchdog_wake_event = self.env.event()
                yield self._recv_watchdog_wake_event
                continue

            next_deadline = min(
                deadline
                for deadline, _expected_len, _device_name
                in self._recv_watchdogs.values()
            )
            delay = max(0, next_deadline - self.env.now)
            if delay > 0:
                yield self.env.timeout(delay)

            now = self.env.now
            for key, (deadline, expected_len, device_name) in list(
                self._recv_watchdogs.items()
            ):
                if deadline > now:
                    continue
                if key not in self.recv_buffer:
                    self._clear_recv_watchdog(key)
                    continue
                actual_len = len(self.recv_buffer[key])
                if actual_len < expected_len:
                    raise TimeoutError(
                        f"🚨 致命异常: {device_name} 接收数据超时! "
                        f"Task {key} 期望 {expected_len} 拍，实际仅收到 {actual_len} 拍."
                    )
                self._clear_recv_watchdog(key)

    # ==========================================
    # 抽象的公共接收验资方法
    # 返回 True 表示收齐了，False 表示还在等
    # ==========================================
    def _register_beat(
        self, comm: CommInstPacket, expected_len: int, device_name: str
    ) -> bool:
        key = (comm.uid, comm.layer_id, comm.tid, comm.tag, comm.opcode)

        # 🛡️ 验证 1：防越界 (Out-of-Bounds Check)
        if comm._beat_id_ < 0 or comm._beat_id_ >= expected_len:
            raise ValueError(
                f"🚨 [{device_name} 安检失败] 收到越界切片！"
                f"User {comm.uid} Task {comm.tid} 预期总长度为 {expected_len}，"
                f"却收到非法的 beat_id = {comm._beat_id_}。疑似路由错乱！"
            )

        # 第一次来，建档放狗
        if key not in self.recv_buffer:
            self.recv_buffer[key] = set()
            self.recv_start_times[key] = self.env.now  # 🎯 记录首拍时间
            self._arm_recv_watchdog(key, expected_len, device_name)

        # 🛡️ 验证 2：防重播/幻象包 (Duplicate/Replay Check)
        if comm._beat_id_ in self.recv_buffer[key]:
            raise RuntimeError(
                f"🚨 [{device_name} 安检失败] 收到重复切片！"
                f"User {comm.uid} Task {comm.tid} 的第 {comm._beat_id_} 拍被重复接收。"
                f"可能是网络中存在幽灵环路或数据包克隆！"
            )

        self.recv_buffer[key].add(comm._beat_id_)

        # 🛡️ 验证 3：防溢出 (Overflow Check)
        current_len = len(self.recv_buffer[key])
        if current_len == expected_len:
            del self.recv_buffer[key]
            self._clear_recv_watchdog(key)
            return True  # 功德圆满，盖章放行

        elif current_len > expected_len:
            raise RuntimeError(
                f"🚨 [{device_name} 安检失败] 缓存溢出！"
                f"预期 {expected_len} 拍，结果收到了 {current_len} 拍。"
            )

        return False

    # ==========================================
    # 抽象的公共发包器 (协程)
    # ==========================================
    def _send_burst(
        self, packet, dst_coords, dst_id, opcode, tag, vol, bandwidth, tid_override=None
    ):
        # DTE/BachModule 提供 identity-aware resolver；其他 legacy CommDevice 仍可
        # 直接使用 router 欄位。每個 burst 只捕獲一次，避免傳輸中途被換線。
        resolver = getattr(self, "_require_router", None)
        router = resolver() if callable(resolver) else self.router
        if not router:
            raise RuntimeError("❌ 连 Router 都没有，发个毛线！")
        route_packet = router.route_packet

        target_tid = tid_override if tid_override is not None else packet.tid
        burst_len = self._calculate_cycles(vol, bandwidth)

        for beat in range(burst_len):
            chunk_size = self._calculate_chunk_size(vol, bandwidth, beat)
            push_comm = CommInstPacket(
                uid=packet.uid,
                tid=target_tid,
                opcode=opcode,
                tag=tag,
                layer_id=getattr(packet, "layer_id", 0),
                _beat_id_=beat,
                total_fragments=burst_len,
                HitMap=getattr(packet, "HitMap", ()),
                MoEBitMap=getattr(packet, "MoEBitMap", ()),
                phase1_lane_id=getattr(packet, "phase1_lane_id", None),
                phase1_group_id=getattr(packet, "phase1_group_id", None),
            )
            self.env.process(
                route_packet(
                    packet=packet,
                    dst_coords=dst_coords,
                    dst_id=dst_id,
                    packet_data=push_comm,
                    src_port="LOCAL",
                    packet_size=chunk_size,
                )
            )
            yield self.env.timeout(1)

    @abstractmethod
    def comm(self, comm: CommInstPacket) -> Any:
        """
        推送信息的逻辑.
        """

        pass

    @abstractmethod
    def handle_comm(self, comm: CommInstPacket) -> Any:
        """
        处理信息的逻辑
        """

        pass
