"""96 concurrent GLM5 pairs, full immutable model, exported pcode only."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time
import numpy as np
from pcode_glm5_dense import (MODEL_SEED, OFFSETS, PROFILES, specifications, ep_of,
    layout, prepare_model, DenseModel, reference, fixture, compare, sha)
from pcode_precision_reference import PrecisionRoutes
from pcode_glm5_program_audit import audit_program


def token_spec(experts, shared, seed, profile='signed', weights=None):
    assert len(experts) == len(set(experts)) == 8
    groups = [[] for _ in range(6)]; topk = [[] for _ in range(6)]
    if weights is None:
        weights = np.arange(1, 9, dtype=np.float32) / np.float32(36)
    weights = np.asarray(weights, dtype=np.float32)
    assert len(weights) == 8 and np.isfinite(weights).all() and min(weights) >= 0
    assert abs(float(weights.sum()) - 1) < 1e-6
    for expert, weight in zip(experts, weights):
        ep = ep_of(expert); groups[ep].append(expert - OFFSETS[ep]); topk[ep].append(float(weight))
    routed = sum(1 << ep for ep, ids in enumerate(groups) if ids)
    actual = routed | (1 << shared)
    return dict(global_experts=list(map(int, experts)), expert_ids=groups, topk_weights=topk,
        counts=list(map(len, groups)), shared_ep=shared, routed_mask=routed,
        mask=actual, actual_ep_mask=actual, routed_mask_hex=f'0x{routed:02x}',
        actual_ep_mask_hex=f'0x{actual:02x}', profile=profile, input_seed=seed, model_seed=MODEL_SEED)


def experts_for_mask(mask, seed):
    rng = np.random.default_rng(seed); active = [ep for ep in range(6) if mask >> ep & 1]
    counts = Counter(active)
    for ep in rng.choice(active, 8-len(active)): counts[int(ep)] += 1
    return [OFFSETS[ep] + int(local) for ep in active
            for local in rng.choice(OFFSETS[ep+1]-OFFSETS[ep], counts[ep], replace=False)]


def pairs():
    result = []; single = specifications()
    def add(name, family, left, right, gap=1, users=None):
        index = len(result); tokens = [deepcopy(left), deepcopy(right)]
        for j, spec in enumerate(tokens):
            spec.update(name=f'{name}.token{j}', family=family,
                user=spec['shared_ep'] + 6*((index*7+j*73)%170), token=30000+2*index+j)
            if users is not None: spec['user'] = users[j]
            assert spec['user'] % 6 == spec['shared_ep'] and 0 <= spec['user'] <= 1023
            spec.pop('relation', None)
        assert tokens[0]['user'] != tokens[1]['user']
        result.append(dict(name=name, family=family, mode='overlap', gap=gap, tokens=tokens))
    # All 36 ordered shared-EP pairs, and all 63 nonempty routed masks.
    for a in range(6):
        for b in range(6):
            i = 6*a+b; masks = [(2*i+j)%63+1 for j in range(2)]
            specs = [token_spec(experts_for_mask(mask, MODEL_SEED+5000+2*i+j), shared,
                                MODEL_SEED+6000+2*i+j)
                     for j, (mask, shared) in enumerate(zip(masks, [a,b]))]
            add(f'mask_shared_{a}_{b}', 'mask_shared_cross', *specs, gap=[1,128,512][i%3])
    # Every global expert executes, paired with a different block of experts.
    for i in range(16):
        add(f'expert_blocks_{i:02}', 'all_experts', single[2*i], single[2*i+1])
    boundaries = [s for s in single if s['family'] == 'boundary']
    for i, spec in enumerate(boundaries):
        other = boundaries[(i+7)%len(boundaries)]
        add(f'boundary_pair_{i:02}', 'boundary', spec, other, gap=[1,512][i%2])
    numeric = [s for s in single if s['family'] == 'numerical']
    for i in range(8):
        a, b = numeric[2*i], numeric[2*((i+3)%8)+1]
        add(f'numeric_{PROFILES[i]}_forward', 'numerical', a, b)
        add(f'numeric_{PROFILES[i]}_reverse', 'numerical', b, a, gap=512)
    def spec(ids, shared, seed=MODEL_SEED+8000, profile='signed', weights=None):
        return token_spec(ids,shared,seed,profile,weights)
    common = [0,1,43,86,129,172,215,255]
    add('contention_identical_experts', 'contention', spec(common,3), spec(common,3,MODEL_SEED+8001))
    add('contention_identical_payload', 'contention', spec(common,3), spec(common,3), users=[3,1023])
    add('contention_partial_expert_overlap', 'contention', spec(common,0), spec([0,1,43,86,130,173,216,254],5))
    # EP5 input traverses more hops. Delay the near-EP0 token so both compute
    # phases overlap, rather than letting EP0 finish before EP5 starts.
    add('contention_ep5_to_ep0', 'contention', spec(list(range(248,256)),5), spec(list(range(8)),0), gap=2048)
    add('contention_ep0_to_ep5', 'contention', spec(list(range(8)),5), spec(list(range(248,256)),5))
    add('contention_same_actual_different_routed', 'contention', spec(list(range(8)),5), spec(list(range(248,256)),0))
    add('contention_same_routed_different_shared', 'contention', spec([0,1,2,3,252,253,254,255],1), spec([0,1,2,3,252,253,254,255],4))
    weights = np.arange(1,9,dtype=np.float32)/36
    add('contention_topk_permutation', 'contention', spec(common,2,weights=weights), spec(common[::-1],2,weights=weights[::-1]))
    add('contention_zero_coefficients', 'contention', spec(common,4,weights=np.array([1,1,1,1,0,0,0,0],np.float32)/4), spec(common,4))
    add('contention_zero_input_isolation', 'contention', spec(common,5,profile='zero'), spec(common,5,profile='mixed_scale'))
    assert len(result) == len({p['name'] for p in result}) == 96
    assert {s['routed_mask'] for p in result[:36] for s in p['tokens']} == set(range(1,64))
    assert {(p['tokens'][0]['shared_ep'],p['tokens'][1]['shared_ep']) for p in result[:36]} == {(a,b) for a in range(6) for b in range(6)}
    assert {e for p in result[36:52] for s in p['tokens'] for e in s['global_experts']} == set(range(256))
    return result


def identity(args):
    paths = dict(simulator=args.simulator, runner=Path(__file__),
        dense_reference=Path(__file__).with_name('pcode_glm5_dense.py'),
        reference_helpers=Path(__file__).with_name('pcode_campaign.py'),
        precision_routes=Path(__file__).with_name('pcode_precision_reference.py'),
        program_audit=Path(__file__).with_name('pcode_glm5_program_audit.py'),
        harness=Path(__file__).with_name('pcode_bundle.cpp'),
        dense_loader=Path(__file__).with_name('pcode_dense_model.h'),
        route_catalog=args.catalog, model_manifest=args.model/'manifest.json')
    result = {key+'_sha256':sha(path) for key,path in paths.items()}
    digest = hashlib.sha256()
    for path in sorted([args.bundle/'pcode_ep6_tp8.bachir', *args.bundle.glob('*.hex')]):
        digest.update(path.name.encode()+b'\0'+path.read_bytes())
    result['bundle_sha256'] = digest.hexdigest()
    result['planned_cases_sha256'] = hashlib.sha256(json.dumps(pairs(),sort_keys=True).encode()).hexdigest()
    return result


def compatible_identity(recorded, current, out):
    """Permit a driver-only update with immutable source provenance.

    Program, simulator, fixtures plan, model and reference must be unchanged.
    The final audit freshly validates every fixture and arithmetic reference.
    """
    if recorded == current: return True
    if set(recorded) != set(current): return False
    if any(recorded[k] != current[k] for k in current if k != 'runner_sha256'): return False
    archive = out/'runner_sources'/(recorded['runner_sha256']+'.py')
    return archive.is_file() and sha(archive) == recorded['runner_sha256']


def native_evidence(text, pair, model_bytes, injection_order=(0,1)):
    sent = {int(i):int(c) for i,c in re.findall(r'pcode sent token (\d+) at (\d+)',text)}
    received = {int(i):int(c) for i,c in re.findall(r'pcode received token (\d+) at (\d+)',text)}
    matches = re.findall(r'pcode mask routed (\d+), shared EP (\d+), actual (\d+)',text)
    masks = [tuple(map(int,m)) for m in matches]
    expected = [(pair['tokens'][j]['routed_mask'],pair['tokens'][j]['shared_ep'],pair['tokens'][j]['actual_ep_mask']) for j in injection_order]
    preloads = re.findall(r'pcode dense model images (\d+), data bytes (\d+)',text)
    overlap = re.findall(r'pcode concurrent compute users at (\d+) (\d+) (\d+)',text)
    stop = re.search(r'pcode stopped at (\d+), output packets (\d+)',text)
    timing = (set(sent)==set(received)=={0,1} and max(sent.values()) < min(received.values()))
    compute = bool(len(overlap)==1 and {int(overlap[0][1]),int(overlap[0][2])} == {s['user'] for s in pair['tokens']})
    if compute:
        timing = timing and max(sent.values(),default=10**9) <= int(overlap[0][0]) < min(received.values(),default=0)
    sent = {injection_order[j]:c for j,c in sent.items()}
    received = {injection_order[j]:c for j,c in received.items()}
    return dict(sent_at=sent,received_at=received,
        simultaneous_compute_users_verified=compute,
        compute_overlap_cycle=int(overlap[0][0]) if compute else None,
        overlapped=bool(timing),native_masks_verified=masks==expected,
        all_model_images_preloaded=preloads==[('768',str(model_bytes))],
        completed_cycles=int(stop[1]) if stop else None, output_packets=int(stop[2]) if stop else 0,
        native_state_checks_passed='[  PASSED  ] 1 test.' in text and '[  FAILED  ]' not in text)


def alignment_only_failure(report, text):
    """Only retry stimulus timing; numerical/state failures remain failures."""
    return bool(report['simulator_returncode']==1 and report['output_packets']==2
        and report['overlapped'] and not report['simultaneous_compute_users_verified']
        and report['native_masks_verified'] and report['all_model_images_preloaded']
        and all(t['passed'] for t in report['tokens'])
        and text.count(': Failure\n')==1
        and 'Expected: (h.compute_overlap_cycle) > (0u), actual: 0 vs 0' in text)


def summary(out,reports,planned,source):
    tokens = [s for p in reports for s in p['tokens']]
    cos = [s['cosine_similarity'] for s in tokens if s.get('cosine_similarity') is not None]
    result = dict(total=len(reports),planned=planned,passed=sum(p['passed'] for p in reports),
        failed=[p['name'] for p in reports if not p['passed']],identity=source,
        coverage=dict(Counter(p['family'] for p in reports)),token_outputs=len(tokens),
        minimum_nonzero_cosine=min(cos) if cos else None,
        maximum_relative_l2=max((s.get('relative_l2_error',0) for s in tokens),default=0),
        cases=sorted(reports,key=lambda p:p['name']))
    (out/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


def run(args):
    assert 1 <= args.workers <= 4 and 1 <= args.threads <= 16
    args.out.mkdir(parents=True,exist_ok=True)
    templates,logical = layout(args.bundle,args.catalog)
    program = audit_program(args.bundle,args.catalog,templates)
    manifest = prepare_model(args.model,templates,logical)
    model = DenseModel(args.model,manifest); routes = PrecisionRoutes(args.bundle,args.catalog,logical)
    planned = pairs(); selected = [p for p in planned if args.filter in p['name']]
    if args.limit: selected=selected[:args.limit]
    assert selected
    (args.out/'planned_cases.json').write_text(json.dumps(planned,indent=2)+'\n')
    source = identity(args)
    archives=args.out/'runner_sources';archives.mkdir(exist_ok=True)
    (archives/(source['runner_sha256']+'.py')).write_bytes(Path(__file__).read_bytes())
    def execute(pair):
        name=pair['name']; path=args.out/(name+'.report.json')
        previous=None
        if args.resume and path.exists():
            old=json.loads(path.read_text())
            if old.get('passed') and compatible_identity(old['identity'],source,args.out) and old['threads']==args.threads:
                for j,s in enumerate(old['tokens']):
                    assert sha(args.out/(s['name']+'.fixture'))==s['fixture_sha256']
                    assert sha(args.out/(s['name']+'.expected.bf16'))==s['expected_sha256']
                    assert sha(args.out/(name+f'.actual.token{j}.bf16'))==s['actual_sha256']
                assert sha(args.out/(name+'.log'))==old['log_sha256']
                return old
            if compatible_identity(old['identity'],source,args.out):
                old_log=args.out/(name+'.log')
                if sha(old_log)==old['log_sha256'] and alignment_only_failure(old,old_log.read_text()):
                    previous=old
        start=time.monotonic(); metadata=[]
        for spec in pair['tokens']:
            meta=fixture(spec,args.out); expected,shared_hash=reference(spec,model,logical,routes)
            ep=args.out/(spec['name']+'.expected.bf16'); ep.write_bytes(expected.astype('<u2').tobytes())
            metadata.append(dict(meta,expected_sha256=sha(ep),shared_reference_sha256=shared_hash))
        actual=args.out/(name+'.actual')
        previous_attempts=[]
        if previous is not None:
            for j,meta in enumerate(metadata):
                for key in ['fixture_sha256','expected_sha256','shared_reference_sha256']:
                    assert previous['tokens'][j][key]==meta[key]
                assert sha(Path(str(actual)+f'.token{j}.bf16'))==previous['tokens'][j]['actual_sha256']
            previous_attempts=previous['timing_alignment_attempts'][:]
            trial=dict(previous);index=len(previous_attempts)
            trial['log_file']=f'{name}.alignment_attempt{index}.log'
            (args.out/(name+'.log')).rename(args.out/trial['log_file'])
            trial['actual_files']=[]
            for j in range(2):
                dest=args.out/f'{name}.alignment_attempt{index}.actual.token{j}.bf16'
                Path(str(actual)+f'.token{j}.bf16').rename(dest);trial['actual_files'].append(dest.name)
            previous_attempts.append(trial)
        for j in range(2): Path(str(actual)+f'.token{j}.bf16').unlink(missing_ok=True)
        env=dict(os.environ,PCODE_BUNDLE_ROOT=str(args.bundle.parent.resolve()),
            PCODE_CASE_FILE=str((args.out/(metadata[0]['name']+'.fixture')).resolve()),
            PCODE_SECOND_CASE_FILE=str((args.out/(metadata[1]['name']+'.fixture')).resolve()),
            PCODE_DENSE_MODEL=str(args.model.resolve()),PCODE_ACTUAL=str(actual.resolve()),
            PCODE_THREADS=str(args.threads),PCODE_MAX_CYCLES=str(args.max_cycles),PCODE_INJECT_MODE='overlap',
            PCODE_REQUIRE_COMPUTE_OVERLAP='1',PCODE_TOKEN_GAP=str(pair['gap']))
        print(f'start {name}: '+str([(s['routed_mask_hex'],s['shared_ep'],s['actual_ep_mask_hex']) for s in metadata]),flush=True)
        log=args.out/(name+'.log'); attempts=previous_attempts
        strategies=[((0,1),g) for g in dict.fromkeys([pair['gap'],2048,4096])]
        reverse=[((1,0),g) for g in dict.fromkeys([2048,pair['gap'],4096])]
        strategies = reverse if previous is not None else strategies+reverse
        for attempt,(order,gap) in enumerate(strategies):
            env['PCODE_TOKEN_GAP']=str(gap)
            env['PCODE_CASE_FILE']=str((args.out/(metadata[order[0]]['name']+'.fixture')).resolve())
            env['PCODE_SECOND_CASE_FILE']=str((args.out/(metadata[order[1]]['name']+'.fixture')).resolve())
            try:
                with log.open('w') as stream:
                    code=subprocess.run([str(args.simulator.resolve())],env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=args.timeout).returncode
            except subprocess.TimeoutExpired: code=124
            # Native output indices follow the injection order. Preserve logical
            # token identities in filenames; leave the raw native log untouched.
            if order==(1,0):
                outputs=[Path(str(actual)+f'.token{j}.bf16') for j in range(2)]
                if all(p.exists() for p in outputs):
                    temporary=Path(str(actual)+'.swap.bf16')
                    outputs[0].rename(temporary);outputs[1].rename(outputs[0]);temporary.rename(outputs[1])
            text=log.read_text();evidence=native_evidence(text,pair,manifest['total_data_bytes'],order)
            report=dict(name=name,family=pair['family'],mode=pair['mode'],gap=gap,requested_gap=pair['gap'],threads=args.threads,
                maximum_cycles=args.max_cycles,
                injection_order=list(order),identity=source,simulator_returncode=code,**evidence,tokens=[],log_sha256=sha(log))
            for j,meta in enumerate(metadata):
                try: metrics=compare(Path(str(actual)+f'.token{j}.bf16'),args.out/(meta['name']+'.expected.bf16'))
                except (OSError,AssertionError,ValueError) as error: metrics=dict(passed=False,error=str(error))
                report['tokens'].append(dict(meta,**metrics))
            report['passed']=bool(code==0 and report['output_packets']==2 and all(report[k] for k in
                ['overlapped','simultaneous_compute_users_verified','native_masks_verified','all_model_images_preloaded','native_state_checks_passed'])
                and all(t['passed'] for t in report['tokens']))
            if report['passed'] or not alignment_only_failure(report,text) or attempt==len(strategies)-1: break
            trial=dict(report);index=len(attempts);trial['log_file']=f'{name}.alignment_attempt{index}.log'
            log.rename(args.out/trial['log_file']);trial['actual_files']=[]
            for j in range(2):
                src=Path(str(actual)+f'.token{j}.bf16')
                dest=args.out/f'{name}.alignment_attempt{index}.actual.token{j}.bf16'
                src.rename(dest);trial['actual_files'].append(dest.name)
            attempts.append(trial)
            print(f'{name}: both numeric outputs/state checks passed, compute phases did not overlap; retry order/gap={strategies[attempt+1]}',flush=True)
        report['timing_alignment_attempts']=attempts
        report['wall_seconds']=time.monotonic()-start
        path.write_text(json.dumps(report,indent=2)+'\n');return report
    reports=[]
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        pending=[pool.submit(execute,p) for p in selected]
        for future in as_completed(pending):
            r=future.result(); reports.append(r); summary(args.out,reports,len(selected),source)
            print(f'{len(reports)}/{len(selected)} {r["name"]} {"PASS" if r["passed"] else "FAIL"} '+
                f'cos={[s.get("cosine_similarity") for s in r["tokens"]]} overlap_cycle={r["compute_overlap_cycle"]} {r["wall_seconds"]:.1f}s',flush=True)
            if not r['passed']:
                for f in pending: f.cancel()
                raise SystemExit(1)
    assert all(r['passed'] for r in reports)


def parser():
    p=argparse.ArgumentParser()
    p.add_argument('--bundle',type=Path,required=True);p.add_argument('--catalog',type=Path,required=True)
    p.add_argument('--model',type=Path,default=Path('build-ep6/glm5-dense-model'))
    p.add_argument('--out',type=Path,default=Path('build-ep6/glm5-dense-two-token-96'))
    p.add_argument('--simulator',type=Path,default=Path('build-ep6/pcode_bundle'))
    p.add_argument('--workers',type=int,default=2);p.add_argument('--threads',type=int,default=2)
    p.add_argument('--timeout',type=int,default=2400);p.add_argument('--filter',default='')
    p.add_argument('--max-cycles',type=int,default=50000)
    p.add_argument('--limit',type=int);p.add_argument('--resume',action='store_true')
    return p

if __name__=='__main__': run(parser().parse_args())
