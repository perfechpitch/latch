"""Ten batches of 64 GLM5 tokens through an exported pcode program and latch.

A batch is a stream of distinct users, not 64 physical TS slots. Numerical and
quiescence checks determine PASS; sampled concurrency is recorded separately.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from copy import deepcopy
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import subprocess
import tempfile
import time

import numpy as np
from pcode_glm5_dense import (MODEL_SEED, OFFSETS, PROFILES, specifications,
    layout, prepare_model, DenseModel, reference, fixture, compare, sha)
from pcode_glm5_two_token import token_spec, experts_for_mask
from pcode_glm5_dense_verify import verify_fixture
from pcode_precision_reference import PrecisionRoutes
from pcode_glm5_program_audit import audit_program

N = 64


def groups():
    plan = []
    seed = MODEL_SEED + 64000
    ramp = np.arange(1, 9, dtype=np.float32) / np.float32(36)
    uniform = np.full(8, 1 / 8, dtype=np.float32)
    common = [0, 1, 43, 86, 129, 172, 215, 255]

    def make(ids, shared, offset, profile='signed', weights=None):
        return token_spec(ids, shared, seed + offset, profile, weights)

    def add(name, description, tokens, gap):
        assert len(tokens) == N
        gi = len(plan); tokens = deepcopy(tokens); used = set()
        for i, spec in enumerate(tokens):
            uid = spec['shared_ep'] + 6 * ((gi * 73 + i * 13) % 170)
            if name == 'same_experts_hotspot' and i < 2:
                uid = [3, 1023][i]
            while uid in used or (name == 'same_experts_hotspot' and i >= 2 and uid in (3, 1023)):
                uid = spec['shared_ep'] + 6 * ((uid // 6 + 1) % 170)
            used.add(uid)
            spec.update(name=f'{name}.token{i:02d}', family=name, user=uid,
                        token=60000 + gi * N + i)
            assert 0 <= uid <= 1023 and uid % 6 == spec['shared_ep']
        assert len(used) == N
        plan.append(dict(name=name, description=description, gap=gap, tokens=tokens))

    add('expert_bank_forward', 'All 256 experts twice, ascending eight-expert blocks; shared EP rotates.',
        [make(list(range(8 * (i % 32), 8 * (i % 32) + 8)), i % 6, i) for i in range(N)], 1)
    add('expert_bank_reverse', 'All 256 experts twice, descending blocks and reversed local order; reverse shared rotation.',
        [make(list(range(8 * (31 - i % 32), 8 * (31 - i % 32) + 8))[::-1], 5 - i % 6, 100 + i) for i in range(N)], 64)
    hot = [make(common, 3, 200 + i) for i in range(N)]
    hot[1] = deepcopy(hot[0])
    add('same_experts_hotspot', '64 users contend for identical experts/shared EP3; first pair has identical payload and UID3/1023.', hot, 1)
    perm = []
    for batch in range(5):
        for reverse in [False, True]:
            for ep in range(6):
                perm.append(make(common[::-1] if reverse else common, ep, 300 + batch,
                                 weights=ramp[::-1] if reverse else ramp))
    perm += [make([0, 43, 86, 129, 173+i, 180+i, 216+i, 250+i], i, 310+i) for i in range(4)]
    add('shared_permutation', 'Five sets of six shared placements and six matched reversed TopK orders, plus partial-overlap inputs.', perm, 64)
    for reverse in [False, True]:
        masks = list(range(63, 0, -1)) + [1] if reverse else list(range(1, 64)) + [63]
        add('mask_sweep_reverse' if reverse else 'mask_sweep_forward',
            'All 63 nonempty routed masks; varied experts and shared placement; ' + ('descending.' if reverse else 'ascending.'),
            [make(experts_for_mask(mask, seed + 500 + 64*reverse + i), (5-i) % 6 if reverse else i % 6,
                  600 + 64*reverse + i) for i, mask in enumerate(masks)], 128 if reverse else 1)
    boundary = [s for s in specifications() if s['family'] == 'boundary']
    b = []
    for i in range(N):
        spec = deepcopy(boundary[i % len(boundary)])
        spec['input_seed'] += (i // len(boundary)) * 1000
        b.append(spec)
    add('boundary_ring', 'Cycle all 18 expert-boundary specifications, including every EP first/last, adjacent divisions and global endpoints.', b, 128)
    concentrated = []
    for i in range(N):
        ep = (5-i) % 6
        ids = np.random.default_rng(seed+800+i).choice(np.arange(OFFSETS[ep], OFFSETS[ep+1]),8,replace=False).tolist()
        concentrated.append(make(ids,ep,900+i,weights=uniform))
    add('concentrated_ep_round_robin', 'Eight routed experts and shared in one EP per token; cycle EP5..EP0 to exercise independent queues.', concentrated, 1)
    numeric = []
    for i in range(N):
        w = ramp if i % 3 == 0 else uniform
        if i % 3 == 2: w = np.array([1,1,1,1,0,0,0,0],np.float32)/4
        numeric.append(make(experts_for_mask(63,seed+1000+i),i % 6,1100+i,PROFILES[i // 8],w))
    add('numeric_isolation', 'Eight input profiles times eight seeds; full-EP routed masks, uniform/ramp/zero coefficients.', numeric, 64)
    masks = [0x01,0x20,0x15,0x2a,0x09,0x12,0x24,0x3f]
    alt = []
    for i in range(N):
        mask = masks[i % 8]; ep = (i//8 + i) % 6
        w = np.array([1,0,0,0,0,0,0,0],np.float32) if i % 4 == 0 else ramp
        alt.append(make(experts_for_mask(mask,seed+1200+i),ep,1300+i,weights=w))
    add('alternating_masks', 'Alternate near/far, disjoint/interleaved/full EP masks; shared inside/outside routed set and one-hot weights.', alt, 1)
    assert len(plan) == 10 and all(len(g['tokens']) == N for g in plan)
    assert {e for g in plan[:2] for t in g['tokens'] for e in t['global_experts']} == set(range(256))
    assert all({t['routed_mask'] for t in g['tokens']} == set(range(1,64)) for g in plan[4:6])
    return plan


def evidence(text, group, model_bytes):
    expected = {i:(s['user'],s['token']) for i,s in enumerate(group['tokens'])}; n=len(expected)
    def rows(pattern):return [tuple(map(int,r)) for r in re.findall(pattern,text)]
    sent=rows(r'pcode sent token (\d+) at (\d+) user (\d+) token (\d+)')
    received=rows(r'pcode received token (\d+) at (\d+) user (\d+) token (\d+)')
    heads=rows(r'pcode wire token (\d+) head at (\d+) user (\d+) token (\d+)')
    tails=rows(r'pcode wire token (\d+) tail at (\d+) user (\d+) token (\d+)')
    def valid(rs):
        return len(rs)==n and {r[0] for r in rs}==set(expected) and all(expected[i]==(u,t) for i,c,u,t in rs)
    sent_times={i:c for i,c,u,t in sent};got={i:c for i,c,u,t in received}
    h={i:c for i,c,u,t in heads};tl={i:c for i,c,u,t in tails}
    causal=valid(sent) and valid(received) and all(sent_times[i]<got[i] for i in expected)
    wire=bool(causal and valid(heads) and valid(tails)
        and all(sent_times[i]<=h[i]<tl[i]<got[i] for i in expected)
        and all(tl[i]<h[i+1] for i in range(n-1)))
    stop=re.search(r'pcode stopped at (\d+), output packets (\d+)',text)
    occ=re.search(r'pcode concurrency summary peak (\d+) core peak (\d+) overlapped users (\d+) stream mask (\d+)',text)
    peak,local,participants,slots=map(int,occ.groups()) if occ else (0,0,0,0)
    peak=max([peak]+[int(x) for x in re.findall(r'pcode peak compute users (\d+)',text)])
    masks=rows(r'pcode mask routed (\d+), shared EP (\d+), actual (\d+)')
    progress=rows(r'pcode cycle (\d+), output packets (\d+)')
    increased=[c for j,(c,k) in enumerate(progress) if j==0 or k>progress[j-1][1]]
    errors=[s for s in text.splitlines() if 'assertion failed:' in s or ': Failure' in s]
    return dict(sent_at=sent_times,received_at=got,wire_head_at=h,wire_tail_at=tl,
        source_submissions=len(sent),ingress_heads=len(heads),ingress_tails=len(tails),
        identities_verified=valid(sent) and valid(received),causal_timing_verified=bool(causal),wire_ingress_verified=wire,
        native_masks_verified=masks==[(s['routed_mask'],s['shared_ep'],s['actual_ep_mask']) for s in group['tokens']],
        all_model_images_preloaded=rows(r'pcode dense model images (\d+), data bytes (\d+)')==[(768,model_bytes)],
        peak_compute_users=peak,peak_core_users=local,overlap_participants=participants,observed_stream_mask=slots,
        completed_cycles=int(stop[1]) if stop else None,output_packets=int(stop[2]) if stop else len(received),
        last_progress_cycle=progress[-1][0] if progress else 0,last_progress_outputs=progress[-1][1] if progress else 0,
        last_output_progress_cycle=increased[-1] if increased else None,native_errors=errors,
        native_state_checks_passed='[  PASSED  ] 1 test.' in text and '[  FAILED  ]' not in text)


def identity(args,plan):
    files={'runner':Path(__file__),'harness':Path(__file__).with_name('pcode_bundle.cpp'),
        'spread_harness':Path(__file__).parent/'ip/chip/moe_common.h','reference':Path(__file__).with_name('pcode_glm5_dense.py'),
        'reference_precision':Path(__file__).with_name('pcode_precision_reference.py'),
        'fixture_verifier':Path(__file__).with_name('pcode_glm5_dense_verify.py'),'simulator':args.simulator,
        'catalog':args.catalog,'model_manifest':args.model/'manifest.json'}
    out={k+'_sha256':sha(p) for k,p in files.items()};d=hashlib.sha256()
    for p in sorted([args.bundle/'pcode_ep6_tp8.bachir',*args.bundle.glob('*.hex')]):d.update(p.name.encode()+b'\0'+p.read_bytes())
    out['bundle_sha256']=d.hexdigest();out['plan_sha256']=hashlib.sha256(json.dumps(plan,sort_keys=True).encode()).hexdigest()
    return out


def verdict(r,text):
    complete=(r['simulator_returncode']==0 and r['output_packets']==N and r['completed_cycles'] is not None
        and r['completed_cycles']<r['maximum_cycles'] and all(r[k] for k in ['identities_verified',
            'causal_timing_verified','wire_ingress_verified','native_masks_verified','all_model_images_preloaded','native_state_checks_passed'])
        and len(r['tokens'])==N and all(t['passed'] for t in r['tokens']))
    if complete:return 'PASS'
    if 'assertion failed: SameNet' in text:return 'REDUCE_STAGE_ASSERT'
    if r['simulator_returncode']==124:return 'HOST_TIMEOUT_UNRESOLVED'
    if r['completed_cycles'] is not None and r['completed_cycles']>=r['maximum_cycles']:return 'CYCLE_LIMIT_REQUIRES_DEADLOCK_ANALYSIS'
    if any(t.get('actual_sha256') and not t['passed'] for t in r['tokens']):return 'NUMERICAL_FAILURE'
    return 'STATE_OR_EXECUTION_FAILURE'


def summarize(args,reports,source):
    data=dict(planned_groups=10,planned_tokens=640,executed_groups=len(reports),
        passed_groups=sum(r['passed'] for r in reports),failed_groups=[r['name'] for r in reports if not r['passed']],
        actual_outputs_checked=sum('actual_sha256' in t for r in reports for t in r['tokens']),
        actual_outputs_passed=sum(bool(t['passed']) for r in reports for t in r['tokens']),
        identity=source,cases=sorted(reports,key=lambda r:r['name']))
    data['campaign_passed']=len(reports)==10 and data['passed_groups']==10 and data['actual_outputs_passed']==640
    (args.out/'summary.json').write_text(json.dumps(data,indent=2)+'\n')
    lines=['GLM5 EP6+TP8: 10 batches x 64 tokens (16 physical TS slots, unchanged)',
        f'Executed {len(reports)}/10, passed {data["passed_groups"]}, outputs checked/passed {data["actual_outputs_checked"]}/{data["actual_outputs_passed"]}',
        'group\tclassification\toutputs\tcompleted_cycle\tpeak_global/core\tcos_min\tL2_max']
    for r in data['cases']:
        cos=[t['cosine_similarity'] for t in r['tokens'] if 'cosine_similarity' in t]
        l2=[t['relative_l2_error'] for t in r['tokens'] if 'relative_l2_error' in t]
        lines.append(f'{r["name"]}\t{r["classification"]}\t{r["output_packets"]}\t{r["completed_cycles"]}\t{r["peak_compute_users"]}/{r["peak_core_users"]}\t{min(cos) if cos else None}\t{max(l2) if l2 else None}')
        for i,t in enumerate(r['tokens']):
            lines.append(f'  token{i:02} uid={t["user"]} token={t["token"]} routed={t["routed_mask_hex"]} shared={t["shared_ep"]} actual={t["actual_ep_mask_hex"]} experts={t["global_experts"]} profile={t["profile"]} seed={t["input_seed"]} passed={t["passed"]} cos={t.get("cosine_similarity")} L2={t.get("relative_l2_error")}')
    (args.out/'results.txt').write_text('\n'.join(lines)+'\n');return data


def run(args):
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    plan=groups();selected=[g for g in plan if args.filter in g['name']]
    if args.limit:selected=selected[:args.limit]
    assert selected and 1<=args.workers<=2 and 1<=args.threads<=16
    args.out.mkdir(parents=True,exist_ok=True)
    (args.out/'planned_cases.json').write_text(json.dumps(plan,indent=2)+'\n')
    if args.plan_only:
        print(json.dumps([dict(name=g['name'],description=g['description'],gap=g['gap']) for g in selected],indent=2));return
    templates,logical=layout(args.bundle,args.catalog)
    (args.out/'program_audit.json').write_text(json.dumps(audit_program(args.bundle,args.catalog,templates),indent=2)+'\n')
    manifest=prepare_model(args.model,templates,logical);model=DenseModel(args.model,manifest)
    routes=PrecisionRoutes(args.bundle,args.catalog,logical);source=identity(args,plan)
    archive=args.out/'executed_sources';archive.mkdir(exist_ok=True)
    for p in [Path(__file__),Path(__file__).with_name('pcode_bundle.cpp'),Path(__file__).parent/'ip/chip/moe_common.h']:
        (archive/p.name).write_bytes(p.read_bytes())

    def execute(g):
        path=args.out/g['name'];path.mkdir(exist_ok=True);metadata=[];start=time.monotonic()
        for s in g['tokens']:
            meta=fixture(s,path);verify_fixture(path/(s['name']+'.fixture'),s)
            expected,shared_hash=reference(s,model,logical,routes)
            p=path/(s['name']+'.expected.bf16');p.write_bytes(expected.astype('<u2').tobytes())
            metadata.append(dict(meta,expected_sha256=sha(p),shared_reference_sha256=shared_hash))
        listing=path/'fixtures.list';listing.write_text(''.join(str((path/(s['name']+'.fixture')).resolve())+'\n' for s in g['tokens']))
        env=dict(os.environ)
        for k in list(env):
            if k.startswith('PCODE_'):env.pop(k)
        env.update(PCODE_BUNDLE_ROOT=str(args.bundle.parent.resolve()),PCODE_CASE_LIST=str(listing.resolve()),
            PCODE_DENSE_MODEL=str(args.model.resolve()),PCODE_ACTUAL=str((path/'actual').resolve()),
            PCODE_THREADS=str(args.threads),PCODE_MAX_CYCLES=str(args.max_cycles),
            PCODE_INJECT_MODE='overlap',PCODE_TOKEN_GAP=str(g['gap']))
        for i in range(N):(path/f'actual.token{i}.bf16').unlink(missing_ok=True)
        (path/'execution_env.json').write_text(json.dumps({k:v for k,v in env.items() if k.startswith('PCODE_')},indent=2)+'\n')
        print(f'start {g["name"]}: 64 tokens gap={g["gap"]}',flush=True)
        try:
            with (path/'native.log').open('w') as log:
                code=subprocess.run([str(args.simulator.resolve())],cwd=path,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=args.timeout).returncode
        except subprocess.TimeoutExpired:code=124
        text=(path/'native.log').read_text();r=dict(name=g['name'],description=g['description'],gap=g['gap'],threads=args.threads,
            maximum_cycles=args.max_cycles,simulator_returncode=code,identity=source,log_sha256=sha(path/'native.log'),
            wall_seconds=time.monotonic()-start,**evidence(text,g,manifest['total_data_bytes']),tokens=[])
        for i,meta in enumerate(metadata):
            try:m=compare(path/f'actual.token{i}.bf16',path/(meta['name']+'.expected.bf16'))
            except (OSError,AssertionError,ValueError) as error:m=dict(passed=False,error=str(error))
            r['tokens'].append(dict(meta,**m))
        r['classification']=verdict(r,text);r['passed']=r['classification']=='PASS'
        (path/'report.json').write_text(json.dumps(r,indent=2)+'\n');return r
    reports=[]
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        fs=[pool.submit(execute,g) for g in selected]
        for f in as_completed(fs):
            r=f.result();reports.append(r);summarize(args,reports,source)
            print(f'{len(reports)}/{len(selected)} {r["name"]} {r["classification"]} outputs={r["output_packets"]} cycles={r["completed_cycles"]} peak={r["peak_compute_users"]}/{r["peak_core_users"]}',flush=True)
    if not all(r['passed'] for r in reports):raise SystemExit(1)


def audit(args):
    summary=json.loads((args.out/'summary.json').read_text());plan=groups();source=identity(args,plan)
    assert summary['identity']==source
    assert summary['executed_groups']==summary['planned_groups']==10
    assert json.loads((args.out/'planned_cases.json').read_text())==plan
    templates,logical=layout(args.bundle,args.catalog);manifest=prepare_model(args.model,templates,logical)
    model=DenseModel(args.model,manifest);routes=PrecisionRoutes(args.bundle,args.catalog,logical)
    reports={r['name']:r for r in summary['cases']};verified=0;checked=0;refs=0;controls=[]
    for g in plan:
        r=reports[g['name']];path=args.out/g['name'];text=(path/'native.log').read_text()
        assert r==json.loads((path/'report.json').read_text()) and sha(path/'native.log')==r['log_sha256']
        for k,v in evidence(text,g,manifest['total_data_bytes']).items():assert r[k]==json.loads(json.dumps(v)),k
        assert len(r['tokens'])==N and r['identity']==source
        for i,s in enumerate(g['tokens']):
            t=r['tokens'][i];verify_fixture(path/(s['name']+'.fixture'),s)
            with tempfile.TemporaryDirectory() as temp:
                rebuilt=fixture(s,Path(temp));assert all(t[k]==v for k,v in rebuilt.items())
            expected,sh=reference(s,model,logical,routes);raw=expected.astype('<u2').tobytes();refs+=1
            assert raw==(path/(s['name']+'.expected.bf16')).read_bytes()
            assert hashlib.sha256(raw).hexdigest()==t['expected_sha256'] and sh==t['shared_reference_sha256']
            actual=path/f'actual.token{i}.bf16'
            if actual.exists():
                m=compare(actual,path/(s['name']+'.expected.bf16'));assert all(t[k]==v for k,v in m.items())
                checked+=1;verified+=int(m['passed'])
            else:assert not t['passed'] and not r['passed']
        assert r['classification']==verdict(r,text) and r['passed']==(r['classification']=='PASS')
        print('audited',g['name'],'references=64 outputs=',sum('actual_sha256' in t for t in r['tokens']),flush=True)
    # Matched-payload and permutation checks, in addition to independent references.
    for name in ['same_experts_hotspot','shared_permutation']:
        r=reports[name];path=args.out/name
        pairs=[(0,1)] if name=='same_experts_hotspot' else [(b*12+ep,b*12+6+ep) for b in range(5) for ep in range(6)]
        for a,b in pairs:
            if all((path/f'actual.token{i}.bf16').exists() for i in [a,b]):
                if name=='same_experts_hotspot':
                    assert r['tokens'][a]['expected_sha256']==r['tokens'][b]['expected_sha256']
                else:
                    assert compare(path/(r['tokens'][a]['name']+'.expected.bf16'),path/(r['tokens'][b]['name']+'.expected.bf16'))['passed']
                m=compare(path/f'actual.token{a}.bf16',path/f'actual.token{b}.bf16');assert m['passed']
                controls.append(dict(group=name,left=a,right=b,**m))
    assert checked==summary['actual_outputs_checked'] and verified==summary['actual_outputs_passed'] and refs==640
    result=dict(audited_at=datetime.now(timezone.utc).isoformat(),audit_passed=True,campaign_passed=summary['campaign_passed'],
        fixtures_rebuilt=640,references_rebuilt=refs,actual_outputs_checked=checked,actual_outputs_passed=verified,
        controls=controls,summary_sha256=sha(args.out/'summary.json'),identity=source)
    (args.out/'audit.json').write_text(json.dumps(result,indent=2)+'\n')
    print('audit complete',refs,checked,verified,flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bundle',type=Path,required=True);p.add_argument('--catalog',type=Path,required=True)
    p.add_argument('--model',type=Path,default=Path('build-ep6/glm5-dense-model'))
    p.add_argument('--simulator',type=Path,default=Path('build-ep6/pcode_bundle'))
    p.add_argument('--out',type=Path,default=Path('build-ep6/glm5-64-token'))
    p.add_argument('--threads',type=int,default=1);p.add_argument('--workers',type=int,default=2)
    p.add_argument('--max-cycles',type=int,default=160000);p.add_argument('--timeout',type=int,default=3600)
    p.add_argument('--filter',default='');p.add_argument('--limit',type=int)
    p.add_argument('--plan-only',action='store_true');p.add_argument('--audit-only',action='store_true')
    args=p.parse_args()
    # All subprocess paths remain correct when each simulator runs in its own output directory.
    for field in ['bundle','catalog','model','simulator','out']:setattr(args,field,getattr(args,field).resolve())
    audit(args) if args.audit_only else run(args)
