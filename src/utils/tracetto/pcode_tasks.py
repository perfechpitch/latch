"""Associate an exported pcode task chain with the tasks recorded in a trace.

python src/utils/tracetto/pcode_tasks.py TRACE --html REPORT --bundle BUNDLE_DIR
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from tracetto import index, spans as S
from tracetto.reader import TraceReader


def build(trace: Path, html: Path, bundle: Path, report_url: str = "") -> dict:
    text = html.read_text(encoding="utf-8")

    def data(name):
        match = re.search(r'<script type="application/json" id="' + re.escape(name)
                          + r'">(.*?)</script>', text, re.S)
        if not match:
            raise ValueError(f"Report missing {name}")
        return json.loads(match.group(1))

    mapping, topology = data("map-data"), data("topology-data")
    cases, calls = data("chip-cases"), data("pcode-line-text")
    ir = bundle / "pcode_ep6_tp8.bachir"
    digest = hashlib.sha256()
    for file in sorted([ir, *bundle.glob("*.hex")]):
        digest.update(file.name.encode() + b"\0" + file.read_bytes())
    receipt = trace.with_suffix(".report.json")
    if receipt.exists():
        recorded = json.loads(receipt.read_text()).get("bundle_sha256")
        if recorded and recorded != digest.hexdigest():
            raise ValueError("Exported bundle does not match the recorded simulation")
    harvest, chains, self_start = {}, {}, set()
    for line in ir.read_text().splitlines():
        f = line.split()
        if not f:
            continue
        if f[0] == "CHIP":
            chip, mask = int(f[1]), int(f[2], 0)
            harvest[chip] = min(c["h"] for c in cases[str(chip)] if c["mask"] == mask)
        elif f[0] == "TCHAIN":
            chains.setdefault((int(f[1]), int(f[2])), []).append((int(f[3]), int(f[5])))
        elif f[0] == "CFGMISC" and int(f[4]):
            self_start.add((int(f[1]), int(f[2])))
    result = dict(format=1, source=index.source_identity(trace), name=trace.stem,
                  bundle_sha256=digest.hexdigest(), report_url=report_url, cores={})
    mapped_events = 0
    with TraceReader(str(trace.with_suffix(""))) as reader:
        chips, signals = S.core_paths(reader)
        end = S.trace_t_end(reader, signals)
        for chip, core_ids in sorted(chips.items()):
            h = harvest[chip]
            for core in sorted(core_ids):
                key = f"{chip}.{core}"
                cell = mapping["chips"][f"c{chip}"][f"h{h}"].get(f"p{core}", {})
                tasks = [dict(mapping["tp"][i]) for i in cell.get("tc", [])]
                regular = sorted((t for t in tasks if t["task_id"] >= 0), key=lambda t: t["task_id"])
                datain = next((t for t in tasks if t["task_id"] == -1), None)
                renumber = ((chip, core) in self_start and datain is None and regular
                            and len(chains.get((chip, core), [])) == len(regular) - 1)
                if renumber:
                    datain, regular = regular[0], regular[1:]
                lookup = {}
                for i, task in enumerate(regular):
                    task["native_task_id"] = i if renumber else task["task_id"]
                    lookup[(task["unit"], task["native_task_id"])] = task
                if datain:
                    datain["native_task_id"] = 63
                    datain["datain"] = True
                    lookup[(datain["unit"], 63)] = datain
                expected = {(t["native_task_id"], S.UNITS.index(t["unit"])) for t in regular}
                if expected != set(chains.get((chip, core), [])):
                    raise ValueError(f"HTML and exported task chain differ on {key}")
                for task in tasks:
                    task["pcode_task_id"] = task.pop("task_id")
                    task["pcode_call"] = calls.get(str(task["pcode_line"]), "")
                    task["timings"] = {}
                sp = S.core_spans(reader, signals[key], end) if key in signals else None
                if sp:
                    for row, parts in S.ROWS:
                        for lane, _color in parts:
                            _label, group, unit_idx = S.LANES[lane]
                            for segment in sp[group][unit_idx]:
                                task = lookup.get((S.UNITS[unit_idx], segment[3]))
                                if task is None:
                                    raise ValueError(f"Unmapped recorded task on {key}: {segment}")
                                task["timings"].setdefault(row, []).append(segment)
                                if group == "core":
                                    mapped_events += 1
                role = next(c for c in topology["chips"][f"c{chip}"][f"h{h}"] if c["phys"] == core)
                result["cores"][key] = dict(chip=chip, core=core, harvest=h,
                                             logical=role.get("logical"), tasks=tasks)
                reader.cache.clear()
                reader._segs.clear()
    result["mapped_rv_events"] = mapped_events
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--html", type=Path, required=True)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--report-url", default="")
    args = parser.parse_args()
    trace = args.trace.resolve()
    result = build(trace, args.html.resolve(), args.bundle.resolve(), args.report_url)
    output = trace.with_suffix(".tasks.json")
    output.write_text(json.dumps(result, ensure_ascii=False, separators=(",", ":")) + "\n")
    print(f"{output}: {len(result['cores'])} cores, {result['mapped_rv_events']} mapped RV events")


if __name__ == "__main__":
    main()
