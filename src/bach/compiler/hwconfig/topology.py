"""读一份拓扑描述，展开成一套硬件配置。

描述给的是参数：阵列摆几颗 chip、每颗什么形状、广播从哪进来往哪几个口出、部分
和按什么次序归约。这一层按 `layer` 分派给对应的展开器，交出来的 Plan 里是逐
core 逐 path 的路由表与逐 core 的任务链，直接就是硬件要的形式。
"""

import json
from pathlib import Path

from . import moe

LAYERS = {"moe": moe.build_plan, "moe_lpu": moe.build_lpu_plan}


def load(path, kmap=None):
    """读一份拓扑描述，展开成 Plan。符号表给了就把每笔 task 的入口地址填上。"""
    desc = json.loads(Path(path).read_text(encoding="utf-8"))
    build = LAYERS.get(desc.get("layer"))
    if build is None:
        raise ValueError(f"{path} 的 layer 只认 {sorted(LAYERS)}")
    plan = build(desc)
    plan.name = desc.get("name", plan.name)
    fill_pc(plan, kmap)
    return plan


def fill_pc(plan, kmap):
    """把任务链与 datain 的入口地址从符号表填进去。

    符号表是 kernel 编出来的，没编过就都留 0。地址一变就得重新编一次配置，两者
    因此同在 bundle 里。
    """
    for items in plan.chains.values():
        for it in items:
            it.task_pc = symbol(kmap, it.sym)
    for key, sym in plan.datain.items():
        plan.datain_pc[key] = symbol(kmap, sym)


def symbol(kmap, sym):
    if kmap is None or not kmap.loaded:
        return 0
    return kmap.symbols.get(sym, 0)
