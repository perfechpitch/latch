"""Fresh arithmetic and execution audit for both dynamic-VL GLM5 campaigns.

Unlike the historical two-token migration report, this audit does not reuse
regression logs tied to an older executable. Every oracle is rebuilt and every
native output, identity, EP mask and concurrency record is checked again.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess

from pcode_glm5_dense import (DenseModel, OFFSETS, compare, layout, prepare_model, reference,
                              sha, specifications)
from pcode_glm5_dense_verify import verify_fixture
from pcode_glm5_program_audit import audit_program
from pcode_glm5_relations import shared_difference_check
from pcode_glm5_two_token import identity, native_evidence, pairs
from pcode_precision_reference import PrecisionRoutes


def stats(tokens):
    nonzero = [t for t in tokens if not t['zero_reference']]
    return dict(outputs=len(tokens), minimum_nonzero_cosine=min(
        t['cosine_similarity'] for t in nonzero),
        maximum_relative_l2=max(t['relative_l2_error'] for t in tokens),
        bit_exact_outputs=sum(t['bf16_mismatches'] == 0 for t in tokens),
        zero_reference_outputs=sum(t['zero_reference'] for t in tokens))


def run(args):
    singles = specifications()
    doubles = pairs()
    one = json.loads((args.single/'summary.json').read_text())
    two = json.loads((args.two/'summary.json').read_text())
    for saved in (one, two):
        assert saved['total'] == saved['planned'] == saved['passed'] == 96
        assert not saved['failed']
    source = identity(args)
    assert two['identity'] == source
    for key in ('bundle_sha256', 'simulator_sha256', 'model_manifest_sha256'):
        assert one['identity'][key] == source[key]
    for key, filename in dict(campaign_sha256='pcode_glm5_dense.py',
                              reference_helpers_sha256='pcode_campaign.py',
                              precision_routes_sha256='pcode_precision_reference.py').items():
        assert one['identity'][key] == sha(Path(__file__).with_name(filename))
    templates, logical = layout(args.bundle, args.catalog)
    program = audit_program(args.bundle, args.catalog, templates)
    manifest = prepare_model(args.model, templates, logical)
    assert len(manifest['images']) == 768 and manifest['total_data_bytes'] == 9890168832
    assert sum(image['count'] for image in manifest['images']) == 33536
    shared_shards, shared_hashes = Counter(), {}
    for image in manifest['images']:
        ep = image['chip'] // 8
        assert image['count'] == OFFSETS[ep + 1] - OFFSETS[ep] + 1
        assert len(image['expert_hashes']) == image['count']
        key = image['stage'], image['chip'] % 8, image['slot']
        shared_shards[key] += 1
        assert shared_hashes.setdefault(key, image['expert_hashes'][0]) == image['expert_hashes'][0]
    assert len(shared_shards) == 128 and set(shared_shards.values()) == {6}
    model = DenseModel(args.model, manifest)
    routes = PrecisionRoutes(args.bundle, args.catalog, logical)
    meta = json.loads((args.catalog.parent.parent/'task_meta.json').read_text())
    count_tasks = 0
    for tasks in meta.values():
        by_id = {t['task_id']:t for t in tasks}
        for task in tasks:
            if task['op'] != 'silu_dot_quant':
                continue
            assert task['dynamic_vl'] and task['vl_formula'] == '(ep_counter + 1) * 256'
            predecessor = by_id[task['task_id']-1]
            assert predecessor['op'] == 'silu_dot_quant_count' and predecessor['unit'] == 'MU'
            count_tasks += 1
    assert count_tasks > 0

    def verify_token(directory, spec, saved, actual):
        base = directory/spec['name']
        verify_fixture(Path(str(base)+'.fixture'), spec)
        for key in ('user','token','counts','global_experts','expert_ids','topk_weights',
                    'routed_mask','shared_ep','actual_ep_mask'):
            assert saved[key] == spec[key], (spec['name'],key)
        assert sha(Path(str(base)+'.fixture')) == saved['fixture_sha256']
        expected, shared_hash = reference(spec, model, logical, routes)
        raw = expected.astype('<u2').tobytes()
        assert hashlib.sha256(raw).hexdigest() == saved['expected_sha256']
        assert shared_hash == saved['shared_reference_sha256']
        expected_path = Path(str(base)+'.expected.bf16')
        assert raw == expected_path.read_bytes()
        assert sha(actual) == saved['actual_sha256']
        metrics = compare(actual, expected_path)
        assert metrics['passed'] and all(saved[k] == v for k,v in metrics.items())
        return dict(name=spec['name'], user=spec['user'], token=spec['token'],
            counts=spec['counts'], routed_mask=spec['routed_mask'], shared_ep=spec['shared_ep'],
            actual_ep_mask=spec['actual_ep_mask'], expected_sha256=saved['expected_sha256'],
            shared_reference_sha256=shared_hash, **metrics)

    single_by_name = {r['name']:r for r in one['cases']}
    assert set(single_by_name) == {s['name'] for s in singles}
    verified_one = []
    for i,spec in enumerate(singles):
        old = single_by_name[spec['name']]
        assert old['passed'] and old['simulator_returncode'] == 0 and old['output_packets'] == 1
        assert old['native_mask_verified'] and old['all_model_images_preloaded']
        assert all(old[k] == v for k,v in one['identity'].items())
        text = (args.single/(spec['name']+'.log')).read_text()
        masks = re.findall(r'pcode mask routed (\d+), shared EP (\d+), actual (\d+)',text)
        assert masks == [(str(spec['routed_mask']),str(spec['shared_ep']),str(spec['actual_ep_mask']))]
        assert '[  PASSED  ] 1 test.' in text and '[  FAILED  ]' not in text
        row = verify_token(args.single,spec,old,args.single/(spec['name']+'.actual.bf16'))
        verified_one.append(dict(row,completed_cycles=old['completed_cycles']))
        print(f'fresh single {i+1}/96 {spec["name"]}',flush=True)
    relations = []
    for group in ('order_0','order_1','order_2','shared_replicas'):
        members = [s for s in singles if s.get('relation') == group]
        assert len(members) == (6 if group == 'shared_replicas' else 2)
        first = args.single/members[0]['name']
        for member in members[1:]:
            other = args.single/member['name']
            if group == 'shared_replicas':
                metrics = shared_difference_check(Path(str(first)+'.actual.bf16'),
                    Path(str(other)+'.actual.bf16'),Path(str(first)+'.expected.bf16'),
                    Path(str(other)+'.expected.bf16'))
            else:
                metrics = compare(Path(str(other)+'.actual.bf16'),Path(str(first)+'.actual.bf16'))
            assert metrics['passed']
            relations.append(dict(group=group,**metrics))

    double_by_name = {r['name']:r for r in two['cases']}
    assert set(double_by_name) == {p['name'] for p in doubles}
    verified_two, double_tokens, concurrent_vs_single = [], [], []
    for i,pair in enumerate(doubles):
        old = double_by_name[pair['name']]
        assert old['identity'] == source and old['passed'] and old['simulator_returncode'] == 0
        log = args.two/(pair['name']+'.log')
        assert sha(log) == old['log_sha256']
        evidence = native_evidence(log.read_text(),pair,manifest['total_data_bytes'],old['injection_order'])
        assert evidence['output_packets'] == 2
        assert all(evidence[k] for k in ('overlapped','simultaneous_compute_users_verified',
            'native_masks_verified','all_model_images_preloaded','native_state_checks_passed'))
        tokens = []
        for j,spec in enumerate(pair['tokens']):
            actual = args.two/(pair['name']+f'.actual.token{j}.bf16')
            tokens.append(verify_token(args.two,spec,old['tokens'][j],actual))
            if pair['family'] == 'all_experts':
                block = 2*int(pair['name'].split('_')[-1])+j
                name = f'experts_{8*block:03d}_{8*block+7:03d}'
                assert single_by_name[name]['expected_sha256'] == tokens[-1]['expected_sha256']
                metrics = compare(actual,args.single/(name+'.actual.bf16'))
                assert metrics['passed']
                concurrent_vs_single.append(dict(pair=pair['name'],token=j,single_case=name,**metrics))
        if pair['name'] == 'contention_identical_payload':
            metrics = compare(args.two/(pair['name']+'.actual.token0.bf16'),
                              args.two/(pair['name']+'.actual.token1.bf16'))
            assert metrics['passed']
        double_tokens.extend(tokens)
        verified_two.append(dict(name=pair['name'],family=pair['family'],tokens=tokens,
            gap=old['gap'],injection_order=old['injection_order'],
            timing_alignment_attempts=len(old['timing_alignment_attempts']),**evidence))
        print(f'fresh double {i+1}/96 {pair["name"]}',flush=True)
    assert len(concurrent_vs_single) == 32
    bach = args.bundle.resolve().parents[3]
    latch = Path(__file__).resolve().parents[2]
    changed_sources = [(bach,p) for p in (bach/'pcode/src/bachflow_pcode/ops/vector.py',
        bach/'pcode/src/bachflow_pcode/scenarios/glm5_ep6_tp8.py')]
    changed_sources += [(latch,latch/p) for p in ('src/bach/ip/chip/core/mu/gen_ep_info.h',
        'src/bach/ip/chip/core/mu/regfile.h','src/bach/ip/chip/core/mu/mu.h',
        'src/bach/ip/chip/core/rv_core/rv_ports.h','src/bach/ip/chip/core/rv_core/exec.h')]
    versions = {}
    for name,root in (('bachflow',bach),('latch',latch)):
        def git(*parameters):
            return subprocess.check_output(['git','-C',str(root),*parameters],text=True).strip()
        assert git('branch','--show-current') == 'pcode_dev_zlm'
        versions[name] = dict(branch='pcode_dev_zlm',base_commit=git('rev-parse','HEAD'))
    result = dict(status='passed',verified_at=datetime.now(timezone.utc).isoformat(),
        identity=source,versions=versions,verifier_sha256=sha(__file__),program_audit=program,
        model_audit=dict(images=768,routed_experts=256,shared_replicas=6,
            shared_identical=True,expert_shards=33536,data_bytes=manifest['total_data_bytes']),
        configured_dynamic_vu_tasks=count_tasks,
        source_sha256={str(path.relative_to(root)):sha(path) for root,path in changed_sources},
        single=dict(total=96,passed=96,coverage=dict(Counter(s['family'] for s in singles)),
                    statistics=stats(verified_one),cases=verified_one,relations=relations),
        double=dict(total=96,passed=96,coverage=dict(Counter(p['family'] for p in doubles)),
                    statistics=dict(stats(double_tokens),
                        timing_alignment_retry_pairs=sum(bool(r['timing_alignment_attempts']) for r in verified_two),
                        reversed_injection_pairs=sum(r['injection_order']==[1,0] for r in verified_two)),
                    cases=verified_two),
        concurrent_vs_single=concurrent_vs_single)
    args.report.write_text(json.dumps(result,indent=2)+'\n')
    lines=['GLM5 EP6+TP8 dynamic TYPE_VL: 96 single + 96 two-token cases PASS',
           'All 288 outputs independently rebuilt and verified; shared slot retained.',
           f'Verified: {result["verified_at"]}',
           f'Single: {result["single"]["statistics"]}',f'Double: {result["double"]["statistics"]}',
           'case | user | routed mask | shared EP | actual EP mask | counts | cosine | relative L2']
    for row in verified_one+double_tokens:
        lines.append(f'{row["name"]} | {row["user"]} | 0x{row["routed_mask"]:02x} | '
            f'{row["shared_ep"]} | 0x{row["actual_ep_mask"]:02x} | {row["counts"]} | '
            f'{row["cosine_similarity"]} | {row["relative_l2_error"]:.9g}')
    args.report.with_suffix('.txt').write_text('\n'.join(lines)+'\n')
    print(json.dumps({k:result[k]['statistics'] for k in ('single','double')},indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle',type=Path,required=True)
    parser.add_argument('--catalog',type=Path,required=True)
    parser.add_argument('--single',type=Path,required=True)
    parser.add_argument('--two',type=Path,required=True)
    parser.add_argument('--model',type=Path,default=Path('build-ep6/glm5-dense-model'))
    parser.add_argument('--simulator',type=Path,default=Path('build-ep6/pcode_bundle'))
    parser.add_argument('--report',type=Path,default=Path('test/bach/pcode_dynamic_vl_results.json'))
    run(parser.parse_args())
