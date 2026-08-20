"""DeepSeek-V4 排在 512 个核上。跑这个文件就是编译。

产出是每个硬件单元实际执行的 kernel，写成一份 .bachk，模拟器读它跑：

    python3 src/bach/compiler/deepseek.py [--out PATH] [--layers N] [--mtp N]
                                          [--seq N] [--cores N] [--seed N]

不给参数就是 61 层、一批 4096 个 token、384 个专家一核一个。MTP 那一段默认不编，
要它给 --mtp 1：它挂在主干旁边多猜一个 token，内部也是一整层，照样给每个核产六条
指令。截几层、少喂几个 token 只改这几个数，模型描述本身不动。

三份输入：机器描述、模型描述（本文件）、实测时间（TIMING）。

算子对编译器是黑盒，回答两件事：有哪些轴、各多长、输出什么形状；每个轴切开之后各份
什么关系。并行轴各份互不相干，出口拼接；归约轴各份是部分结果，出口相加。

核只做 FFN。只有 FFN 那一段排到核上、产出 kernel，模型的其余部分给出它的输入形状和
它在模型里的位置，不产 kernel。切成几份由给了几个核决定。

张量只有形状，切片长度可以是变量，变量在编译时按种子求值。

搬运三档，同一份描述里可混用：不写，编译器补；写 concat / reduce_sum，拓扑用现成的；
写 send / wait，逐跳自己定。

权重 fp8，专家权重 fp4，激活 bf16，字节数按各自的宽度算。
"""

import argparse
import json
import pathlib
from types import SimpleNamespace

from compile import (Machine, Timing, defaults, embedding, linear, attn,
                     index_topk, rope, norm, add, ffn, topk, concat,
                     reduce_sum, send, wait, tokens_of, compile_to,
                     write_kernels)

C = json.loads((pathlib.Path(__file__).parent / "deepseek_v4_config.json").read_text(),
               object_hook=lambda d: SimpleNamespace(**d))

SEQ = 4096        # 一份输入的 token 数
NOPE = C.head_dim - C.qk_rope_head_dim

defaults(seq=SEQ, norm_eps=C.rms_norm_eps, act=C.hidden_act, dtype=C.torch_dtype,
         weight_dtype=C.quantization_config.quant_method,
         block=C.quantization_config.weight_block_size,
         rope_theta=C.rope_theta, rope_scaling=C.rope_scaling)

ROUTE = dict(score=C.scoring_func, method=C.topk_method, norm_prob=C.norm_topk_prob,
             scale=C.routed_scaling_factor)

# ================================ 机器 ================================

M = Machine(
    chips=(16, 4), cores_per_chip=(2, 4),
    inputs={f"in{i}": dict(entry=i * 32, link="PCIE_WEST", bandwidth=32,
                           latency=400) for i in range(16)},
    outputs={f"out{i}": dict(exit=i * 32 + 23, link="PCIE_EAST", bandwidth=32,
                             latency=200) for i in range(16)},
    credit=2,          # 每个下游账户的额度
)

TIMING = Timing.load("deepseek_timing.json")

# ============================== 在核上 ==============================
# 384 个专家铺在前 384 个核上，每核 1 个。整个文件只有这一段产 kernel。

ALL = M.cores()
FFN_CORES = ALL[:C.n_routed_experts]


def moe(x, cores, sel):
    """routed 专家分给 cores，每核 n_routed_experts // len(cores) 个，权重 fp4。

    每个核分到多少 token 按 sel 定，编译期求值。一个核上降下来是：Recv、两条 Gemm
    出 gate 与 up、一条 swiglu、一条 Gemm 出结果、Send。
    """
    per = C.n_routed_experts // len(cores)
    parts = []
    for i, c in enumerate(cores):
        n = tokens_of(sel, experts=range(i * per, (i + 1) * per))
        with c:
            parts.append(ffn(x.token[:n], "expert", inner=C.moe_intermediate_size,
                             weight_dtype=C.expert_dtype, limit=C.swiglu_limit))
    return reduce_sum(parts)


# ============================== 不在核上 ==============================
# 下面这些不产 kernel。


def embed(tok):
    return embedding(tok, vocab=C.vocab_size, dim=C.hidden_size)


def indexer(x):
    """DSA 索引器。"""
    q = rope(linear(x, "idx_q", out=C.index_n_heads * C.index_head_dim))
    k = rope(linear(x, "idx_k", out=C.index_n_heads * C.index_head_dim))
    return index_topk(q, k, heads=C.index_n_heads, topk=C.index_topk)


def attention(x, idx, ratio):
    """MLA + MQA。q 压到 q_lora_rank 再展开，kv 只有一个 head 且按 ratio 压。

    o_proj 分 o_groups 组、经 o_lora_rank 降维。压缩那条路用 compress_rope_theta。
    """
    h = norm(x, "input_layernorm")
    q = norm(linear(h, "q_a_proj", out=C.q_lora_rank), "q_a_layernorm")
    q = rope(linear(q, "q_b_proj", out=C.num_attention_heads * C.head_dim),
             part=C.qk_rope_head_dim)
    kv = linear(h, "kv_proj", out=C.num_key_value_heads * C.head_dim)
    kv = rope(norm(kv, "kv_layernorm"), part=C.qk_rope_head_dim,
              theta=C.compress_rope_theta, compress=ratio)
    a = attn(q, kv, heads=C.num_attention_heads, kv_heads=C.num_key_value_heads,
             qk_dim=C.head_dim, nope_dim=NOPE, v_dim=C.head_dim,
             topk=idx, window=C.sliding_window)
    o = linear(a, "o_a_proj", out=C.o_lora_rank // C.o_groups, groups=C.o_groups)
    return linear(o, "o_b_proj", out=C.hidden_size)


def router(x, hashed):
    """挑出每个 token 走哪 num_experts_per_tok 个专家，结果交给 moe。

    hashed 为真的层用 hash 分簇加 Sinkhorn 均衡代替打分。这一段的结构还没定，
    hc_mult / hc_sinkhorn_iters / hc_eps 三个参数带着，怎么用未定。
    """
    if hashed:
        return topk(linear(x, "hash_proj", out=C.n_routed_experts),
                    k=C.num_experts_per_tok, method="hash_sinkhorn",
                    mult=C.hc_mult, iters=C.hc_sinkhorn_iters, eps=C.hc_eps)
    return topk(linear(x, "gate", out=C.n_routed_experts),
                k=C.num_experts_per_tok, **ROUTE)


def dspark(x):
    """dspark_target_layer_ids 那几层额外做的一段。结构还没定。

    带着的参数：dspark_block_size、dspark_markov_rank、dspark_noise_token_id。
    """
    return x


def shared_expert(x):
    """n_shared_experts 个共享专家，每个 token 都过。"""
    return ffn(x, "shared_expert", limit=C.swiglu_limit,
               inner=C.moe_intermediate_size * C.n_shared_experts)


def lm_head(x):
    return linear(norm(x, "final_norm"), "lm_head", out=C.vocab_size)


# ============================== 串起来 ==============================


def layer(x, i, cores, idx):
    """一层：attention 一段，MoE 一段，各带一次残差。只有 MoE 落在核上。"""
    idx = indexer(norm(x, "idx_norm"))
    x = add(x, attention(x, idx, C.compress_ratios[i]))
    h = norm(x, "post_attention_layernorm")
    sel = router(h, hashed=i < C.num_hash_layers)
    x = add(x, add(moe(h, cores, sel), shared_expert(h)))
    if i in C.dspark_target_layer_ids:
        x = dspark(x)
    return x, idx


def mtp(x, tok, idx):
    """num_nextn_predict_layers 那一层。"""
    h = concat([norm(x, "enorm"), norm(embed(tok), "hnorm")], "hidden")
    h = linear(h, "eh_proj", out=C.hidden_size)
    h, _ = layer(h, C.num_hidden_layers - 1, FFN_CORES, idx)
    return lm_head(h)


def model(tok):
    """embedding，61 层，lm_head，再加 num_nextn_predict_layers 层 MTP。"""
    x = embed(tok)
    idx = None
    for i in range(C.num_hidden_layers):
        x, idx = layer(x, i, FFN_CORES, idx)
    out = [lm_head(x)]
    for _ in range(C.num_nextn_predict_layers):
        out.append(mtp(x, tok, idx))
    return concat(out, "token")


def main():
    """编译一次，把 kernel 与时序表写成一份 .bachk。"""
    ap = argparse.ArgumentParser(description="编译 DeepSeek-V4，产出每个核的 kernel")
    ap.add_argument("--out", default="build/playback/deepseek.bachk")
    ap.add_argument("--layers", type=int, default=C.num_hidden_layers)
    # MTP 那一段默认不编，见文件开头。config 里是 1，要它就给 --mtp 1。
    ap.add_argument("--mtp", type=int, default=0,
                    help="MTP 那一段编几层，默认 0 不编")
    ap.add_argument("--seq", type=int, default=SEQ)
    ap.add_argument("--cores", type=int, default=C.n_routed_experts,
                    help="专家铺在前几个核上，默认一核一个")
    ap.add_argument("--seed", type=int, default=0)
    a = ap.parse_args()

    global FFN_CORES
    full_layers = C.num_hidden_layers + C.num_nextn_predict_layers
    C.num_hidden_layers = a.layers
    C.num_nextn_predict_layers = a.mtp
    FFN_CORES = ALL[:a.cores]
    defaults(seq=a.seq)

    kernels, feeds, drains = compile_to(
        machine=M, model=model, timing=TIMING,
        into="in0", out_of="out0", inflight=16, seed=a.seed)

    # 落在核上的是哪几层，直接数 kernel 里的 Recv：一层一条。命令行给的层数不等于
    # 它，模型里有不产 kernel 的层。
    kernel_layers = max((sum(1 for op in k.ops if type(op).__name__ == "Recv")
                         for k in kernels), default=0)
    out = pathlib.Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    write_kernels(out, kernels, feeds, drains, meta={
        "model": "deepseek_v4",
        "model_name": "DeepSeek-V4",
        "model_layers": str(full_layers),
        "layers": str(kernel_layers),
        "experts_per_token": str(C.num_experts_per_tok),
        "seq": str(a.seq),
        "experts": str(C.n_routed_experts),
        "expert_cores": str(len(FFN_CORES)),
        "seed": str(a.seed),
    })
    print(f"{len(kernels)} 个核，{sum(len(k.ops) for k in kernels)} 条指令，"
          f"{len(feeds)} 条 FEED，{len(drains)} 条 DRAIN → {out}")


if __name__ == "__main__":
    main()
