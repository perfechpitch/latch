"""编译器产出的东西：每个核一段 kernel。

一条 kernel 是一个算子，带上操作数在本核 SRAM 里的地址和形状。核自己决定怎么分块取
数。地址由编译器静态排好，权重常驻，一层 SRAM。

核只做 FFN，所以指令就这四条：

    MC    Gemm            gate / up / down
    VC    Elemwise        swiglu
    DTE   Send  Recv      两个 master 口出，两个 slave 口进

每个核两个 slave 作为输入、两个 master 作为输出，接的是本核的路由器，路由器之间走
mesh 逐跳、只看目标坐标。Send 说走哪个 master 口、发给哪个核；Recv 从哪个核收由连接
决定，只说走哪个 slave 口、收多少、落到哪。

编译器产出两样：每个核的 kernel，和一份时序表。核只做 FFN，前后那些计算在一个外围
模块里，它照时序表按 cycle 把 FFN 的输入喂进核阵列、再收走结果。

拓扑、路由、credit 不在这里，另一份描述给。
"""

from dataclasses import dataclass, field


@dataclass
class Gemm:
    """MC。C[m,n] = A[m,k] @ B[k,n]。dtype 是 B 的位宽，A 与 C 按默认。"""
    m: int
    k: int
    n: int
    a: int
    b: int
    c: int
    dtype: str = "bf16"
    cycles: int = 0


@dataclass
class Elemwise:
    """VC。op 取 add / mul / silu / swiglu 之一，srcs 按 op 定几个。"""
    op: str
    n: int
    srcs: list
    dst: int
    limit: float = 0.0
    cycles: int = 0


@dataclass
class Send:
    """DTE。从本核 src 取 length 字节，经 master 口发给 dst_core。"""
    src: int
    length: int
    dst_core: int
    master: int


@dataclass
class Recv:
    """DTE。从 slave 口收 length 字节落到本核 dst。从谁那儿收由连接决定。"""
    slave: int
    dst: int
    length: int


@dataclass
class CoreKernel:
    """一个核的一段 kernel，按次序执行。"""
    core: int
    ops: list = field(default_factory=list)


@dataclass
class Feed:
    """时序表的一条：第 cycle 拍，往 core 的 slave 口给 length 字节。"""
    cycle: int
    core: int
    slave: int
    length: int


@dataclass
class Drain:
    """时序表的一条：外围模块在 core 的哪个 master 口上收结果。"""
    core: int
    master: int
    length: int


@dataclass
class Timing:
    """算子的拍数。有实测值用实测值，没有就按下面估。

    Gemm 估法：一次要把 k*n 的权重全读一遍，按 load_bw 字节每拍算。token 数少的时候
    权重加载就是瓶颈，所以先只按它估。
    Elemwise 估法：按元素数除以 elem_bw。
    """
    table: dict = field(default_factory=dict)
    load_bw: int = 1024
    elem_bw: int = 256


# ============================ 一段真实的样例 ============================
# DeepSeek-V4 的一个专家落在一个核上：hidden 7168，inner 3072，权重 fp4，激活 bf16，
# 这一轮路由分到它 64 个 token。编译器排出来的地址与 kernel 如下。

HIDDEN, INNER, TOK = 7168, 3072, 64
W = HIDDEN * INNER // 2                       # fp4 一个参数半字节
X, G, Y = TOK * HIDDEN * 2, TOK * INNER * 2, TOK * HIDDEN * 2

W_GATE, W_UP, W_DOWN = 0x0, W, 2 * W          # 权重常驻，排在最前
A_X = 3 * W                                   # 激活区接在权重后面
A_G, A_U, A_A, A_Y = A_X + X, A_X + X + G, A_X + X + 2 * G, A_X + X + 3 * G

EXPERT = CoreKernel(core=17, ops=[
    Recv(slave=0, dst=A_X, length=X),
    Gemm(m=TOK, k=HIDDEN, n=INNER, a=A_X, b=W_GATE, c=A_G, dtype="fp4"),
    Gemm(m=TOK, k=HIDDEN, n=INNER, a=A_X, b=W_UP,   c=A_U, dtype="fp4"),
    Elemwise("swiglu", n=TOK * INNER, srcs=[A_G, A_U], dst=A_A, limit=10.0),
    Gemm(m=TOK, k=INNER, n=HIDDEN, a=A_A, b=W_DOWN, c=A_Y, dtype="fp4"),
    Send(src=A_Y, length=Y, dst_core=273, master=0),
])


# 这一层 FFN 的时序表。外围模块算完 attention 那一段，在第 8400 拍把这 64 个 token
# 交给核 17，算完在 master 0 上收回来。

FEED = [Feed(cycle=8400, core=17, slave=0, length=X)]
DRAIN = [Drain(core=17, master=0, length=Y)]


if __name__ == "__main__":
    hexf = ("a", "b", "c", "src", "dst")
    print(f"core {EXPERT.core}")
    for op in EXPERT.ops:
        args = ", ".join(
            f"{k}=0x{v:X}" if k in hexf and isinstance(v, int)
            else f"{k}={[hex(s) for s in v]}" if k == "srcs"
            else f"{k}={v}"
            for k, v in vars(op).items())
        print(f"  {type(op).__name__:9} {args}")
    for f in FEED:
        print(f"  feed      cycle={f.cycle} core={f.core} slave={f.slave} "
              f"length={f.length}")
    for d in DRAIN:
        print(f"  drain     core={d.core} master={d.master} length={d.length}")
