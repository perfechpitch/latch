"""Rebuild independent oracles and audit every saved two-token output."""
import argparse
from collections import Counter
from datetime import datetime
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess
import tempfile
from zoneinfo import ZoneInfo

import numpy as np

from pcode_campaign import layout, make_fixture, compare
from pcode_precision_reference import PrecisionRoutes
from pcode_two_token import pairs, audit_shared_model


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bundle', type=Path, required=True)
    ap.add_argument('--catalog', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--report', type=Path, required=True)
    args = ap.parse_args()
    summary = json.loads((args.out/'summary.json').read_text())
    planned = {p['name']: p for p in pairs()}
    assert summary['total'] == summary['planned'] == summary['passed'] == len(planned) == 52
    assert not summary['failed']
    assert {r['name'] for r in summary['cases']} == planned.keys()
    thread_counts = {r['threads'] for r in summary['cases']}
    assert len(thread_counts) == 1
    tpl, logical = layout(args.bundle, args.catalog)
    routes = PrecisionRoutes(args.bundle, args.catalog, logical)
    identity = {key+'_sha256': sha(path) for key, path in [
        ('simulator', Path('build-ep6/pcode_bundle')),
        ('runner', Path(__file__).with_name('pcode_two_token.py')),
        ('fixture_generator', Path(__file__).with_name('pcode_campaign.py')),
        ('precision_reference', Path(__file__).with_name('pcode_precision_reference.py'))]}
    digest = hashlib.sha256()
    for path in sorted([args.bundle/'pcode_ep6_tp8.bachir', *args.bundle.glob('*.hex')]):
        digest.update(path.name.encode()+b'\0'+path.read_bytes())
    identity['bundle_sha256'] = digest.hexdigest()
    all_tokens = []
    coverage = dict(overlap_pairs=sum(p['mode'] == 'overlap' for p in planned.values()),
                    sequential_pairs=sum(p['mode'] == 'sequential' for p in planned.values()),
                    same_user_reuse_pairs=sum(p['tokens'][0]['user'] == p['tokens'][1]['user'] for p in planned.values()),
                    full_k_tile_pairs=sum('matrix_seed' in p['tokens'][0] and
                        (p['tokens'][0]['matrix_seed']-20261010)%16 == 0 for p in planned.values()))
    assert coverage == dict(overlap_pairs=37, sequential_pairs=15, same_user_reuse_pairs=9, full_k_tile_pairs=8)
    for report in summary['cases']:
        pair = planned[report['name']]
        assert report['passed'] and report['simulator_returncode'] == 0 and report['output_packets'] == 2
        assert all(report[k] == v for k, v in identity.items())
        assert report['mode'] == pair['mode'] and report['gap'] == pair['gap']
        assert len(report['sent_at']) == len(report['received_at']) == 2
        overlap = report['sent_at']['1'] < report['received_at']['0']
        assert overlap == (pair['mode'] == 'overlap')
        if not overlap:
            assert report['sent_at']['1'] > report['received_at']['0']
        with tempfile.TemporaryDirectory(prefix='pcode-two-token-oracle-') as temp:
            fresh = Path(temp)
            fixture_paths = []
            for index, spec in enumerate(pair['tokens']):
                old = report['tokens'][index]
                meta = make_fixture(spec, tpl, logical, fresh, routes)
                for k, v in meta.items():
                    assert old[k] == v, (report['name'], index, k)
                fixture = args.out/(spec['name']+'.fixture')
                fixture_paths.append(fixture)
                assert sha(fixture) == meta['fixture_sha256']
                expected = fresh/(spec['name']+'.expected.bf16')
                assert expected.read_bytes() == (args.out/expected.name).read_bytes()
                actual = args.out/(pair['name']+f'.actual.token{index}.bf16')
                metrics = compare(actual, expected, exact=spec['family'] != 'numerical')
                assert metrics['passed'] and metrics['actual_sha256'] == old['actual_sha256']
                assert all(metrics[k] == old[k] for k in metrics)
                old['expected_sha256'] = sha(expected)
                all_tokens.append(old)
            assert audit_shared_model(*fixture_paths) == report['model_audit']
        print('verified '+report['name'], flush=True)
    records = [line.split() for line in (args.bundle/'pcode_ep6_tp8.bachir').read_text().splitlines()]
    rt = [f for f in records if f and f[0] == 'RTAB']
    assert not [f for f in rt if int(f[12]) and int(f[14])]
    entries = {(int(f[1]), int(f[2]), int(f[3])): f for f in rt}
    bad = {int(f[1]): int(f[2], 0) for f in records if f and f[0] == 'CHIP'}
    catalog = json.loads(args.catalog.read_text())
    requests = json.loads((args.catalog.parent.parent/'route_intents.json').read_text())
    masked_destinations = 0
    rows = 0
    for net, request in enumerate(requests):
        if request['step'] != 4 or request['kind'] != 'broadcast':
            continue
        rows += 1
        tray, chip, _ = request['src_coord'][0]
        ep = (tray*16+chip)//8
        assert request['core_mask_idx'] == [ep]*32
        destinations = 0
        for chip in range(48):
            harvests = catalog['chips'][f'c{chip}']['harvests']
            selected = next(h for k, h in sorted(harvests.items(), key=lambda item: int(item[0][1:]))
                            if h['disable_mask'] == bad[chip])
            for key, core in selected['cores'].items():
                for raw in core.get('entries', []):
                    if raw.get('net') != net or not int(raw['directionMask']) & 16:
                        continue
                    assert raw['use_core_mask'] == 1 and raw['core_mask_idx'] == ep
                    native = entries[chip, int(key[1:]), int(raw['PathID'])]
                    assert int(native[12]) == 1 and int(native[13]) == ep and int(native[14]) == 0
                    destinations += 1
        assert destinations == 32
        masked_destinations += destinations
    assert rows == 12 and masked_destinations == 384
    images = {f[-1] for f in records if f and f[0] == 'KERNELCORE'}
    assert len(images) == 855 and all((args.bundle/name).is_file() for name in images)
    assert len(tpl) == 768
    stages = Counter()
    for (_, _, stage), t in tpl.items():
        assert t[0]&65535 == (4 if stage == 0 else 6)
        assert t[0]>>16 == (12 if stage == 0 else 4)
        assert t[7] == (393216 if stage == 0 else 196608)
        stages[stage] += 1
    assert stages == {0: 384, 1: 384}
    def git(root, *args):
        return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()
    bach = args.bundle.parents[3]
    latch = Path.cwd()
    assert git(bach, 'branch', '--show-current') == git(latch, 'branch', '--show-current') == 'pcode_dev_zlm'
    result = dict(
        verified_at=datetime.now(ZoneInfo('Asia/Shanghai')).isoformat(),
        status='passed', total=52, passed=52, failed=[], token_outputs=len(all_tokens),
        program_origin=dict(generator='BachFlow pcode', name='pcode_ep6_tp8', bachir_version=11,
                            source='pcode/output/ep6_single_token', rv_hex_images=len(images),
                            internal_latch_moe_compiler_used=False),
        configuration=dict(ep=6, tp=8, chips=48, tokens_per_fresh_process=2,
                           simulator_threads=thread_counts.pop(),
                           hidden_dimension=6144, expert_intermediate_dimension=2048,
                           weights='immutable controlled sparse MXFP8, preloaded into MatrixMem',
                           cycle_limit=50000, wall_timeout_seconds=2400),
        versions=dict(bachflow_source_commit=git(bach, 'rev-parse', 'HEAD'),
                      latch_source_commit=git(latch, 'rev-parse', 'HEAD'), branch='pcode_dev_zlm',
                      python=platform.python_version(), numpy=np.__version__,
                      machine=platform.machine(), system=platform.system()),
        identity=identity,
        verification=dict(rebuilt_oracles=104, fixture_hashes_checked=104,
                          actual_outputs_recompared=104, immutable_model_pairs_audited=52,
                          verifier_sha256=sha(Path(__file__)), route_catalog_sha256=sha(args.catalog)),
        coverage=dict(**coverage, token_families=dict(Counter(t['family'] for t in all_tokens))),
        statistics=dict(bit_exact_tokens=sum(t['bf16_mismatches'] == 0 for t in all_tokens),
                        zero_reference_tokens=sum(t['zero_reference'] for t in all_tokens),
                        minimum_nonzero_cosine=min(t['cosine_similarity'] for t in all_tokens if not t['zero_reference']),
                        maximum_relative_l2_error=max(t['relative_l2_error'] for t in all_tokens),
                        maximum_bf16_mismatches=max(t['bf16_mismatches'] for t in all_tokens),
                        minimum_completed_cycles=min(r['completed_cycles'] for r in summary['cases']),
                        maximum_completed_cycles=max(r['completed_cycles'] for r in summary['cases'])),
        export_audit=dict(route_entries=len(rt), masked_bypass_entries=0,
                          fc13_broadcast_rows=rows, fc13_masked_destinations=masked_destinations,
                          fc13_templates=384, fc13_shape=[512, 768], fc13_expert_stride=393216,
                          fc2_templates=384, fc2_shape=[768, 256], fc2_expert_stride=196608),
        cases=summary['cases'])
    assert result['token_outputs'] == 104
    regression_logs = {
        'router_reduce': ('router_reduce-final-regression.log', 21),
        'router_scenarios': ('router_scenarios-credit-reuse-regression.log', 7),
        'router_assembly': ('router_assembly-credit-reuse-regression.log', 14),
        'bundle_load': ('bundle_load-final-regression.log', 13),
        'ts_done': ('ts_done-final-regression.log', 12),
        'ts_credit': ('ts_credit-credit-reuse-regression.log', 7),
    }
    result['affected_module_regressions'] = {}
    for target, (name, count) in regression_logs.items():
        log = args.out.parent/name
        assert f'[  PASSED  ] {count} tests.' in log.read_text()
        result['affected_module_regressions'][target] = dict(passed=count, log_sha256=sha(log))
    result['regressions_before_latch_fixes'] = []
    for name, reason, cycles, packets in [
        ('numeric-reverse-diag5.log', 'one user context serialized independent reduction paths', 11000, 0),
        ('same-user-diag.log', 'credit deduplication suppressed the next token request', 19000, 1),
    ]:
        log = args.out.parent/name
        if log.exists():
            stop = re.search(r'pcode stopped at (\d+), output packets (\d+)', log.read_text())
            assert stop and (int(stop[1]), int(stop[2])) == (cycles, packets)
            assert '[  FAILED  ] PcodeBundle.Ep6ExportedTokens' in log.read_text()
            result['regressions_before_latch_fixes'].append(dict(
                log=name, log_sha256=sha(log), completed_cycles=cycles, output_packets=packets, reason=reason))
    before = args.out.parent/'two-token-before-row-mask/route_overlap_03.report.json'
    if before.exists():
        failure = json.loads(before.read_text())
        assert failure['simulator_returncode'] != 0 and failure['output_packets'] == 1
        result['regression_before_row_mask_fix'] = dict(
            name=failure['name'], input_users=[t['user'] for t in failure['tokens']],
            simulator_returncode=failure['simulator_returncode'],
            completed_cycles=failure['completed_cycles'], output_packets=failure['output_packets'],
            simulator_sha256=failure['simulator_sha256'], bundle_sha256=failure['bundle_sha256'],
            report_sha256=sha(before), reason='inactive EP0 first row created incomplete TP reductions')
    args.report.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k: result[k] for k in ['status', 'total', 'token_outputs', 'statistics']}))


if __name__ == '__main__':
    main()
