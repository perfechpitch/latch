"""下载 GLM-5 第 3 层 MXFP8 权重，并展开成 latch 测试台能灌进 Matrix Mem 的文件。

``python -m bachflow_importer`` 按 EP1 场景里的 ``hf_name`` 拉取
``mlx-community/GLM-5-8bit-MXFP8``，切到每个逻辑核的 ``mmem`` / ``mmem_sf``。
本脚本再按 ``latch_export.logical_core_of`` 把逻辑核映到物理核，并把 §3.3.1
的 scale 槽展开成「每 32 字节数据一个 E8M0」。产物：

    <out>/weights/L00003/          importer 的原始切分
    <out>/latch/index.txt          chip phys offset data scale
    <out>/latch/c{chip}p{phys}.dat
    <out>/latch/c{chip}p{phys}.sf
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

LAYER = 3
CHECKPOINT = "mlx-community/GLM-5-8bit-MXFP8"
REVISION = "main"


def _bachflow_root() -> Path:
    env = os.environ.get("BACHFLOW_ROOT")
    if env:
        return Path(env)
    here = Path(__file__).resolve()
    for parent in here.parents:
        if (parent / "BachFlow" / "importer").is_dir():
            return parent / "BachFlow"
    return Path("/mnt/c/Users/kim01/Desktop/BachFlow")


def _import_layer(out: Path, bach: Path) -> Path:
    manifest = out / "weights" / f"L{LAYER:05d}" / "weights.manifest.json"
    if manifest.is_file():
        print(f"reuse {manifest}", flush=True)
        return manifest
    env = os.environ.copy()
    env["PYTHONPATH"] = os.pathsep.join(
        [
            str(bach / "importer" / "src"),
            str(bach / "pcode" / "src"),
            str(bach / "defs"),
            str(bach / "route" / "src"),
            env.get("PYTHONPATH", ""),
        ]
    )
    cmd = [
        sys.executable,
        "-u",
        "-m",
        "bachflow_importer",
        "--scenario",
        "glm5_ep1_pp3_tp16",
        "--out",
        str(out),
        "--checkpoint",
        CHECKPOINT,
        "--layers",
        str(LAYER),
        "--revision",
        REVISION,
    ]
    print(" ".join(cmd), flush=True)
    last = None
    for attempt in range(1, 8):
        try:
            subprocess.run(cmd, cwd=bach, env=env, check=True)
            last = None
            break
        except subprocess.CalledProcessError as exc:
            last = exc
            print(f"importer attempt {attempt} failed ({exc.returncode}); resume", flush=True)
    if last is not None:
        raise last
    if not manifest.is_file():
        raise SystemExit(f"importer finished without {manifest}")
    return manifest


def _phys_of(chip: int, logical: int) -> int:
    from bachflow_pcode.latch_export import logical_core_of

    for phys in range(10):
        if logical_core_of(chip, phys) == logical:
            return phys
    raise SystemExit(f"chip {chip} logical {logical} has no physical core")


def _linear_scale(data_off: int, data: bytes, sf_off: int, sf: bytes) -> bytes:
    from bachflow.mmem_layout import BLOCK, sf_offset

    groups = len(data) // 32
    out = bytearray(groups)
    for i in range(0, groups * 32, BLOCK):
        at = sf_offset(data_off + i) - sf_off
        if at < 0 or at + 4 > len(sf):
            raise SystemExit(
                f"scale slot {at} outside sf file of {len(sf)} B (data {data_off:#x}+{i})"
            )
        out[i // 32 : i // 32 + 4] = sf[at : at + 4]
    return bytes(out)


def _pack_latch(out: Path, manifest_path: Path, bach: Path) -> None:
    sys.path[:0] = [
        str(bach / "pcode" / "src"),
        str(bach / "defs"),
    ]
    manifest = json.loads(manifest_path.read_text())
    src = manifest_path.parent
    latch = out / "latch"
    latch.mkdir(parents=True, exist_ok=True)
    lines = [
        f"# {manifest['checkpoint']} revision {manifest['revision']} layer {manifest['layer']}",
    ]
    for key in sorted(manifest["cores"], key=lambda s: (int(s.split(".")[0][1:]), int(s.split(".")[1][1:]))):
        core = manifest["cores"][key]
        chip, logical = core["c"], core["l"]
        phys = _phys_of(chip, logical)
        mm = core["mmem"]
        data = (src / mm["file"]).read_bytes()
        if len(data) != mm["size"]:
            raise SystemExit(f"{mm['file']}: {len(data)} bytes, manifest says {mm['size']}")
        sf_meta = core.get("mmem_sf")
        if sf_meta is None:
            scale = bytes(len(data) // 32)
        else:
            sf = (src / sf_meta["file"]).read_bytes()
            if len(sf) != sf_meta["size"]:
                raise SystemExit(f"{sf_meta['file']}: {len(sf)} bytes, manifest says {sf_meta['size']}")
            scale = _linear_scale(mm["offset"], data, sf_meta["offset"], sf)
        data_name = f"c{chip}p{phys}.dat"
        scale_name = f"c{chip}p{phys}.sf"
        (latch / data_name).write_bytes(data)
        (latch / scale_name).write_bytes(scale)
        lines.append(f"{chip} {phys} {mm['offset']} {data_name} {scale_name}")
        print(f"  chip {chip} logical {logical} -> p{phys} off {mm['offset']:#x} {len(data)} B", flush=True)
    (latch / "index.txt").write_text("\n".join(lines) + "\n")
    print(f"wrote {latch / 'index.txt'} ({len(lines) - 1} cores)", flush=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    bach = _bachflow_root()
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = _import_layer(args.out, bach)
    index = args.out / "latch" / "index.txt"
    if not index.is_file():
        _pack_latch(args.out, manifest, bach)
    else:
        print(f"reuse {index}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
