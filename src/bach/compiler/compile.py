"""编译器。跑一遍模型描述，产出每个核的 kernel 和一份时序表。

核只做 FFN，所以只有带核的 ffn 调用会降成 kernel，其余算子只推形状。
"""

import json
import random
from dataclasses import dataclass, field

from kernel import (Gemm, Elemwise, Send, Recv, CoreKernel, Feed, Drain,
                    Timing as KTiming)

BITS = {"fp4": 0.5, "fp8": 1, "bf16": 2, "bfloat16": 2, "fp32": 4, "float32": 4,
        "int32": 4}

DEF = {}
CUR = []          # with 的核栈
CALLS = []        # 带核的 ffn 调用
ROUTE_CACHE = {}
SEED = [0]


def defaults(**kw):
    DEF.update(kw)


# ================================ 张量 ================================


class Axis:
    def __init__(self, t):
        self.t = t

    def __getitem__(self, s):
        n = s.stop if isinstance(s, slice) else s
        return Tensor((n,) + self.t.shape[1:], self.t.dtype, self.t.origin)


@dataclass
class Tensor:
    shape: tuple
    dtype: str = "bf16"
    origin: object = None          # router 的输出带着路由信息

    @property
    def token(self):
        return Axis(self)

    @property
    def bytes(self):
        n = 1
        for d in self.shape:
            n *= d
        return int(n * BITS[self.dtype])

    def like(self, last=None):
        s = self.shape if last is None else self.shape[:-1] + (last,)
        return Tensor(s, self.dtype, self.origin)


# ================================ 机器 ================================


class Core:
    def __init__(self, cid):
        self.id = cid

    def __enter__(self):
        CUR.append(self)
        return self

    def __exit__(self, *a):
        CUR.pop()


class Machine:
    def __init__(self, chips, cores_per_chip, inputs, outputs, credit):
        self.n = chips[0] * chips[1] * cores_per_chip[0] * cores_per_chip[1]
        self.chips, self.per_chip = chips, cores_per_chip
        self.inputs, self.outputs, self.credit = inputs, outputs, credit
        self.all = [Core(i) for i in range(self.n)]

    def cores(self):
        return self.all


class Timing(KTiming):
    @classmethod
    def load(cls, path):
        try:
            return cls(json.load(open(path)))
        except FileNotFoundError:
            return cls()

    def gemm(self, m, k, n, dtype):
        key = f"gemm/{m}/{k}/{n}/{dtype}"
        if key in self.table:
            return self.table[key]
        return max(1, int(k * n * BITS[dtype] / self.load_bw))

    def elemwise(self, op, n):
        key = f"elemwise/{op}/{n}"
        if key in self.table:
            return self.table[key]
        return max(1, n // self.elem_bw)


# ============================ 不产 kernel 的算子 ============================
# 只推形状。


def embedding(tok, vocab, dim):
    return Tensor((tok.shape[0], dim), DEF.get("dtype", "bf16"))


def linear(x, name, out, dtype=None, groups=1, **kw):
    return Tensor(x.shape[:-1] + (out,), dtype or x.dtype, x.origin)


def norm(x, name=None, **kw):
    return x.like()


def rope(x, **kw):
    return x.like()


def add(a, b):
    return a.like()


def attn(q, kv, heads, qk_dim=0, v_dim=0, **kw):
    return Tensor((q.shape[0], heads * v_dim), q.dtype)


def index_topk(q, k, heads, topk, **kw):
    return Tensor((q.shape[0], topk), "int32")


def topk(x, k, **kw):
    t = Tensor((x.shape[0], k), "int32")
    t.origin = ("route", x.shape[0], x.shape[-1], k, len(CALLS))
    return t


def concat(parts, axis):
    if axis == "token":
        return Tensor((sum(p.shape[0] for p in parts),) + parts[0].shape[1:],
                      parts[0].dtype)
    return Tensor(parts[0].shape[:-1] + (sum(p.shape[-1] for p in parts),),
                  parts[0].dtype)


def reduce_sum(parts):
    return parts[0].like()


def send(x, to, tag=None):
    pass


def wait(tag=None):
    return None


# ============================ 编译期求值 ============================


def tokens_of(sel, experts):
    """这一批 token 里有多少个路由到了 experts 这几个专家。按种子算。"""
    kind, n_tok, n_exp, k, tag = sel.origin
    key = (n_tok, n_exp, k, tag)
    if key not in ROUTE_CACHE:
        rng = random.Random(SEED[0] + tag)
        ROUTE_CACHE[key] = [rng.sample(range(n_exp), k) for _ in range(n_tok)]
    picks = ROUTE_CACHE[key]
    want = set(experts)
    return sum(1 for p in picks if want & set(p))


# ============================ 产 kernel 的算子 ============================


def ffn(x, name, inner, weight_dtype=None, limit=0.0, at=None):
    c = at or (CUR[-1] if CUR else None)
    if c is not None:
        CALLS.append(dict(core=c.id, x=x, inner=inner, name=name, limit=limit,
                          wd=weight_dtype or DEF.get("weight_dtype", "bf16")))
    return x.like()


# ================================ 降级 ================================


def lower(machine, timing, out_of):
    """把带核的 ffn 调用降成每个核的 kernel，同时排地址。

    结果发回外围模块，目标写 -1。核阵列里没有哪个核在收它，收的是阵列外面那个模块。
    """
    exit_core = -1
    per_core = {}
    for call in CALLS:
        per_core.setdefault(call["core"], []).append(call)

    kernels, feeds, drains = [], [], []
    for cid, calls in sorted(per_core.items()):
        ops, addr = [], 0
        # 权重常驻，排在最前。同名的一段权重只占一次。
        weights = {}
        for call in calls:
            hid, inner, wd = call["x"].shape[-1], call["inner"], call["wd"]
            key = (call["name"], hid, inner, wd)
            if key in weights:
                continue
            size = int(hid * inner * BITS[wd])
            weights[key] = (addr, addr + size, addr + 2 * size)
            addr += 3 * size
        # 激活区接在权重后面，两层交替用一块。一层的输入与中间结果在下一层还被读着
        # （这一层的结果正推出去时下一层的输入已经在收），所以不能只留一块；隔了两层
        # 的那一层早就做完了，两块够用。
        act_base = addr
        span = 0
        for call in calls:
            x = call["x"]
            if x.shape[0] == 0:
                continue
            act = Tensor((x.shape[0], call["inner"]), x.dtype)
            span = max(span, 2 * x.bytes + 3 * act.bytes)
        addr = act_base + 2 * span

        for turn, call in enumerate(calls):
            x = call["x"]
            hid, inner, wd = x.shape[-1], call["inner"], call["wd"]
            tok = x.shape[0]
            if tok == 0:
                continue
            g, u, d = weights[(call["name"], hid, inner, wd)]
            act = Tensor((tok, inner), x.dtype)
            base = act_base + (turn % 2) * span
            ax = base
            ag = ax + x.bytes
            au = ag + act.bytes
            aa = au + act.bytes
            ay = aa + act.bytes
            ops += [
                Recv(slave=0, dst=ax, length=x.bytes),
                Gemm(m=tok, k=hid, n=inner, a=ax, b=g, c=ag, dtype=wd,
                     cycles=timing.gemm(tok, hid, inner, wd)),
                Gemm(m=tok, k=hid, n=inner, a=ax, b=u, c=au, dtype=wd,
                     cycles=timing.gemm(tok, hid, inner, wd)),
                Elemwise("swiglu", n=tok * inner, srcs=[ag, au], dst=aa,
                         limit=call["limit"],
                         cycles=timing.elemwise("swiglu", tok * inner)),
                Gemm(m=tok, k=inner, n=hid, a=aa, b=d, c=ay, dtype=wd,
                     cycles=timing.gemm(tok, inner, hid, wd)),
                Send(src=ay, length=x.bytes, dst_core=exit_core, master=0),
            ]
            feeds.append(Feed(cycle=0, core=cid, slave=0, length=x.bytes))
            drains.append(Drain(core=cid, master=0, length=x.bytes))
        if ops:
            kernels.append(CoreKernel(core=cid, ops=ops))
    return kernels, feeds, drains


def schedule(kernels, feeds, off_core_cycles, link_bw):
    """给时序表排拍。

    一层在核上要花的时间是收进来、算、再发出去三段相加。各核分到的 token 数不同，长短
    不一，一层取最长的那个，因为下一层的输入要等最慢的那个核交回来。外围模块自己那一段
    用一个占位数，等它的时间有数了再换。
    """
    spans = {}
    for ck in kernels:
        seq, cur, started = [], 0, False
        for op in ck.ops:
            t = type(op).__name__
            if t == "Recv":
                if started:
                    seq.append(cur)
                cur, started = (op.length + link_bw - 1) // link_bw, True
            elif t == "Send":
                cur += (op.length + link_bw - 1) // link_bw
            else:
                cur += op.cycles
        if started:
            seq.append(cur)
        spans[ck.core] = seq

    nlayer = max((len(v) for v in spans.values()), default=0)
    longest = [max((v[i] for v in spans.values() if i < len(v)), default=0)
               for i in range(nlayer)]

    per_core = {}
    for f in feeds:
        per_core.setdefault(f.core, []).append(f)
    cycle = 0
    for i in range(nlayer):
        for v in per_core.values():
            if i < len(v):
                v[i].cycle = cycle
        cycle += longest[i] + off_core_cycles
    return feeds


def compile_to(machine, model, timing, into, out_of, inflight, seed,
               off_core_cycles=8000, link_bw=32):
    SEED[0] = seed
    CALLS.clear()
    ROUTE_CACHE.clear()
    model(Tensor((DEF.get("seq", 4096),), "int32"))
    kernels, feeds, drains = lower(machine, timing, out_of)
    feeds = schedule(kernels, feeds, off_core_cycles, link_bw)
    return kernels, feeds, drains


# ================================ 落盘 ================================
# 一行一条记录，空格分隔。地址十六进制带前缀，其余十进制。
#
#   CORE  <core_id>                                     以下是这个核的 kernel
#   LAYER <i>                                           往下是第 i 层，调试用
#   GEMM  <m> <k> <n> <a> <b> <c> <dtype> <cycles>
#   ELEM  <op> <n> <dst> <limit> <cycles> <src>...      源地址变长，放最后
#   SEND  <src> <length> <dst_core> <port>
#   RECV  <port> <dst> <length>
#   FEED  <cycle> <core> <port> <length>                外围模块什么时候喂进来
#   DRAIN <core> <port> <length>                        外围模块在哪个口收
#   META  <key> <value，吃到行尾>


def fmt_op(op):
    t = type(op).__name__
    if t == "Gemm":
        return (f"GEMM {op.m} {op.k} {op.n} {op.a:#x} {op.b:#x} {op.c:#x} "
                f"{op.dtype} {op.cycles}")
    if t == "Elemwise":
        srcs = " ".join(f"{s:#x}" for s in op.srcs)
        return f"ELEM {op.op} {op.n} {op.dst:#x} {op.limit} {op.cycles} {srcs}"
    if t == "Send":
        return f"SEND {op.src:#x} {op.length} {op.dst_core} {op.master}"
    if t == "Recv":
        return f"RECV {op.slave} {op.dst:#x} {op.length}"
    raise ValueError(t)


def write_kernels(path, kernels, feeds, drains, meta=None):
    with open(path, "w") as f:
        f.write("BACHK 1\n")
        for k, v in (meta or {}).items():
            f.write(f"META {k} {v}\n")
        for ck in kernels:
            f.write(f"\nCORE {ck.core}\n")
            layer = -1
            for op in ck.ops:
                if type(op).__name__ == "Recv":
                    layer += 1
                    f.write(f"LAYER {layer}\n")
                f.write(fmt_op(op) + "\n")
        f.write("\n")
        for x in feeds:
            f.write(f"FEED {x.cycle} {x.core} {x.slave} {x.length}\n")
        for x in drains:
            f.write(f"DRAIN {x.core} {x.master} {x.length}\n")


def read_kernels(path):
    """读回来自检用。返回 (每核的指令条数, feed 数, drain 数)。"""
    per_core, feeds, drains, cur = {}, 0, 0, None
    for ln in open(path):
        f = ln.split()
        if not f:
            continue
        if f[0] == "CORE":
            cur = int(f[1])
            per_core[cur] = 0
        elif f[0] in ("GEMM", "ELEM", "SEND", "RECV"):
            per_core[cur] += 1
        elif f[0] == "FEED":
            feeds += 1
        elif f[0] == "DRAIN":
            drains += 1
        elif f[0] in ("BACHK", "META", "LAYER"):
            pass
        else:
            raise ValueError("认不出的记录 " + f[0])
    return per_core, feeds, drains
