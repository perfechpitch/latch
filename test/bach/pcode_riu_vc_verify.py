"""Fresh GLM5 arithmetic/state audit with original planner VCs and RIU mapping.

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
from pcode_glm5_two_token import identity, native_evidence, pairs, alignment_only_failure
from pcode_precision_reference import PrecisionRoutes


def audit_alignment_trial(trial, directory, pair, model_bytes, expected_identity):
    log=directory/trial['log_file']
    assert sha(log)==trial['log_sha256'] and trial['identity']==expected_identity
    text=log.read_text()
    assert alignment_only_failure(trial,text)
    evidence=native_evidence(text,pair,model_bytes,trial['injection_order'])
    # Overall GTest FAIL is expected: exactly one missing-overlap assertion.
    # alignment_only_failure rejects any additional numerical/state failure.
    assert evidence['output_packets']==2 and evidence['overlapped']
    assert evidence['native_masks_verified'] and evidence['all_model_images_preloaded']
    for key,value in evidence.items():
        recorded=trial[key]
        if isinstance(recorded,dict): recorded={int(k):v for k,v in recorded.items()}
        assert recorded==value,(pair['name'],key)
    for j,filename in enumerate(trial['actual_files']):
        actual=directory/filename
        assert sha(actual)==trial['tokens'][j]['actual_sha256']
        expected=directory/(pair['tokens'][j]['name']+'.expected.bf16')
        assert compare(actual,expected)['passed']


def audit_bundle_delta(experiment_root, bundle):
    before=experiment_root/'before_bundle/pcode_ep6_tp8'
    old=[line.split() for line in (before/'pcode_ep6_tp8.bachir').read_text().splitlines()]
    new=[line.split() for line in (bundle/'pcode_ep6_tp8.bachir').read_text().splitlines()]
    assert len(old)==len(new)
    changes=Counter()
    for left,right in zip(old,new):
        assert bool(left)==bool(right)
        if not left: continue
        assert left[0]==right[0]
        span={'RTAB':(6,12),'TSRTAB':(5,6)}.get(left[0])
        if span:
            start,stop=span
            assert left[:start]+left[stop:]==right[:start]+right[stop:]
            changes[left[0]]+=left[start:stop]!=right[start:stop]
        else: assert left==right
    references={token for row in new for token in row if token.endswith('.hex')}
    assert len(references)==855 and all(sha(before/name)==sha(bundle/name) for name in references)
    assert dict(changes)=={'RTAB':1153,'TSRTAB':1057}
    sources=json.loads((experiment_root/'generated_selected_sources.json').read_text())
    assert sources['count']==1278
    for name,row in sources['files'].items():
        assert sha(experiment_root/'generated_snapshot'/name)==row['sha256']
    return dict(changed_vc_records=dict(changes),all_non_vc_fields_and_records_identical=True,
        referenced_hex_images_identical=855,selected_generated_sources_checked=1278,
        selected_source_manifest_sha256=sha(experiment_root/'generated_selected_sources.json'))


def audit_controls(args, boundary_pair, model_bytes, selected=None):
    """Audit auxiliary native logs and pin the archived binaries themselves."""
    root=args.experiment_root.resolve()
    frozen_binaries={
        'before_simulator':'cfa0c4b7c3e42cc4259667095745c0851d1de74187a50415d7cf8f0422599266',
        'historical_simulator':'8efe4539a0bd2b04a7ee82c5d14a70cc5eb5c58ee3cdb8cbc0d2306a372c237e',
        'after_simulator':'075c9391d666f5b6aef022a163ccef69196332ae0b8f0ab2ba992ff90826c36d'}
    for name,digest in frozen_binaries.items(): assert sha(root/name)==digest
    if getattr(args,'candidate_source',None) is None:
        assert sha(args.simulator)==frozen_binaries['after_simulator']
    variants={
        'dynamic_raw_before':('riu-vc-before-boundary15','before_simulator','planner_bundle'),
        'dynamic_raw_after':('riu-vc-after-boundary15','after_simulator','planner_bundle'),
        'dynamic_exact_e2_inverse_before':('riu-vc-exact-e2-inverse-boundary15','before_simulator','e2_inverse_bundle'),
        'historical_original_before':('riu-vc-historical-boundary15','historical_simulator','historical_bundle'),
        'historical_raw_after':('riu-vc-historical-planner-after-boundary15','after_simulator','historical_planner_bundle'),
        'historical_raw_before':('riu-vc-historical-planner-before-boundary15','historical_simulator','historical_planner_bundle'),
        'historical_raw_direct_before':('riu-vc-historical-planner-direct-before-boundary15','before_simulator','historical_planner_bundle')}
    results={}
    for name,(directory,binary,bundle) in variants.items():
        if selected is not None and name not in selected: continue
        out=args.two.parent/directory
        saved=json.loads((out/'summary.json').read_text())
        assert saved['total']==saved['planned']==1
        row=saved['cases'][0];assert row['name']==boundary_pair['name']
        assert row['identity']['simulator_sha256']==frozen_binaries[binary]
        control_args=argparse.Namespace(**vars(args))
        control_args.bundle=root/bundle/'pcode_ep6_tp8';control_args.simulator=root/binary
        assert row['identity']==identity(control_args)
        log=out/(row['name']+'.log');assert sha(log)==row['log_sha256']
        text=log.read_text()
        evidence=native_evidence(text,boundary_pair,model_bytes,row['injection_order'])
        assert evidence['native_masks_verified'] and evidence['all_model_images_preloaded']
        for j,spec in enumerate(boundary_pair['tokens']):
            fixture=out/(spec['name']+'.fixture');verify_fixture(fixture,spec)
            assert sha(fixture)==row['tokens'][j]['fixture_sha256']
            expected=args.two/(spec['name']+'.expected.bf16')
            assert sha(expected)==row['tokens'][j]['expected_sha256']
            if row['passed']:
                actual=out/(row['name']+f'.actual.token{j}.bf16')
                assert sha(actual)==row['tokens'][j]['actual_sha256']
                assert compare(actual,expected)['passed']
        if row['passed']:
            assert row['simulator_returncode']==0 and evidence['output_packets']==2
            assert evidence['native_state_checks_passed'] and evidence['overlapped']
            assert evidence['simultaneous_compute_users_verified']
        else:
            # Preserve the historical failure as a failure, not an alignment retry.
            assert name in {'historical_original_before','historical_raw_before','historical_raw_direct_before'}
            assert row['simulator_returncode']==1
            assert evidence['completed_cycles']==16000 and evidence['output_packets']==0
            assert not evidence['native_state_checks_passed']
            assert 'private credit 20 shared credit 20' in text
            assert 'reduce chip 23 core 2 user 195 path 2' in text
        results[name]=dict(passed=row['passed'],identity=row['identity'],log_sha256=sha(log),**evidence)
    return dict(binary_sha256=frozen_binaries,controls=results)


def audit_candidate_controls(args, model_bytes):
    """Pin candidate1 counterexamples and candidate2 minimum controls separately."""
    if args.candidate_source is None: return None
    root=args.experiment_root.resolve();latch=Path(__file__).resolve().parents[2]
    final_sim=args.simulator.resolve();candidate2_sim=root/'candidate2/simulator';results={}
    pair_by_name={p['name']:p for p in pairs()}
    variants=[
        ('candidate1_raw_expert14','riu-vc-diagnostic-expert-blocks-14','expert_blocks_14',
         root/'after_simulator','planner_bundle',False),
        ('candidate1_exact_e2_inverse_expert14','riu-vc-exact-e2-inverse-after-expert14','expert_blocks_14',
         root/'after_simulator','e2_inverse_bundle',False),
        ('candidate2_raw_expert14','riu-vc-candidate2-expert14','expert_blocks_14',
         candidate2_sim,'planner_bundle',True),
        ('candidate2_raw_boundary15','riu-vc-candidate2-boundary15','boundary_pair_15',
         candidate2_sim,'planner_bundle',True),
        ('candidate2_historical_raw_boundary15','riu-vc-candidate2-historical-boundary15','boundary_pair_15',
         candidate2_sim,'historical_planner_bundle',True)]
    if args.candidate_source.resolve().parent.name=='candidate3':
        variants += [
            ('candidate3_raw_expert14','riu-vc-candidate3-expert14','expert_blocks_14',
             final_sim,'planner_bundle',True),
            ('candidate3_raw_boundary15','riu-vc-candidate3-boundary15','boundary_pair_15',
             final_sim,'planner_bundle',True),
            ('candidate3_historical_raw_boundary15','riu-vc-candidate3-historical-boundary15','boundary_pair_15',
             final_sim,'historical_planner_bundle',True)]
    for label,directory,name,binary,bundle,should_pass in variants:
        out=latch/'build-ep6'/directory;pair=pair_by_name[name]
        row=json.loads((out/(name+'.report.json')).read_text())
        control_args=argparse.Namespace(**vars(args));control_args.simulator=binary
        control_args.bundle=root/bundle/'pcode_ep6_tp8'
        assert row['identity']==identity(control_args)
        assert row['passed']==should_pass and row['maximum_cycles']==16000
        text=(out/(name+'.log')).read_text();assert sha(out/(name+'.log'))==row['log_sha256']
        evidence=native_evidence(text,pair,model_bytes,row['injection_order'])
        for key,value in evidence.items():
            recorded=row[key]
            if isinstance(recorded,dict):recorded={int(k):v for k,v in recorded.items()}
            assert recorded==value,(label,key)
        assert evidence['native_masks_verified'] and evidence['all_model_images_preloaded']
        for j,spec in enumerate(pair['tokens']):
            fixture=out/(spec['name']+'.fixture');verify_fixture(fixture,spec)
            assert sha(fixture)==row['tokens'][j]['fixture_sha256']
            expected=args.two/(spec['name']+'.expected.bf16')
            assert sha(expected)==row['tokens'][j]['expected_sha256']
            if should_pass:
                actual=out/(name+f'.actual.token{j}.bf16')
                assert sha(actual)==row['tokens'][j]['actual_sha256'] and compare(actual,expected)['passed']
        if should_pass:
            assert row['simulator_returncode']==0 and evidence['output_packets']==2
            assert all(evidence[k] for k in ('native_state_checks_passed','overlapped',
                'simultaneous_compute_users_verified'))
        else:
            assert row['simulator_returncode']==1 and evidence['completed_cycles']==16000
            assert evidence['output_packets']==0 and not evidence['native_state_checks_passed']
            assert 'path 34' in text and 'private credit 20 shared credit 20' in text
        results[label]=dict(passed=row['passed'],identity=row['identity'],
            log_sha256=sha(out/(name+'.log')),**evidence)
    return results


def audit_packet_owner_controls(args):
    if args.candidate_source is None or args.candidate_source.resolve().parent.name!='candidate3':
        return None
    candidate=args.candidate_source.resolve().parent
    saved=json.loads((candidate/'unit_owner_controls.json').read_text())
    for name in ('unit_owner_before.log','unit_owner_after.log'):
        assert sha(candidate/name)==saved[name]['sha256']
        assert (candidate/name).read_text()==saved[name]['text']
    before=saved['unit_owner_before.log']['text'];after=saved['unit_owner_after.log']['text']
    assert 'Which is: { 100, 200, 100 }' in before and '[  FAILED  ] 1 test' in before
    assert '[  PASSED  ] 1 test.' in after and '[  FAILED  ]' not in after
    assert '100% tests passed, 0 tests failed out of 8' in (candidate/'router_tests.log').read_text()
    assert sha(candidate/'router_tests.log')==saved['router_test_log_sha256'] and saved['full_router_tests']==104
    for name,digest in saved['binary_sha256'].items():assert sha(candidate/name)==digest
    return dict(negative_probe_failed_as_expected=True,source_vc_owner_probe_passed=True,
        router_tests=104,executables=8,control_manifest_sha256=sha(candidate/'unit_owner_controls.json'),
        binary_sha256=saved['binary_sha256'],before_log_sha256=saved['unit_owner_before.log']['sha256'],
        after_log_sha256=saved['unit_owner_after.log']['sha256'])


def audit_actual(actual, expected, saved, require_correct):
    """Check available bytes without turning native failure/missing output into PASS."""
    if not actual.exists():
        assert not require_correct and not saved['passed']
        assert 'actual_sha256' not in saved
        return dict(passed=False,actual_available=False,error=saved.get('error','output missing'))
    digest=sha(actual)
    if 'actual_sha256' in saved: assert digest==saved['actual_sha256']
    try: metrics=compare(actual,expected)
    except (OSError,AssertionError,ValueError) as error:
        assert not require_correct and not saved['passed']
        return dict(passed=False,actual_available=True,actual_sha256=digest,
                    invalid_output=True,error=str(error))
    for key,value in metrics.items():
        if key in saved and key!='passed': assert saved[key]==value,(actual.name,key)
    if require_correct: assert metrics['passed'] and saved['passed']
    return dict(metrics,actual_available=True)


def failure_details(text, returncode):
    stop=re.search(r'pcode stopped at (\d+), output packets (\d+)',text)
    progress=re.findall(r'pcode cycle (\d+), output packets (\d+)',text)
    kind='host_timeout' if returncode==124 else 'native_or_functional_failure'
    if stop and int(stop[2])==0: kind='cycle_limit_zero_output'
    excerpt=[line for line in text.splitlines() if any(word in line for word in
        ('Failure','reduce chip','queued xbar','xbar owner','pending compute','pending chip'))]
    return dict(kind=kind,last_progress_cycle=int(progress[-1][0]) if progress else None,
                stopped_cycle=int(stop[1]) if stop else None,
                diagnostic_lines=excerpt[:80],diagnostic_line_count=len(excerpt))


def stats(tokens):
    available=[t for t in tokens if t['actual_available']]
    numeric=[t for t in available if 'relative_l2_error' in t]
    nonzero=[t for t in numeric if not t['zero_reference'] and t['cosine_similarity'] is not None]
    return dict(outputs_expected=len(tokens),outputs_available=len(available),
        outputs_missing=len(tokens)-len(available),arithmetic_passed_outputs=sum(t['passed'] for t in tokens),
        invalid_outputs=sum(t.get('invalid_output',False) for t in tokens),
        minimum_nonzero_cosine=min((t['cosine_similarity'] for t in nonzero),default=None),
        maximum_relative_l2=max((t['relative_l2_error'] for t in numeric),default=None),
        bit_exact_outputs=sum(t.get('bf16_mismatches')==0 for t in available),
        zero_reference_outputs=sum(t['zero_reference'] for t in tokens))


def run(args):
    singles = specifications()
    doubles = pairs()
    one = json.loads((args.single/'summary.json').read_text())
    two = json.loads((args.two/'summary.json').read_text())
    for saved in (one, two):
        assert saved['total'] == saved['planned'] == 96
        assert saved['passed']==sum(row['passed'] for row in saved['cases'])
        assert set(saved['failed'])=={row['name'] for row in saved['cases'] if not row['passed']}
    source = identity(args)
    collection_audits={}
    for campaign,directory in (('single',args.single),('double',args.two)):
        manifest_path=directory/'case_sources.json'
        if not manifest_path.exists(): continue
        collection=json.loads(manifest_path.read_text())
        for name,row in collection['cases'].items():
            origin=Path(row['source_directory'])
            for filename,digest in row['files'].items():
                assert sha(directory/filename)==sha(origin/filename)==digest,(name,filename)
        for row in collection.get('original_summaries',[]):
            assert sha(Path(row['path']))==row['sha256']
        collection_audits[campaign]=dict(manifest_sha256=sha(manifest_path),
            cases=len(collection['cases']),original_artifacts_unchanged=True)
    bundle_delta=audit_bundle_delta(args.experiment_root,args.bundle)
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
    records=[line.split() for line in (args.bundle/'pcode_ep6_tp8.bachir').read_text().splitlines()]
    native_routes={tuple(map(int,row[1:4])):row for row in records if row and row[0]=='RTAB'}
    catalog=json.loads(args.catalog.read_text())
    checked_vcs=0
    for row in records:
        if not row or row[0]!='CHIP': continue
        chip,mask=int(row[1]),int(row[2],0)
        selected=next(h for _,h in sorted(catalog['chips'][f'c{chip}']['harvests'].items(),
            key=lambda item:int(item[0][1:])) if h['disable_mask']==mask)
        for core_id,core in selected['cores'].items():
            for entry in core.get('entries',[]):
                if not int(entry.get('VALID',1)): continue
                native=native_routes[chip,int(core_id[1:]),int(entry['PathID'])]
                assert set(map(int,native[6:12]))=={int(entry.get('nxtVC',0))}
                checked_vcs+=1
    assert checked_vcs>0
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

    def verify_token(directory, spec, saved, actual, require_correct=True):
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
        metrics=audit_actual(actual,expected_path,saved,require_correct)
        metrics.setdefault('zero_reference',bool(((expected & 0x7fff)==0).all()))
        return dict(name=spec['name'], user=spec['user'], token=spec['token'],
            counts=spec['counts'], routed_mask=spec['routed_mask'], shared_ep=spec['shared_ep'],
            actual_ep_mask=spec['actual_ep_mask'], expected_sha256=saved['expected_sha256'],
            shared_reference_sha256=shared_hash, **metrics)

    single_by_name = {r['name']:r for r in one['cases']}
    assert set(single_by_name) == {s['name'] for s in singles}
    verified_one = []
    failures=[]
    for i,spec in enumerate(singles):
        old = single_by_name[spec['name']]
        assert all(old[k] == v for k,v in one['identity'].items())
        log=args.single/(spec['name']+'.log');text=log.read_text()
        masks = re.findall(r'pcode mask routed (\d+), shared EP (\d+), actual (\d+)',text)
        masks_valid=masks==[(str(spec['routed_mask']),str(spec['shared_ep']),str(spec['actual_ep_mask']))]
        preloads=re.findall(r'pcode dense model images (\d+), data bytes (\d+)',text)
        assert masks_valid==old['native_mask_verified']
        assert (preloads==[('768',str(manifest['total_data_bytes']))])==old['all_model_images_preloaded']
        native_passed='[  PASSED  ] 1 test.' in text and '[  FAILED  ]' not in text
        row = verify_token(args.single,spec,old,args.single/(spec['name']+'.actual.bf16'),old['passed'])
        successful=(old['simulator_returncode']==0 and old.get('output_packets')==1 and native_passed
                    and masks_valid and old['all_model_images_preloaded'] and row['passed'])
        assert bool(old['passed'])==bool(successful)
        verified_one.append(dict(row,case_passed=old['passed'],completed_cycles=old.get('completed_cycles'),
            simulator_returncode=old['simulator_returncode'],native_state_checks_passed=native_passed,
            log_sha256=sha(log)))
        if not old['passed']:
            failures.append(dict(campaign='single',name=spec['name'],log_sha256=sha(log),
                simulator_returncode=old['simulator_returncode'],**failure_details(text,old['simulator_returncode'])))
        print(f'fresh single {i+1}/96 {spec["name"]}: {"PASS" if old["passed"] else "FAIL preserved"}',flush=True)
    relations = []
    for group in ('order_0','order_1','order_2','shared_replicas'):
        members = [s for s in singles if s.get('relation') == group]
        assert len(members) == (6 if group == 'shared_replicas' else 2)
        first = args.single/members[0]['name']
        for member in members[1:]:
            other = args.single/member['name']
            if not Path(str(first)+'.actual.bf16').exists() or not Path(str(other)+'.actual.bf16').exists():
                relations.append(dict(group=group,status='unavailable_due_to_missing_output'));continue
            try:
                if group == 'shared_replicas':
                    metrics = shared_difference_check(Path(str(first)+'.actual.bf16'),
                        Path(str(other)+'.actual.bf16'),Path(str(first)+'.expected.bf16'),
                        Path(str(other)+'.expected.bf16'))
                else:
                    metrics = compare(Path(str(other)+'.actual.bf16'),Path(str(first)+'.actual.bf16'))
            except (OSError,AssertionError,ValueError) as error:
                metrics=dict(passed=False,error=str(error))
            if all(single_by_name[m['name']]['passed'] for m in (members[0],member)): assert metrics['passed']
            relations.append(dict(group=group,**metrics))

    double_by_name = {r['name']:r for r in two['cases']}
    assert set(double_by_name) == {p['name'] for p in doubles}
    verified_two, double_tokens, concurrent_vs_single = [], [], []
    for i,pair in enumerate(doubles):
        old = double_by_name[pair['name']]
        assert old['identity'] == source
        log = args.two/(pair['name']+'.log');text=log.read_text()
        assert sha(log) == old['log_sha256']
        evidence = native_evidence(text,pair,manifest['total_data_bytes'],old['injection_order'])
        for key,value in evidence.items():
            recorded=old[key]
            if isinstance(recorded,dict): recorded={int(k):v for k,v in recorded.items()}
            assert recorded==value,(pair['name'],key)
        tokens = []
        for j,spec in enumerate(pair['tokens']):
            actual = args.two/(pair['name']+f'.actual.token{j}.bf16')
            tokens.append(verify_token(args.two,spec,old['tokens'][j],actual,old['passed']))
            if pair['family'] == 'all_experts':
                block = 2*int(pair['name'].split('_')[-1])+j
                name = f'experts_{8*block:03d}_{8*block+7:03d}'
                assert single_by_name[name]['expected_sha256'] == tokens[-1]['expected_sha256']
                single_actual=args.single/(name+'.actual.bf16')
                if actual.exists() and single_actual.exists():
                    try: metrics = compare(actual,single_actual)
                    except (OSError,AssertionError,ValueError) as error: metrics=dict(passed=False,error=str(error))
                    if old['passed'] and single_by_name[name]['passed']: assert metrics['passed']
                    concurrent_vs_single.append(dict(pair=pair['name'],token=j,single_case=name,**metrics))
                else:
                    concurrent_vs_single.append(dict(pair=pair['name'],token=j,single_case=name,
                        status='unavailable_due_to_missing_output'))
        successful=(old['simulator_returncode']==0 and evidence['output_packets']==2
            and all(evidence[k] for k in ('overlapped','simultaneous_compute_users_verified',
                'native_masks_verified','all_model_images_preloaded','native_state_checks_passed'))
            and all(token['passed'] for token in tokens))
        assert bool(old['passed'])==bool(successful)
        if pair['name'] == 'contention_identical_payload':
            paths=[args.two/(pair['name']+f'.actual.token{j}.bf16') for j in range(2)]
            if all(path.exists() for path in paths):
                try: metrics=compare(*paths)
                except (OSError,AssertionError,ValueError) as error: metrics=dict(passed=False,error=str(error))
                if old['passed']: assert metrics['passed']
        for trial in old['timing_alignment_attempts']:
            audit_alignment_trial(trial,args.two,pair,manifest['total_data_bytes'],source)
        double_tokens.extend(tokens)
        verified_two.append(dict(name=pair['name'],family=pair['family'],tokens=tokens,
            passed=old['passed'],simulator_returncode=old['simulator_returncode'],log_sha256=sha(log),
            gap=old['gap'],injection_order=old['injection_order'],
            timing_alignment_attempts=len(old['timing_alignment_attempts']),**evidence))
        if not old['passed']:
            failures.append(dict(campaign='double',name=pair['name'],identity=old['identity'],
                log_sha256=sha(log),simulator_returncode=old['simulator_returncode'],**evidence,
                **failure_details(text,old['simulator_returncode'])))
        print(f'fresh double {i+1}/96 {pair["name"]}: {"PASS" if old["passed"] else "FAIL preserved"}',flush=True)
    assert len(concurrent_vs_single) == 32
    controls=audit_controls(args,next(p for p in doubles if p['name']=='boundary_pair_15'),manifest['total_data_bytes'])
    assert len(controls['controls'])==7
    candidate_controls=audit_candidate_controls(args,manifest['total_data_bytes'])
    packet_owner_controls=audit_packet_owner_controls(args)
    bach = args.pcode_root.resolve()
    latch = Path(__file__).resolve().parents[2]
    source_root=args.candidate_source.resolve() if args.candidate_source else latch
    source_paths=['src/bach/ip/chip/core/router/'+name for name in
        ('router_table.h','router.h','xbar.h','reduce_module.h')]
    if args.candidate_source: source_paths += ['src/bach/ip/chip/core/router/'+name for name in
        ('router_ports.h','router_station.h')]
    changed_sources = [(bach,bach/'pcode/src/bachflow_pcode/latch_export.py')]
    changed_sources += [(source_root,source_root/p) for p in source_paths]
    versions = {}
    for name,root in (('bachflow',bach),('latch',latch)):
        def git(*parameters):
            return subprocess.check_output(['git','-C',str(root),*parameters],text=True).strip()
        assert git('branch','--show-current') == 'pcode_dev_zlm'
        versions[name] = dict(branch='pcode_dev_zlm',base_commit=git('rev-parse','HEAD'))
    protected=json.loads((args.experiment_root/'initial_identity.json').read_text())['BachFlow-ep6-single-token']['files']
    protected_actual={relative:sha(bach/relative) for relative in protected}
    assert protected_actual==protected
    source_hashes={str(path.relative_to(root)):sha(path) for root,path in changed_sources}
    candidate_source_audit=None
    if args.candidate_source:
        candidate=args.candidate_source.resolve().parent
        candidate_identity=json.loads((candidate/'identity.json').read_text())
        assert sha(args.simulator)==candidate_identity['simulator_sha256']
        initial=json.loads((candidate/'initial_source_manifest.json').read_text())
        for name,old_digest in initial.items():
            expected=candidate_identity['changed'].get(name,{}).get('after',old_digest)
            assert sha(source_root/name)==expected,(name,'candidate source changed')
        expected_sources={name:sha(source_root/name) for name in source_paths}
        expected_sources['pcode/src/bachflow_pcode/latch_export.py']=next(
            row['sha256'] for row in json.loads((args.experiment_root/'source_snapshot_manifest.json').read_text())
            if row['path']=='pcode/src/bachflow_pcode/latch_export.py')
        assert source_hashes==expected_sources
        # Promote only the already tested source, then audit the exact files
        # that remain in the user repositories against the frozen candidate.
        assert all(sha(latch/name)==digest for name,digest in expected_sources.items()
                   if name.startswith('src/')),'working repository differs from tested candidate'
        assert all(sha(latch/name)==row['after'] for name,row in candidate_identity['changed'].items())
        source_manifest_digest=sha(candidate/'identity.json')
        candidate_source_audit=dict(source_root=str(source_root),files_checked=len(initial),
            initial_manifest_sha256=sha(candidate/'initial_source_manifest.json'),
            identity_sha256=source_manifest_digest,working_execution_sources_match=True,
            changed_files=candidate_identity['changed'])
    else:
        frozen_sources=json.loads((args.experiment_root/'source_snapshot_manifest.json').read_text())
        assert source_hashes=={row['path']:row['sha256'] for row in frozen_sources}
        for row in frozen_sources:
            snapshot=args.experiment_root/'source_snapshot/after'/row['repository']/row['path']
            assert sha(snapshot)==row['sha256']
        source_manifest_digest=sha(args.experiment_root/'source_snapshot_manifest.json')
    functional_passed=one['passed']==two['passed']==96
    result = dict(status='passed' if functional_passed else 'failed',audit_completed=True,
        regression_failures=failures,verified_at=datetime.now(timezone.utc).isoformat(),
        identity=source,versions=versions,verifier_sha256=sha(__file__),program_audit=program,
        auxiliary_controls=controls,candidate_controls=candidate_controls,packet_owner_controls=packet_owner_controls,bundle_delta_audit=bundle_delta,collection_audits=collection_audits,
        model_audit=dict(images=768,routed_experts=256,shared_replicas=6,
            shared_identical=True,expert_shards=33536,data_bytes=manifest['total_data_bytes']),
        original_planner_vc_entries_checked=checked_vcs,
        configured_dynamic_vu_tasks=count_tasks,
        source_sha256=source_hashes,
        protected_bachflow_files_unchanged=protected_actual,
        source_snapshot_manifest_sha256=source_manifest_digest,candidate_source_audit=candidate_source_audit,
        single=dict(total=96,passed=one['passed'],failed=one['failed'],coverage=dict(Counter(s['family'] for s in singles)),
                    statistics=stats(verified_one),cases=verified_one,relations=relations),
        double=dict(total=96,passed=two['passed'],failed=two['failed'],coverage=dict(Counter(p['family'] for p in doubles)),
                    statistics=dict(stats(double_tokens),
                        timing_alignment_retry_pairs=sum(bool(r['timing_alignment_attempts']) for r in verified_two),
                        reversed_injection_pairs=sum(r['injection_order']==[1,0] for r in verified_two)),
                    cases=verified_two),
        concurrent_vs_single=concurrent_vs_single)
    args.report.write_text(json.dumps(result,indent=2)+'\n')
    lines=[f'GLM5 EP6+TP8 original planner VCs + RIU: {result["status"].upper()}',
           f'Single {one["passed"]}/96; double {two["passed"]}/96. All 288 oracles rebuilt; available outputs audited.',
           f'Verified: {result["verified_at"]}',
           f'Single: {result["single"]["statistics"]}',f'Double: {result["double"]["statistics"]}',
           'case | user | routed mask | shared EP | actual EP mask | counts | cosine | relative L2 | output availability']
    for row in verified_one+double_tokens:
        lines.append(f'{row["name"]} | {row["user"]} | 0x{row["routed_mask"]:02x} | '
            f'{row["shared_ep"]} | 0x{row["actual_ep_mask"]:02x} | {row["counts"]} | '
            f'{row.get("cosine_similarity")} | {row.get("relative_l2_error")} | '
            f'{"available" if row["actual_available"] else "MISSING"}')
    args.report.with_suffix('.txt').write_text('\n'.join(lines)+'\n')
    print(json.dumps({k:result[k]['statistics'] for k in ('single','double')},indent=2))
    return functional_passed


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle',type=Path,required=True)
    parser.add_argument('--pcode-root',type=Path,required=True)
    parser.add_argument('--candidate-source',type=Path,help='Frozen isolated source directory for the final source-VC candidate')
    parser.add_argument('--experiment-root',type=Path,required=True)
    parser.add_argument('--catalog',type=Path,required=True)
    parser.add_argument('--single',type=Path,required=True)
    parser.add_argument('--two',type=Path,required=True)
    parser.add_argument('--model',type=Path,default=Path('build-ep6/glm5-dense-model'))
    parser.add_argument('--simulator',type=Path,default=Path('build-ep6/pcode_bundle'))
    parser.add_argument('--report',type=Path,default=Path('test/bach/pcode_riu_vc_results.json'))
    raise SystemExit(0 if run(parser.parse_args()) else 1)
