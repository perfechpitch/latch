"""Ten GLM5 EP6/TP8 groups of sixteen tokens, executing exported pcode only."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from copy import deepcopy
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

import numpy as np

from pcode_glm5_dense import (MODEL_SEED, PROFILES, specifications, layout,
    prepare_model, DenseModel, reference, fixture, compare, sha)
from pcode_glm5_two_token import token_spec, experts_for_mask
from pcode_glm5_dense_verify import verify_fixture
from pcode_precision_reference import PrecisionRoutes
from pcode_glm5_program_audit import audit_program


def groups():
    result = []
    seed = MODEL_SEED + 16000

    def make(ids, shared, offset, profile='signed', weights=None):
        return token_spec(ids, shared, seed + offset, profile, weights)

    def add(name, description, tokens, gap=1):
        index = len(result)
        assert len(tokens) == 16
        tokens = deepcopy(tokens)
        for i, spec in enumerate(tokens):
            spec.update(name=f'{name}.token{i:02}', family=name,
                        user=spec['shared_ep'] + 6 * ((index * 23 + i * 7) % 170),
                        token=50000 + 16 * index + i)
        if name == 'same_experts_contention':
            tokens[0]['user'], tokens[1]['user'] = 3, 1023
        assert len({s['user'] for s in tokens}) == 16
        result.append(dict(name=name, description=description, gap=gap, tokens=tokens))

    for half in range(2):
        add(f'expert_bank_{half}',
            f'Execute every expert in global range {half*128}..{half*128+127}; rotate shared EP.',
            [make(list(range(half*128+8*i, half*128+8*i+8)), i % 6, half*100+i)
             for i in range(16)], gap=1 if half == 0 else 128)
    common = [0, 1, 43, 86, 129, 172, 215, 255]
    tokens = [make(common, 3, 200+i) for i in range(16)]
    tokens[1] = deepcopy(tokens[0])
    add('same_experts_contention',
        'Sixteen users contend for identical experts/shared EP3; first two payloads identical, UID3/1023.', tokens)
    weights = np.arange(1, 9, dtype=np.float32) / 36
    tokens = [make(common, ep, 300, weights=weights) for ep in range(6)]
    tokens += [make(common[::-1], ep, 300, weights=weights[::-1]) for ep in range(6)]
    for i in range(4):
        ids = [0, 43, 86, 129, 173+i, 180+i, 216+i, 250+i]
        tokens.append(make(ids, i, 310+i))
    add('shared_permutation_overlap',
        'Six shared placements and six matching TopK reversals, plus four partial-overlap inputs.', tokens, 64)
    for part in range(4):
        masks = [min(63, part*16+i+1) for i in range(16)]
        add(f'mask_sweep_{part}',
            f'Routed mask range {masks[0]:#x}..{masks[-1]:#x}; rotate shared inside/outside routed set.',
            [make(experts_for_mask(mask, seed+400+part*16+i), (part+i) % 6, 500+part*16+i)
             for i, mask in enumerate(masks)], [1, 128, 256, 1][part])
    boundary = [s for s in specifications() if s['family'] == 'boundary']
    add('boundary_ring',
        'First/last experts in all EPs and four adjacent boundaries; includes both boundary_pair_15 payloads.',
        boundary[:16], 128)
    tokens = []
    for i in range(16):
        ids = boundary[16+i]['global_experts'] if i < 2 else experts_for_mask(63, seed+700+i)
        w = np.arange(1, 9, dtype=np.float32) / 36
        if i in (12, 13):
            w = np.array([1, 1, 1, 1, 0, 0, 0, 0], dtype=np.float32) / 4
        tokens.append(make(ids, i % 6, 800+i, PROFILES[i//2], w))
    add('numeric_isolation',
        'Eight input profiles x two seeds, zero coefficients, final EP boundary and global ends.', tokens, 64)
    assert len(result) == 10
    assert {e for g in result[:2] for s in g['tokens'] for e in s['global_experts']} == set(range(256))
    assert {s['routed_mask'] for g in result[4:8] for s in g['tokens']} == set(range(1, 64))
    return result


def evidence(text, group, model_bytes):
    """Validate raw native identity/mask/concurrency evidence, without assuming output order."""
    sent = [tuple(map(int, x)) for x in re.findall(
        r'pcode sent token (\d+) at (\d+) user (\d+) token (\d+)', text)]
    received = [tuple(map(int, x)) for x in re.findall(
        r'pcode received token (\d+) at (\d+) user (\d+) token (\d+)', text)]
    wire_heads = [tuple(map(int,x)) for x in re.findall(
        r'pcode wire token (\d+) head at (\d+) user (\d+) token (\d+)', text)]
    wire_tails = [tuple(map(int,x)) for x in re.findall(
        r'pcode wire token (\d+) tail at (\d+) user (\d+) token (\d+)', text)]
    expected = {i: (s['user'], s['token']) for i, s in enumerate(group['tokens'])}

    def identities(rows):
        return (len(rows) == 16 and {i for i, *_ in rows} == set(range(16))
                and all(expected[i] == (u, t) for i, c, u, t in rows))

    masks = [tuple(map(int, x)) for x in re.findall(
        r'pcode mask routed (\d+), shared EP (\d+), actual (\d+)', text)]
    wanted_masks = [(s['routed_mask'], s['shared_ep'], s['actual_ep_mask']) for s in group['tokens']]
    stop = re.search(r'pcode stopped at (\d+), output packets (\d+)', text)
    occupancy = re.search(r'pcode concurrency summary peak (\d+) core peak (\d+) overlapped users (\d+) stream mask (\d+)', text)
    peak, core_peak, participants, stream_mask = map(int, occupancy.groups()) if occupancy else (0, 0, 0, 0)
    all_active = re.search(r'pcode concurrent compute users at (\d+)((?: \d+)+)', text)
    all_cycle = int(all_active[1]) if all_active else None
    all_users = set(map(int, all_active[2].split())) if all_active else set()
    sent_times = {i: c for i, c, *_ in sent}
    received_times = {i: c for i, c, *_ in received}
    heads = {i:c for i,c,*_ in wire_heads}; tails = {i:c for i,c,*_ in wire_tails}
    causal = identities(sent) and identities(received) and all(
        sent_times[i] < received_times[i] for i in expected)
    wire_ok = bool(causal and identities(wire_heads) and identities(wire_tails)
        and all(sent_times[i] <= heads[i] < tails[i] < received_times[i] for i in expected)
        and all(tails[i] < heads[i+1] for i in range(15)))
    return dict(sent_at=sent_times, received_at=received_times,
        wire_head_at=heads, wire_tail_at=tails, wire_ingress_verified=wire_ok,
        identities_verified=identities(sent) and identities(received), causal_timing_verified=bool(causal),
        native_masks_verified=masks == wanted_masks,
        all_model_images_preloaded=re.findall(r'pcode dense model images (\d+), data bytes (\d+)', text)
            == [('768', str(model_bytes))],
        peak_compute_users=peak, peak_core_users=core_peak,
        overlap_participants=participants, observed_stream_mask=stream_mask,
        all_sixteen_compute_cycle=all_cycle,
        all_sixteen_compute_verified=bool(peak == participants == 16 and all_users == set(u for u, _ in expected.values())
            and causal and max(sent_times.values()) <= all_cycle < min(received_times.values())),
        completed_cycles=int(stop[1]) if stop else None, output_packets=int(stop[2]) if stop else 0,
        native_state_checks_passed='[  PASSED  ] 1 test.' in text and '[  FAILED  ]' not in text)


def source_identity(args):
    files = {'runner': Path(__file__), 'harness': Path(__file__).with_name('pcode_bundle.cpp'),
             'spread_harness': Path(__file__).parent/'ip/chip/moe_common.h',
             'dense_reference': Path(__file__).with_name('pcode_glm5_dense.py'),
             'reference_helpers': Path(__file__).with_name('pcode_campaign.py'),
             'precision_routes': Path(__file__).with_name('pcode_precision_reference.py'),
             'pair_helpers': Path(__file__).with_name('pcode_glm5_two_token.py'),
             'program_audit': Path(__file__).with_name('pcode_glm5_program_audit.py'),
             'fixture_decoder': Path(__file__).with_name('pcode_glm5_dense_verify.py'),
             'simulator': args.simulator, 'catalog': args.catalog,
             'model_manifest': args.model/'manifest.json'}
    result = {k+'_sha256': sha(p) for k, p in files.items()}
    digest = hashlib.sha256()
    for p in sorted([args.bundle/'pcode_ep6_tp8.bachir', *args.bundle.glob('*.hex')]):
        digest.update(p.name.encode()+b'\0'+p.read_bytes())
    result['bundle_sha256'] = digest.hexdigest()
    result['plan_sha256'] = hashlib.sha256(json.dumps(groups(), sort_keys=True).encode()).hexdigest()
    return result


def summarize(args, reports, planned, identity):
    tokens = [t for r in reports for t in r['tokens']]
    cos = [t['cosine_similarity'] for t in tokens if t.get('cosine_similarity') is not None]
    summary = dict(total=len(reports), planned=planned, passed=sum(r['passed'] for r in reports),
        failed=[r['name'] for r in reports if not r['passed']], token_reports=len(tokens),
        token_outputs=sum('actual_sha256' in t for t in tokens),
        token_outputs_passed=sum(bool(t.get('passed')) for t in tokens),
        reporting_runner_sha256=sha(Path(__file__)),
        minimum_nonzero_cosine=min(cos) if cos else None,
        maximum_relative_l2=max((t.get('relative_l2_error', 0) for t in tokens), default=0),
        identity=identity, cases=sorted(reports, key=lambda r:r['name']))
    (args.out/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    lines = ['GLM5 EP6+TP8: 10 groups x 16 tokens',
             f'Completed {len(reports)}/{planned}; passed {summary["passed"]}; '
             f'token outputs {summary["token_outputs"]}/{len(tokens)} planned',
             'group\tpass\toutputs\tcycles\tpeak_global/core\tcos_min\tL2_max']
    for r in summary['cases']:
        cc = [t['cosine_similarity'] for t in r['tokens'] if t.get('cosine_similarity') is not None]
        lines.append(f'{r["name"]}\t{r["passed"]}\t{r["output_packets"]}\t{r["completed_cycles"]}'
            f'\t{r["peak_compute_users"]}/{r["peak_core_users"]}\t{min(cc) if cc else None}'
            f'\t{max((t.get("relative_l2_error",0) for t in r["tokens"]),default=0)}')
        for i, t in enumerate(r['tokens']):
            lines.append(f'  token{i:02} uid={t["user"]} token={t["token"]} '
                f'routed={t["routed_mask_hex"]} shared={t["shared_ep"]} actual={t["actual_ep_mask_hex"]} '
                f'experts={t["global_experts"]} profile={t["profile"]} seed={t["input_seed"]} '
                f'pass={t["passed"]} cos={t.get("cosine_similarity")} L2={t.get("relative_l2_error")}')
    (args.out/'results.txt').write_text('\n'.join(lines)+'\n')
    return summary


def run(args):
    args.out.mkdir(parents=True, exist_ok=True)
    plan = groups()
    selected = [g for g in plan if args.filter in g['name']]
    if args.limit: selected = selected[:args.limit]
    assert selected and 1 <= args.workers <= 2 and 1 <= args.threads <= 16
    (args.out/'planned_cases.json').write_text(json.dumps(plan, indent=2)+'\n')
    if args.plan_only:
        print(json.dumps([dict(name=g['name'],description=g['description'],gap=g['gap']) for g in selected],indent=2))
        return
    templates, logical = layout(args.bundle, args.catalog)
    audit = audit_program(args.bundle, args.catalog, templates)
    (args.out/'program_audit.json').write_text(json.dumps(audit, indent=2)+'\n')
    manifest = prepare_model(args.model, templates, logical)
    model, routes = DenseModel(args.model, manifest), PrecisionRoutes(args.bundle, args.catalog, logical)
    identity = source_identity(args)

    def execute(group):
        name = group['name']; out = args.out/name; out.mkdir(exist_ok=True)
        report_path = out/'report.json'
        if args.resume and report_path.exists():
            r = json.loads(report_path.read_text())
            if r.get('passed') and r['identity'] == identity and r['threads'] == args.threads and r['maximum_cycles'] == args.max_cycles:
                assert sha(out/'native.log') == r['log_sha256']
                for i, t in enumerate(r['tokens']):
                    assert sha(out/(t['name']+'.fixture')) == t['fixture_sha256']
                    assert sha(out/(t['name']+'.expected.bf16')) == t['expected_sha256']
                    assert sha(out/f'actual.token{i}.bf16') == t['actual_sha256']
                return r
        start = time.monotonic(); metadata = []
        for spec in group['tokens']:
            meta = fixture(spec, out)
            verify_fixture(out/(spec['name']+'.fixture'), spec)
            expected, shared_hash = reference(spec, model, logical, routes)
            expected_path = out/(spec['name']+'.expected.bf16')
            expected_path.write_bytes(expected.astype('<u2').tobytes())
            metadata.append(dict(meta, expected_sha256=sha(expected_path), shared_reference_sha256=shared_hash))
        listing = out/'fixtures.list'
        listing.write_text(''.join(str((out/(s['name']+'.fixture')).resolve())+'\n' for s in group['tokens']))
        env = dict(os.environ)
        for key in ['PCODE_CASE_FILE','PCODE_SECOND_CASE_FILE','PCODE_TRACE_PREFIX','PCODE_TRACE_CHIPS','PCODE_SNAPSHOT_PREFIX']:
            env.pop(key, None)
        env.update(PCODE_BUNDLE_ROOT=str(args.bundle.parent.resolve()), PCODE_CASE_LIST=str(listing.resolve()),
            PCODE_DENSE_MODEL=str(args.model.resolve()), PCODE_ACTUAL=str((out/'actual').resolve()),
            PCODE_THREADS=str(args.threads), PCODE_MAX_CYCLES=str(args.max_cycles),
            PCODE_INJECT_MODE='overlap', PCODE_TOKEN_GAP=str(group['gap']), PCODE_REQUIRE_COMPUTE_OVERLAP='1')
        for i in range(16): (out/f'actual.token{i}.bf16').unlink(missing_ok=True)
        print(f'start {name}: 16 tokens gap={group["gap"]}', flush=True)
        try:
            with (out/'native.log').open('w') as log:
                code = subprocess.run([str(args.simulator.resolve())], env=env, stdout=log,
                    stderr=subprocess.STDOUT, timeout=args.timeout).returncode
        except subprocess.TimeoutExpired: code = 124
        text = (out/'native.log').read_text()
        report = dict(name=name, description=group['description'], gap=group['gap'], threads=args.threads,
            maximum_cycles=args.max_cycles, simulator_returncode=code, identity=identity,
            log_sha256=sha(out/'native.log'), wall_seconds=time.monotonic()-start,
            **evidence(text, group, manifest['total_data_bytes']), tokens=[])
        for i, meta in enumerate(metadata):
            try: metrics = compare(out/f'actual.token{i}.bf16', out/(meta['name']+'.expected.bf16'))
            except (OSError, AssertionError, ValueError) as error: metrics = dict(passed=False,error=str(error))
            report['tokens'].append(dict(meta, **metrics))
        report['passed'] = bool(code == 0 and report['output_packets'] == 16
            and report['completed_cycles'] < args.max_cycles
            and all(report[k] for k in ['identities_verified','causal_timing_verified','native_masks_verified',
                'wire_ingress_verified','all_model_images_preloaded','all_sixteen_compute_verified','native_state_checks_passed'])
            and all(t['passed'] for t in report['tokens']))
        report_path.write_text(json.dumps(report, indent=2)+'\n')
        return report

    reports = []
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for f in as_completed([pool.submit(execute, g) for g in selected]):
            r = f.result(); reports.append(r)
            summarize(args, reports, len(selected), identity)
            print(f'{len(reports)}/{len(selected)} {r["name"]} {"PASS" if r["passed"] else "FAIL"} '
                f'outputs={r["output_packets"]} cycles={r["completed_cycles"]} '
                f'peak={r["peak_compute_users"]}/{r["peak_core_users"]}', flush=True)
    if not all(r['passed'] for r in reports): raise SystemExit(1)


def verify(args):
    summary = json.loads((args.out/'summary.json').read_text())
    identity = source_identity(args)
    assert summary['identity'] == identity
    templates, logical = layout(args.bundle, args.catalog)
    manifest = prepare_model(args.model, templates, logical)
    model, routes = DenseModel(args.model, manifest), PrecisionRoutes(args.bundle, args.catalog, logical)
    specs = {g['name']: g for g in groups()}
    selected = [g for g in groups() if args.filter in g['name']]
    if args.limit: selected = selected[:args.limit]
    assert summary['total'] == summary['planned'] == summary['passed'] == len(selected)
    assert not summary['failed'] and len(summary['cases']) == len(selected)
    assert {r['name'] for r in summary['cases']} == {g['name'] for g in selected}
    assert json.loads((args.out/'planned_cases.json').read_text()) == groups()
    for r in summary['cases']:
        group = specs[r['name']]; out = args.out/r['name']
        assert r['passed'] and r['identity'] == identity and sha(out/'native.log') == r['log_sha256']
        assert r['simulator_returncode'] == 0 and r['completed_cycles'] < r['maximum_cycles']
        for k, v in evidence((out/'native.log').read_text(), group, manifest['total_data_bytes']).items():
            # JSON converts integer dictionary keys to strings.
            assert r[k] == json.loads(json.dumps(v)), k
        for i, spec in enumerate(group['tokens']):
            record = r['tokens'][i]; verify_fixture(out/(spec['name']+'.fixture'), spec)
            with tempfile.TemporaryDirectory() as tmp:
                rebuilt = fixture(spec, Path(tmp))
                assert all(record[k] == v for k,v in rebuilt.items())
            assert sha(out/(spec['name']+'.fixture')) == record['fixture_sha256']
            expected, shared_hash = reference(spec, model, logical, routes)
            assert shared_hash == record['shared_reference_sha256']
            raw = expected.astype('<u2').tobytes()
            assert hashlib.sha256(raw).hexdigest() == record['expected_sha256']
            assert raw == (out/(spec['name']+'.expected.bf16')).read_bytes()
            metrics = compare(out/f'actual.token{i}.bf16', out/(spec['name']+'.expected.bf16'))
            assert metrics['passed'] and all(record[k] == v for k,v in metrics.items())
        print(f'reverified {r["name"]}: 16 independent references/fixtures/outputs',flush=True)
    report = dict(verified_at=datetime.now(timezone.utc).isoformat(), groups=len(summary['cases']),
                  tokens=16*len(summary['cases']), passed=True, identity=identity)
    (args.out/'verification.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bundle', type=Path, required=True)
    p.add_argument('--catalog', type=Path, required=True)
    p.add_argument('--model', type=Path, default=Path('build-ep6/glm5-dense-model'))
    p.add_argument('--simulator', type=Path, default=Path('build-ep6/pcode_bundle'))
    p.add_argument('--out', type=Path, default=Path('build-ep6/glm5-16-token'))
    p.add_argument('--threads', type=int, default=4)
    p.add_argument('--workers', type=int, default=1)
    p.add_argument('--max-cycles', type=int, default=80000)
    p.add_argument('--timeout', type=int, default=2400)
    p.add_argument('--filter', default='')
    p.add_argument('--limit', type=int)
    p.add_argument('--resume', action='store_true')
    p.add_argument('--plan-only', action='store_true')
    p.add_argument('--verify-only', action='store_true')
    args = p.parse_args()
    verify(args) if args.verify_only else run(args)
