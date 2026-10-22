"""Audit all saved sixteen-token attempts, including failed native executions.

Unlike --verify-only in the runner, this preserves a failed campaign as failed,
while independently rebuilding its fixtures/references and checking every output
that actually exists. Missing outputs never count as numerically verified.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import tempfile

from pcode_glm5_multi_token import (groups, source_identity, evidence, layout,
    prepare_model, DenseModel, PrecisionRoutes, verify_fixture, fixture, reference,
    compare, sha)
from pcode_glm5_relations import shared_difference_check


def run(args):
    summary = json.loads((args.out/'summary.json').read_text())
    plan = groups(); source = summary['identity']; current = source_identity(args)
    executed_runner = None
    if current != source:
        assert set(current)==set(source)
        assert all(current[k]==source[k] for k in source if k!='runner_sha256')
        executed_runner = args.executed_runner or args.out.parent/'executed_sources/test/bach/pcode_glm5_multi_token.py'
        assert sha(executed_runner)==source['runner_sha256']
    assert summary['total'] == summary['planned'] == len(plan) == 10
    reports = {r['name']:r for r in summary['cases']}
    assert len(reports) == 10 and set(reports) == {g['name'] for g in plan}
    assert summary['passed'] == sum(r['passed'] for r in reports.values())
    assert set(summary['failed']) == {r['name'] for r in reports.values() if not r['passed']}
    assert json.loads((args.out/'planned_cases.json').read_text()) == plan
    templates, logical = layout(args.bundle,args.catalog)
    manifest = prepare_model(args.model,templates,logical)
    model = DenseModel(args.model,manifest)
    routes = PrecisionRoutes(args.bundle,args.catalog,logical)
    audited = []; output_count = 0; passing_outputs = 0
    for group in plan:
        r = reports[group['name']]; out = args.out/group['name']
        assert r['identity'] == source and sha(out/'native.log') == r['log_sha256']
        assert json.loads((out/'report.json').read_text()) == r
        text = (out/'native.log').read_text()
        native = evidence(text,group,manifest['total_data_bytes'])
        for k,v in native.items():
            assert r[k] == json.loads(json.dumps(v)), k
        if r['passed']:
            assert r['simulator_returncode'] == 0 and r['output_packets'] == 16
            assert r['completed_cycles'] < r['maximum_cycles']
            assert all(r[k] for k in ['identities_verified','causal_timing_verified','native_masks_verified',
                'wire_ingress_verified','all_model_images_preloaded','all_sixteen_compute_verified','native_state_checks_passed'])
        n = 0; numeric_pass = 0
        for i,spec in enumerate(group['tokens']):
            record = r['tokens'][i]
            verify_fixture(out/(spec['name']+'.fixture'),spec)
            with tempfile.TemporaryDirectory() as tmp:
                rebuilt = fixture(spec,Path(tmp))
                assert all(record[k] == v for k,v in rebuilt.items())
            expected,shared_hash = reference(spec,model,logical,routes)
            raw = expected.astype('<u2').tobytes()
            assert hashlib.sha256(raw).hexdigest() == record['expected_sha256']
            assert raw == (out/(spec['name']+'.expected.bf16')).read_bytes()
            assert shared_hash == record['shared_reference_sha256']
            actual = out/f'actual.token{i}.bf16'
            if actual.exists():
                metrics = compare(actual,out/(spec['name']+'.expected.bf16'))
                assert all(record[k] == v for k,v in metrics.items())
                if r['passed']: assert metrics['passed']
                numeric_pass += int(metrics['passed'])
                n += 1
            else:
                assert not record['passed'] and not r['passed']
        peaks = [int(v) for v in re.findall(r'pcode peak compute users (\d+)',text)]
        cycles = [int(v) for v in re.findall(r'pcode cycle (\d+)',text)]
        logged_outputs = [int(v) for v in re.findall(r'pcode cycle \d+, output packets (\d+)',text)]
        errors = [line for line in text.splitlines() if 'assertion failed:' in line or ': Failure' in line]
        overlap_only = bool(r['simulator_returncode']==1 and n==numeric_pass==16
            and r['output_packets']==16 and text.count(': Failure\n')==1
            and 'Expected: (h.compute_overlap_cycle) > (0u), actual: 0 vs 0' in text)
        kind = ('PASS' if r['passed'] else 'COMPUTE_OCCUPANCY_TARGET_NOT_MET' if overlap_only
            else 'REDUCE_ORDER_ASSERT' if 'assertion failed: SameNet' in text
            else 'HOST_TIMEOUT' if r['simulator_returncode']==124
            else 'CYCLE_LIMIT' if r['completed_cycles'] and r['completed_cycles']>=r['maximum_cycles']
            else 'NUMERIC_OR_STATE_FAILURE')
        item = dict(name=r['name'],passed=r['passed'],outputs_audited=n,outputs_passed=numeric_pass,
                    classification=kind,functional_and_state_passed=bool(r['passed'] or overlap_only),
                    completed_cycles=r['completed_cycles'],last_logged_cycle=max(cycles,default=0),
                    last_logged_output_packets=max(logged_outputs,default=0),
                    peak_users_before_abort=max(peaks,default=r['peak_compute_users']),
                    peak_core_users=r['peak_core_users'],native_errors=errors)
        audited.append(item);output_count += n;passing_outputs += numeric_pass
        print(f'audited {r["name"]}: fixtures/references 16, actual verified {n}, passed {r["passed"]}',flush=True)
    controls=[]
    r=reports['shared_permutation_overlap'];b=args.out/r['name']
    if r['passed']:
        assert len({t['shared_reference_sha256'] for t in r['tokens'][:6]})==1
        for i in range(6):
            m=compare(b/f'actual.token{i+6}.bf16',b/f'actual.token{i}.bf16');assert m['passed']
            controls.append(dict(name=f'order_shared_ep{i}',**m))
        for i in range(1,6):
            m=shared_difference_check(b/'actual.token0.bf16',b/f'actual.token{i}.bf16',
                b/(r['tokens'][0]['name']+'.expected.bf16'),b/(r['tokens'][i]['name']+'.expected.bf16'))
            assert m['passed'];controls.append(dict(name=f'shared_0_vs_{i}',**m))
    r=reports['same_experts_contention'];b=args.out/r['name']
    if r['passed']:
        assert r['tokens'][0]['expected_sha256']==r['tokens'][1]['expected_sha256']
        m=compare(b/'actual.token1.bf16',b/'actual.token0.bf16');assert m['passed']
        controls.append(dict(name='identical_payload_distinct_uid',**m))
    result=dict(audited_at=datetime.now(timezone.utc).isoformat(),audit_passed=True,
        campaign_passed=summary['passed']==10,groups_passed=summary['passed'],groups_total=10,
        functional_groups_passed=sum(c['functional_and_state_passed'] for c in audited),
        fixtures_rebuilt=160,references_rebuilt=160,actual_outputs_audited=output_count,
        actual_outputs_passed=passing_outputs,
        controls=controls,cases=audited,source=source,auditor_sha256=sha(Path(__file__)),
        current_runner_sha256=current['runner_sha256'],
        executed_runner_archive=str(executed_runner) if executed_runner else None,
        summary_sha256=sha(args.out/'summary.json'))
    (args.out/'audit.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ['source','controls','cases']},indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bundle',type=Path,required=True)
    p.add_argument('--catalog',type=Path,required=True)
    p.add_argument('--model',type=Path,default=Path('build-ep6/glm5-dense-model'))
    p.add_argument('--simulator',type=Path,default=Path('build-ep6/pcode_bundle'))
    p.add_argument('--out',type=Path,default=Path('build-ep6/glm5-16-token'))
    p.add_argument('--executed-runner',type=Path,help='Immutable executed driver, for reporting-only driver updates')
    run(p.parse_args())
