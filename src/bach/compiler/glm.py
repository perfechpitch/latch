"""GLM-5.0 排在 512 个核上。跑这个文件就是编译。

产出跟 DeepSeek-V4 那份一样，是每个硬件单元实际执行的 kernel，写成一份 .bachk：

    python3 src/bach/compiler/glm.py [--out PATH] [--layers N] [--mtp N]
                                     [--seq N] [--cores N] [--seed N]

不给参数就是整个模型：92 层，前 3 层是 dense，其余是 MoE，一批 4096 个 token，
160 个专家一核一个。MTP 那一段默认不编，要它给 --mtp 1。

跟 DeepSeek-V4 那份的差别只在模型这一侧，机器描述、编译器、模拟器都是同一套：

    attention   GQA，96 个 q head 配 8 个 kv head，q 与 k 各过一次 norm，
                rope 只转前一半维度。DeepSeek 那边是 MLA，q 压到 q_lora_rank 再展开
    前几层      first_k_dense_replace 层不走 MoE，走一个整块的 FFN
    MoE         160 个专家，每个 token 走 8 个；DeepSeek 是 384 个走 6 个
    专家大小    moe_intermediate_size 1536，比 DeepSeek 的 3072 小一半

只有 MoE 那一段排到核上、产出 kernel。attention、router、shared expert、前几层的
dense FFN 与 lm_head 都在外围模块里，只给出形状和它在模型里的位置，不产 kernel——
这跟 DeepSeek 那份的划分一致，核只做 routed expert 的 FFN。

权重 fp8，专家权重 fp4，激活 bf16，字节数按各自的宽度算。

config 里的数是照 GLM-4.5 / GLM-4.6 的公开结构填的占位，GLM-5.0 的官方 config 出来
之后换掉 glm_5_config.json 即可，本文件不用动。
"""

import argparse
import json
import pathlib
from types import SimpleNamespace

from compile import (Machine, Timing, defaults, embedding, linear, attn, rope,
                     norm, add, ffn, topk, concat, reduce_sum, tokens_of,
                     compile_to, write_kernels)

C = json.loads((pathlib.Path(__file__).parent / "glm_5_config.json").read_text(),
               object_hook=lambda d: SimpleNamespace(**d))

SEQ = 4096        # 一份输入的 token 数
ROPE_DIM = int(C.head_dim * C.partial_rotary_factor)

defaults(seq=SEQ, norm_eps=C.rms_norm_eps, act=C.hidden_act, dtype=C.torch_dtype,
         weight_dtype=C.quantization_config.quant_method,
         block=C.quantization_config.weight_block_size, rope_theta=C.rope_theta)

ROUTE = dict(score=C.scoring_func, method=C.topk_method, norm_prob=C.norm_topk_prob,
             scale=C.routed_scaling_factor, groups=C.n_group, topk_group=C.topk_group)

# ================================ 机器 ================================
# 跟 DeepSeek-V4 那份同一台机器：16 行 4 列 chip，每 chip 2 行 4 列核。

M = Machine(
    chips=(16, 4), cores_per_chip=(2, 4),
    inputs={f"in{i}": dict(entry=i * 32, link="PCIE_WEST", bandwidth=32,
                           latency=400) for i in range(16)},
    outputs={f"out{i}": dict(exit=i * 32 + 23, link="PCIE_EAST", bandwidth=32,
                             latency=200) for i in range(16)},
    credit=2,
)

TIMING = Timing.load("glm_timing.json")

# ============================== 在核上 ==============================
# 160 个专家铺在前 160 个核上，每核 1 个。整个文件只有这一段产 kernel。

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


def embed(tok):
    return embedding(tok, vocab=C.vocab_size, dim=C.hidden_size)


def attention(x):
    """GQA。q 与 k 各过一次 norm，rope 只转前 partial_rotary_factor 那部分维度。"""
    h = norm(x, "input_layernorm")
    q = rope(norm(linear(h, "q_proj", out=C.num_attention_heads * C.head_dim,
                         bias=C.attention_bias), "q_norm"), part=ROPE_DIM)
    k = rope(norm(linear(h, "k_proj", out=C.num_key_value_heads * C.head_dim,
                         bias=C.attention_bias), "k_norm"), part=ROPE_DIM)
    v = linear(h, "v_proj", out=C.num_key_value_heads * C.head_dim,
               bias=C.attention_bias)
    a = attn(q, concat([k, v], "hidden"), heads=C.num_attention_heads,
             kv_heads=C.num_key_value_heads, qk_dim=C.head_dim, v_dim=C.head_dim)
    return linear(a, "o_proj", out=C.hidden_size)


def router(x):
    """挑出每个 token 走哪 num_experts_per_tok 个专家，结果交给 moe。"""
    return topk(linear(x, "gate", out=C.n_routed_experts),
                k=C.num_experts_per_tok, **ROUTE)


def dense_mlp(x):
    """前 first_k_dense_replace 层不走 MoE，走一个整块的 FFN。"""
    return ffn(x, "mlp", inner=C.intermediate_size, limit=C.swiglu_limit)


def shared_expert(x):
    """n_shared_experts 个共享专家，每个 token 都过。"""
    return ffn(x, "shared_expert", limit=C.swiglu_limit,
               inner=C.moe_intermediate_size * C.n_shared_experts)


def lm_head(x):
    return linear(norm(x, "final_norm"), "lm_head", out=C.vocab_size)


# ============================== 串起来 ==============================


def layer(x, i, cores):
    """一层：attention 一段，FFN 一段，各带一次残差。只有 MoE 那一段落在核上。"""
    x = add(x, attention(x))
    h = norm(x, "post_attention_layernorm")
    if i < C.first_k_dense_replace:
        return add(x, dense_mlp(h))
    return add(x, add(moe(h, cores, router(h)), shared_expert(h)))


def mtp(x, tok):
    """num_nextn_predict_layers 那一层。挂在主干旁边多猜一个 token，内部也是一整层。"""
    h = concat([norm(x, "enorm"), norm(embed(tok), "hnorm")], "hidden")
    h = linear(h, "eh_proj", out=C.hidden_size)
    return lm_head(layer(h, C.num_hidden_layers - 1, FFN_CORES))


def model(tok):
    """embedding，92 层，lm_head，再加 num_nextn_predict_layers 层 MTP。"""
    x = embed(tok)
    for i in range(C.num_hidden_layers):
        x = layer(x, i, FFN_CORES)
    out = [lm_head(x)]
    for _ in range(C.num_nextn_predict_layers):
        out.append(mtp(x, tok))
    return concat(out, "token")


def main():
    """编译一次，把 kernel 与时序表写成一份 .bachk。"""
    ap = argparse.ArgumentParser(description="编译 GLM-5.0，产出每个核的 kernel")
    # 跟 DeepSeek 那份分开放，两个模型的产物不互相盖
    ap.add_argument("--out", default="build/glm/glm.bachk")
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
    full_layers = (C.num_hidden_layers - C.first_k_dense_replace
                   + C.num_nextn_predict_layers)
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
        "model": "glm_5",
        "model_name": "GLM-5.0",
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
    # 前几层是 dense，不产 kernel。层数截得比它还少就什么都编不出来，这里说一声，
    # 免得拿着一份空的 .bachk 去查模拟器。
    if not kernels:
        print(f"编出来是空的：前 {C.first_k_dense_replace} 层是 dense，不走 MoE，"
              f"也就不落在核上。--layers 要大于 {C.first_k_dense_replace} 才有 kernel，"
              f"想要 N 层 MoE 就给 --layers {C.first_k_dense_replace} + N。")


if __name__ == "__main__":
    main()
