"""Synthetic defects for the completion comparison. No simulator, no checkpoint."""
import json
import subprocess
import sys
import textwrap
from pathlib import Path

import numpy as np
import pytest

MODULE = Path(__file__).resolve().parents[1] / "compare_output.py"


def _reference(directory: Path) -> Path:
    path = directory / "checkpoint_golden.py"
    path.write_text(textwrap.dedent(
        """\
        import numpy as np

        REL_L2 = 0.15
        NORM_RATIO_MIN = 0.85
        NORM_RATIO_MAX = 1.15
        COSINE_MIN = 0.99

        def compare(reference, other, *, rel_l2=REL_L2, norm_min=NORM_RATIO_MIN,
                    norm_max=NORM_RATIO_MAX, cosine_min=COSINE_MIN):
            a = np.asarray(reference, dtype=np.float64).reshape(-1)
            b = np.asarray(other, dtype=np.float64).reshape(-1)
            if a.shape != b.shape:
                raise ValueError(f"compare shape {a.shape} != {b.shape}")
            if not np.isfinite(a).all() or not np.isfinite(b).all():
                raise ValueError("compare refuses non-finite values")
            na = float(np.linalg.norm(a))
            nb = float(np.linalg.norm(b))
            diff = float(np.linalg.norm(a - b))
            if na == 0.0:
                ok = nb == 0.0
                return {"ok": bool(ok), "rel_l2": 0.0 if ok else float("inf"),
                        "norm_ratio": 1.0 if ok else float("inf"), "cosine": 1.0 if ok else 0.0}
            rel = diff / na
            ratio = nb / na
            cos = float((a @ b) / (na * nb)) if nb > 0.0 else 0.0
            ok = rel <= rel_l2 and norm_min <= ratio <= norm_max and cos >= cosine_min
            return {"ok": bool(ok), "rel_l2": rel, "norm_ratio": float(ratio), "cosine": cos}
        """
    ))
    return path


def _bf16(values: np.ndarray) -> bytes:
    return (np.asarray(values, dtype=np.float32).view(np.uint32) >> 16).astype("<u2").tobytes()


def _layout(tmp: Path, users: dict[int, np.ndarray], lines: list[str]):
    run = tmp / "run"
    golden = tmp / "golden"
    run.mkdir()
    golden.mkdir()
    for user, values in users.items():
        np.savez(golden / f"user{user}.npz", sum=np.asarray(values, dtype=np.float64))
        (run / f"user{user}.bf16").write_bytes(_bf16(values))
    (run / "run.txt").write_text("".join(line + "\n" for line in lines))
    return run, golden


def _stored(user: int, sid: int, allocation: int, nbytes: int = 12288) -> str:
    return (
        f"stored_output user {user} sid {sid} allocation {allocation} "
        f"bytes {nbytes} completed_cycle 99"
    )


def _run(tmp: Path, run: Path, golden: Path, users: list[int] | None) -> subprocess.CompletedProcess[str]:
    selection = ["--all-users"] if users is None else ["--users", *[str(user) for user in users]]
    return subprocess.run(
        [
            sys.executable, "-I", str(MODULE),
            "--run", str(run),
            "--golden", str(golden),
            "--reference-module", str(_reference(tmp)),
            *selection,
            "--out", str(tmp / "metrics.json"),
        ],
        capture_output=True,
        text=True,
        check=False,
    )


def _values() -> np.ndarray:
    return np.linspace(-1, 1, 6144, dtype=np.float32)


def test_matching_completion_passes(tmp_path: Path):
    values = _values()
    run, golden = _layout(
        tmp_path,
        {101: values},
        ["completed_by_end 1", _stored(101, 3, 7)],
    )
    result = _run(tmp_path, run, golden, [101])
    assert result.returncode == 0, result.stderr
    report = json.loads((tmp_path / "metrics.json").read_text())
    assert report["ok"] is True
    assert report["users"]["101"]["doubled_reference_rejected"] is True


def test_wrong_user_rejected(tmp_path: Path):
    values = _values()
    run, golden = _layout(
        tmp_path,
        {101: values},
        ["completed_by_end 1", _stored(202, 3, 7)],
    )
    (run / "user202.bf16").write_bytes((run / "user101.bf16").read_bytes())
    result = _run(tmp_path, run, golden, [101])
    assert result.returncode != 0
    assert "Output ownership mismatch" in result.stderr


def test_incomplete_rejected(tmp_path: Path):
    values = _values()
    run, golden = _layout(
        tmp_path,
        {101: values},
        ["completed_by_end 0", _stored(101, 3, 7)],
    )
    result = _run(tmp_path, run, golden, [101])
    assert result.returncode != 0
    assert "did not reach terminal-task completion" in result.stderr


@pytest.mark.parametrize("completion", ["", " completed_cycle 0", " completed_cycle 101"])
def test_invalid_output_completion_rejected(tmp_path: Path, completion: str):
    values = _values()
    row = _stored(101, 3, 7).replace(" completed_cycle 99", completion)
    run, golden = _layout(
        tmp_path, {101: values}, ["stopped_at 100", "completed_by_end 1", row],
    )
    result = _run(tmp_path, run, golden, [101])
    assert result.returncode != 0
    assert "invalid terminal completion cycle" in result.stderr


def test_doubled_amplitude_rejected(tmp_path: Path):
    values = _values()
    run, golden = _layout(
        tmp_path,
        {101: values},
        ["completed_by_end 1", _stored(101, 3, 7)],
    )
    (run / "user101.bf16").write_bytes(_bf16(values * 2))
    result = _run(tmp_path, run, golden, [101])
    assert result.returncode == 1
    assert '"ok": false' in result.stdout


def test_duplicate_user_rejected(tmp_path: Path):
    values = _values()
    run, golden = _layout(
        tmp_path,
        {101: values},
        ["completed_by_end 1", _stored(101, 3, 7), _stored(101, 4, 8)],
    )
    result = _run(tmp_path, run, golden, [101])
    assert result.returncode != 0
    assert "Duplicate completed output" in result.stderr


def test_duplicate_owner_rejected(tmp_path: Path):
    first = _values()
    second = np.linspace(-0.5, 0.5, 6144, dtype=np.float32)
    run, golden = _layout(
        tmp_path,
        {101: first, 202: second},
        ["completed_by_end 1", _stored(101, 3, 7), _stored(202, 3, 7)],
    )
    result = _run(tmp_path, run, golden, [101, 202])
    assert result.returncode != 0
    assert "Duplicate owner" in result.stderr


@pytest.mark.parametrize("second_sid,second_allocation", [(4, 7), (3, 8)])
def test_distinct_owner_pairs_pass(tmp_path: Path, second_sid: int, second_allocation: int):
    first = _values()
    second = np.linspace(-0.5, 0.5, 6144, dtype=np.float32)
    run, golden = _layout(
        tmp_path,
        {101: first, 202: second},
        ["completed_by_end 1", _stored(101, 3, 7), _stored(202, second_sid, second_allocation)],
    )
    result = _run(tmp_path, run, golden, [101, 202])
    assert result.returncode == 0, result.stderr


def test_nonfinite_rejected(tmp_path: Path):
    values = _values()
    run, golden = _layout(
        tmp_path,
        {101: values},
        ["completed_by_end 1", _stored(101, 3, 7)],
    )
    broken = values.copy()
    broken[0] = np.float32(np.inf)
    (run / "user101.bf16").write_bytes(_bf16(broken))
    result = _run(tmp_path, run, golden, [101])
    assert result.returncode != 0
    assert "nonfinite" in result.stderr or "non-finite" in result.stderr


def test_wrong_element_count_rejected(tmp_path: Path):
    values = _values()
    run, golden = _layout(
        tmp_path,
        {101: values},
        ["completed_by_end 1", _stored(101, 3, 7, nbytes=12286)],
    )
    (run / "user101.bf16").write_bytes(_bf16(values)[:-2])
    result = _run(tmp_path, run, golden, [101])
    assert result.returncode != 0
    assert "6144" in result.stderr


def _many_users(tmp: Path, *, missing: int | None = None):
    a, b = _values(), _values()[::-1].copy()
    run, golden = _layout(tmp, {101: a, 202: b}, [])
    users = [101, 202, 302, 303]
    for user, values in ((302, a), (303, b)):
        (run / f"user{user}.bf16").write_bytes(_bf16(values))
    lines = ["completed_by_end 1"]
    lines += [_stored(u, k, 1) for k, u in enumerate(users) if u != missing]
    lines += [f"user {u}" for u in users]
    (run / "run.txt").write_text("".join(line + "\n" for line in lines))
    return run, golden


def test_all_users_pass_when_later_users_match_their_token(tmp_path: Path):
    run, golden = _many_users(tmp_path)
    result = _run(tmp_path, run, golden, None)
    assert result.returncode == 0, result.stderr
    report = json.loads((tmp_path / "metrics.json").read_text())
    assert report["users"]["302"]["same_as"] == 101
    assert report["users"]["303"]["same_as"] == 202


def test_all_users_reject_a_later_user_that_differs(tmp_path: Path):
    run, golden = _many_users(tmp_path)
    (run / "user303.bf16").write_bytes((run / "user101.bf16").read_bytes())
    result = _run(tmp_path, run, golden, None)
    assert result.returncode == 1
    assert json.loads((tmp_path / "metrics.json").read_text())["users"]["303"]["ok"] is False


def test_all_users_reject_a_missing_user(tmp_path: Path):
    run, golden = _many_users(tmp_path, missing=303)
    result = _run(tmp_path, run, golden, None)
    assert result.returncode != 0
    assert "Output ownership mismatch" in result.stderr
