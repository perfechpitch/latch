#!/usr/bin/env python3
"""在 Bach 上跑一张 Map，把它的完成集合与丢包集合写成一份期望结果。

这个脚本只读 Bach 仓库，不往那边写任何东西：报告、快照与诊断三个输出路径都指到
`--outdir` 底下，Bach 的其余落盘路径全部由这三个派生。

产出的期望结果作为 fixture 提交进来，latch 侧的对表测试读它。这样 Bach 只需要跑一
次：两边跑的是同一张 Map、同一份任务表，比的是同一件事的两个实现。

用法：

    python3 src/bach/tools/run_bach.py \\
        --map /home/colin/develop/bach/bach_topology.json \\
        --users 1 --streams 1 \\
        --out test/bach/fixture/expect/bach_topology.expect
"""

import argparse
import datetime
import hashlib
import os
import sys
from pathlib import Path

FORMAT_VERSION = 1

DEFAULT_BACH = "/home/colin/develop/bach"


def sha256_of_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def eth_switch_options(map_path, phase):
    """Phase 那一段的开关。

    四个开关只放行两种组合，与导出中间文件时那两种一一对应：只判 Phase1 边界，或者
    一路走到 Phase3 汇合。组大小从 Map 推，不另设参数。
    """
    if phase == "off":
        return {}

    import json

    data = json.loads(Path(map_path).read_text(encoding="utf-8"))
    groups = data.get("moe_ep_groups") or []
    strategy = (data.get("moe_profile") or {}).get("strategy", {})
    routed = strategy.get("routed_experts")
    if not groups or not routed:
        raise SystemExit("Phase 那一段要求这张 Map 有 EPGroup 与路由专家数。")
    if int(routed) % len(groups) != 0:
        raise SystemExit(
            f"路由专家数 {routed} 不能被 EPGroup 数 {len(groups)} 整除，组大小定不下来。"
        )

    opts = {
        "enable_eth_switch": True,
        "eth_switch_expert_group_size": int(routed) // len(groups),
    }
    if phase == "boundary":
        opts["eth_switch_complete_phase1_boundary"] = True
        return opts

    opts.update(
        {
            "enable_eth_switch_phase2_ingress": True,
            "enable_eth_switch_phase2_host_injection": True,
            "enable_eth_switch_phase2_output_result_bridge": True,
            "eth_switch_complete_phase1_boundary": False,
        }
    )
    return opts


def main():
    ap = argparse.ArgumentParser(description="在 Bach 上跑一张 Map，产出期望结果")
    ap.add_argument("--map", required=True, help="Map 文件路径")
    ap.add_argument("--out", required=True, help="输出的 .expect 路径")
    ap.add_argument("--bach", default=DEFAULT_BACH, help="Bach 仓库路径")
    ap.add_argument("--outdir", default=None, help="Bach 的报告落在哪，默认放输出旁边")
    ap.add_argument("--users", type=int, default=None)
    ap.add_argument("--streams", type=int, default=None)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--dsa", choices=("off", "five_route"), default=None)
    ap.add_argument("--phase", choices=("off", "boundary", "full"), default="off")
    args = ap.parse_args()

    map_path = str(Path(args.map).resolve())
    bach_dir = str(Path(args.bach).resolve())
    dst = Path(args.out).resolve()
    outdir = Path(args.outdir).resolve() if args.outdir else dst.parent / "bach_runs"
    outdir.mkdir(parents=True, exist_ok=True)

    sys.path.insert(0, bach_dir)
    # Bach 有几处按当前目录取相对路径，切进输出目录，它写不到自己的仓库里
    os.chdir(outdir)

    from simulation_config import SimulationConfig
    from hbu_array_simulator import HBUArraySimulator

    kwargs = dict(
        map_path=map_path,
        report_file=str(outdir / "hardware_report.jsonl"),
        snapshot_file=str(outdir / "simulation_snapshot.jsonl"),
        diagnostics_file=str(outdir / "hardware_diagnostics.jsonl"),
        no_pipeline_print=True,
    )
    if args.users is not None:
        kwargs["users"] = args.users
    if args.streams is not None:
        kwargs["streams"] = args.streams
    if args.seed is not None:
        kwargs["seed"] = args.seed
    if args.dsa is not None:
        kwargs["dte_dsa_mode"] = args.dsa
    kwargs.update(eth_switch_options(map_path, args.phase))

    sim = HBUArraySimulator(SimulationConfig(**kwargs))
    result = sim.run()

    lines = [f"BACHEXPECT {FORMAT_VERSION}", ""]
    lines.append(f"META map_path {map_path}")
    lines.append(f"META map_sha256 {sha256_of_file(map_path)}")
    lines.append(f"META end_time {int(result.end_time_ns)}")
    lines.append(f"META phase {args.phase}")
    lines.append(
        "META generated_at "
        + datetime.datetime.now().replace(microsecond=0).isoformat()
    )
    lines.append("META generator src/bach/tools/run_bach.py")
    lines.append("")
    lines.append(f"USERS {result.expected_users}")
    lines.append(
        ("COMPLETED " + " ".join(str(u) for u in result.completed_uids)).rstrip()
    )
    lines.append(("LOST " + " ".join(str(u) for u in result.lost_uids)).rstrip())

    # 每个 user 的端到端延迟，也就是它完成的时刻减去起跑的时刻。比它而不比完成时刻：
    # 完成时刻里含着注入源按推包间隔排的那一段，第几个 user 就多几个间隔，那一段与
    # 模型跑得对不对无关。
    for uid, latency in result.metrics.get("global_latency", ()):
        lines.append(f"DONE {uid} {int(latency)}")

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"写出 {dst}")
    print(
        f"  结束时刻 {int(result.end_time_ns)} ns，完成 {len(result.completed_uids)} 个，"
        f"丢 {len(result.lost_uids)} 个"
    )


if __name__ == "__main__":
    main()
