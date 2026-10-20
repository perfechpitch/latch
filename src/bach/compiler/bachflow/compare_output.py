"""Compare a completed EP1 capture against the independent checkpoint golden.

The capture must name the requested users exactly once, each with a unique
(SID, allocation) pair, 6144 finite BF16 elements, and terminal-task completion.
Transport-only logs are not accepted. Thresholds stay the reference module's
own constants; this gate does not retune them.

``--all-users`` takes the users from the run's own ``user`` lines, in driver
order. Users 0 and 1 (101, 202) are compared with their golden; user k >= 2
sent the same token as user k % 2, so its output must equal that user's byte
for byte.

Default reference is ``bach.compiler.reference.checkpoint_golden`` on
PYTHONPATH (latch ``src``). ``--reference-module PATH`` loads that file
instead, for a test or an explicit checkout. There is no search path.
"""
from __future__ import annotations

import argparse
import importlib
import importlib.util
import json
import sys
from pathlib import Path

import numpy as np

BF16_ELEMENTS = 6144
BF16_BYTES = BF16_ELEMENTS * 2


def load_reference(path: Path | None):
    if path is None:
        return importlib.import_module("bach.compiler.reference.checkpoint_golden")
    spec = importlib.util.spec_from_file_location("checkpoint_golden", path)
    if spec is None or spec.loader is None:
        raise ValueError(f"Cannot load reference module {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def requested_users(text: str) -> list[int]:
    return [int(line.split()[1]) for line in text.splitlines()
            if len(line.split()) == 2 and line.split()[0] == "user"]


def parse_run(text: str) -> tuple[dict[str, str], dict[int, dict[str, str]]]:
    headers: dict[str, str] = {}
    captures: dict[int, dict[str, str]] = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0] != "user":
            headers[fields[0]] = fields[1]
        if len(fields) >= 2 and fields[:2] == ["stored_output", "user"]:
            data = dict(zip(fields[1::2], fields[2::2]))
            user = int(data["user"])
            if user in captures:
                raise ValueError(f"Duplicate completed output for {user}")
            captures[user] = data
    return headers, captures


def decode_bf16(raw: bytes) -> np.ndarray:
    if len(raw) != BF16_BYTES:
        raise ValueError(f"expected {BF16_ELEMENTS} BF16 elements, got {len(raw)} bytes")
    return (np.frombuffer(raw, dtype="<u2").astype("<u4") << 16).view("<f4").astype(np.float64)


def compare_run(run: Path, golden: Path, users: list[int] | None, reference) -> dict:
    text = (run / "run.txt").read_text()
    headers, captures = parse_run(text)
    same_token: dict[int, int] = {}
    if users is None:
        requested = requested_users(text)
        if not requested:
            raise ValueError("Run lists no requested users")
        users = requested
        same_token = {user: requested[k % 2] for k, user in enumerate(requested) if k >= 2}
    if headers.get("completed_by_end") != "1":
        raise ValueError("Execution did not reach terminal-task completion")
    if set(captures) != set(users) or len(captures) != len(users):
        raise ValueError(f"Output ownership mismatch: {set(captures)} != {set(users)}")
    seen_owner: dict[tuple[int, int], int] = {}
    report: dict = {
        "precision": "BF16 captured before SID/allocation retirement",
        "acceptance": "numerical",
        "users": {},
    }
    passed = True
    for user in users:
        data = captures[user]
        sid = data.get("sid")
        allocation = data.get("allocation")
        if sid is None or allocation is None:
            raise ValueError(f"User {user}: missing SID or allocation")
        owner = (int(sid), int(allocation))
        if owner[0] < 0 or owner[1] <= 0:
            raise ValueError(f"User {user}: invalid SID or allocation")
        completed_cycle = int(data.get("completed_cycle", "0"))
        if completed_cycle <= 0 or (
            "stopped_at" in headers and completed_cycle > int(headers["stopped_at"])
        ):
            raise ValueError(f"User {user}: invalid terminal completion cycle")
        if owner in seen_owner:
            raise ValueError(f"Duplicate owner {owner} for users {seen_owner[owner]} and {user}")
        seen_owner[owner] = user
        raw = (run / f"user{user}.bf16").read_bytes()
        if int(data["bytes"]) != BF16_BYTES:
            raise ValueError(f"User {user}: expected {BF16_ELEMENTS} BF16 elements")
        values = decode_bf16(raw)
        if not np.all(np.isfinite(values)):
            raise ValueError(f"User {user}: nonfinite output")
        if user in same_token:
            twin = same_token[user]
            same = raw == (run / f"user{twin}.bf16").read_bytes()
            report["users"][str(user)] = {"capture": data, "same_as": twin, "ok": same}
            passed = passed and same
            continue
        golden_sum = np.load(golden / f"user{user}.npz")["sum"]
        metrics = reference.compare(golden_sum, values)
        negative_doubled = reference.compare(golden_sum, np.asarray(golden_sum, dtype=np.float64) * 2)
        if negative_doubled["ok"]:
            raise ValueError("Amplitude gate accepted a doubled reference")
        report["users"][str(user)] = {
            "capture": data,
            "metrics": metrics,
            "doubled_reference_rejected": True,
        }
        passed = passed and bool(metrics["ok"])
    report["ok"] = passed
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--golden", required=True, type=Path)
    users = parser.add_mutually_exclusive_group(required=True)
    users.add_argument("--users", nargs="+", type=int)
    users.add_argument("--all-users", action="store_true")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument(
        "--reference-module",
        type=Path,
        default=None,
        help="explicit checkpoint_golden.py; default imports bach.compiler.reference.checkpoint_golden",
    )
    args = parser.parse_args(argv)
    reference = load_reference(args.reference_module)
    report = compare_run(args.run, args.golden, None if args.all_users else args.users, reference)
    args.out.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
