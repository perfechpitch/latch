"""96 GLM5 MoE cases: immutable full MXFP8 model, pcode -> latch only."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import time
import numpy as np

from pcode_campaign import FP8, bf16, widen, quantize, layout, compare
from pcode_precision_reference import PrecisionRoutes
from pcode_glm5_relations import shared_difference_check

MODEL_SEED = 20261012
OFFSETS = [0, 43, 86, 129, 172, 215, 256]
PROFILES = ['positive', 'negative', 'signed', 'zero', 'small', 'large', 'mixed_scale', 'alternating']
VERSION = 'glm5-dense-v1'


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def ep_of(expert):
    assert 0 <= expert < 256
    return next(ep for ep in range(6) if expert < OFFSETS[ep + 1])


def specifications():
    cases = []

    def add(name, family, experts, shared, profile='signed', input_seed=None, weights=None, **extra):
        assert len(experts) == len(set(experts)) == 8
        groups = [[] for _ in range(6)]
        topk = [[] for _ in range(6)]
        if weights is None:
            weights = np.arange(1, 9, dtype=np.float32) / np.float32(36)
        assert len(weights) == 8 and min(weights) >= 0 and abs(sum(weights) - 1) < 1e-6
        for expert, weight in zip(experts, weights):
            ep = ep_of(expert)
            groups[ep].append(expert - OFFSETS[ep]); topk[ep].append(float(weight))
        routed = sum(1 << ep for ep, group in enumerate(groups) if group)
        user = shared + 6 * ((len(cases) * 11) % 170)
        if name == 'boundary_global_ends':
            user = 1023; shared = 3
        index = len(cases)
        cases.append(dict(name=name, family=family, global_experts=list(experts),
                          expert_ids=groups, topk_weights=topk, counts=list(map(len, groups)),
                          user=user, token=10000 + index, shared_ep=shared, routed_mask=routed,
                          mask=routed | (1 << shared), actual_ep_mask=routed | (1 << shared),
                          routed_mask_hex=f'0x{routed:02x}', actual_ep_mask_hex=f'0x{routed | (1 << shared):02x}',
                          profile=profile, input_seed=MODEL_SEED + index if input_seed is None else input_seed,
                          model_seed=MODEL_SEED, **extra))

    for i in range(32):
        add(f'experts_{8*i:03d}_{8*i+7:03d}', 'expert_coverage', list(range(8*i, 8*i+8)), i % 6)
    masks = [1 << ep for ep in range(6)] + [3 << ep for ep in range(5)] + [5, 10, 20, 40, 21, 42, 63]
    for i, mask in enumerate(masks):
        active = [ep for ep in range(6) if mask >> ep & 1]
        counts = Counter(active[j % len(active)] for j in range(8))
        experts = [OFFSETS[ep] + j for ep in active for j in range(counts[ep])]
        # Single-EP cases retain exact one-bit masks; other cases exercise a
        # shared-only EP where possible, including EP0 forwarding without work.
        inactive = [ep for ep in range(6) if ep not in active]
        shared = active[0] if len(active) == 1 else inactive[i % len(inactive)] if inactive else i % 6
        add(f'route_{mask:02x}', 'routing', experts, shared)
    for ep in range(6):
        add(f'boundary_ep{ep}_first', 'boundary', list(range(OFFSETS[ep], OFFSETS[ep]+8)), (ep+1)%6)
        add(f'boundary_ep{ep}_last', 'boundary', list(range(OFFSETS[ep+1]-8, OFFSETS[ep+1])), ep)
    for ep in range(5):
        edge = OFFSETS[ep+1]
        add(f'boundary_ep{ep}_ep{ep+1}', 'boundary', list(range(edge-4, edge+4)), (ep+3)%6)
    add('boundary_global_ends', 'boundary', [0,1,2,3,252,253,254,255], 3)
    for i, profile in enumerate(PROFILES):
        for repeat in range(2):
            rng = np.random.default_rng(MODEL_SEED + 1000 + i*2 + repeat)
            experts = rng.choice(256, 8, replace=False).tolist()
            weights = rng.integers(1, 9, 8).astype(np.float32); weights /= weights.sum()
            add(f'numeric_{profile}_{repeat}', 'numerical', experts, (i+repeat)%6,
                profile, MODEL_SEED + 2000 + i*2 + repeat, weights)
    pair_experts = [list(range(248,256)), [0,1,2,3,172,173,174,175], [0,1,43,44,86,129,172,215]]
    for i, experts in enumerate(pair_experts):
        weights = (np.arange(1,9,dtype=np.float32)/36).tolist()
        add(f'order_{i}_forward', 'topk_order', experts, (i+2)%6, input_seed=MODEL_SEED+3000+i,
            weights=weights, relation=f'order_{i}')
        add(f'order_{i}_reverse', 'topk_order', experts[::-1], (i+2)%6, input_seed=MODEL_SEED+3000+i,
            weights=weights[::-1], relation=f'order_{i}')
    experts = [0,1,43,86,129,172,215,255]
    for shared in range(6):
        add(f'shared_replica_{shared}', 'shared_replica', experts, shared,
            input_seed=MODEL_SEED+4000, relation='shared_replicas')
    assert len(cases) == len({c['name'] for c in cases}) == 96
    assert Counter(c['family'] for c in cases) == dict(expert_coverage=32, routing=18, boundary=18,
                                                     numerical=16, topk_order=6, shared_replica=6)
    assert set(sum((c['global_experts'] for c in cases[:32]), [])) == set(range(256))
    return cases


def model_arrays(expert, stage, part, slot):
    """Model identity uses logical coordinates, never EP replica or token ID.

    FC13: rows 0:256 are this TP part's FC1, rows 256:512 its FC3.
    FC2: rows are this logical slot's 768 output dimensions, K is TP part.
    Every coefficient is generated; no K sampling or omitted tiles.
    """
    rows, kdim = (512, 768) if stage == 0 else (768, 256)
    rng = np.random.default_rng(np.random.SeedSequence([MODEL_SEED, expert+1, stage, part, slot]))
    pick = rng.integers(0, 8, (rows,kdim), dtype=np.uint8)
    values = np.array([0x08,0x10,0x18,0x20],np.uint8)[pick & 3] | ((pick >> 2) << 7)
    scales = rng.integers(125,129,(rows,kdim//32),dtype=np.uint8)
    return values, scales


def pack(values, scales):
    rows, kdim = values.shape; nb, kb = rows//128, kdim//64
    data = values.reshape(nb,128,kb,64).transpose(0,2,1,3).copy().tobytes()
    sf = scales.reshape(nb,128,kb,2).transpose(0,2,1,3).copy().tobytes()
    return data, sf


def prepare_model(root, templates, logical):
    root.mkdir(parents=True,exist_ok=True)
    config = dict(version=VERSION, seed=MODEL_SEED, templates={','.join(map(str,k)):v for k,v in templates.items()},
                  logical={','.join(map(str,k)):v for k,v in logical.items()})
    key = hashlib.sha256(json.dumps(config,sort_keys=True).encode()).hexdigest()
    manifest_path = root/'manifest.json'
    if manifest_path.exists():
        old = json.loads(manifest_path.read_text())
        assert old['config_sha256'] == key, 'model layout/config changed; use a new model directory'
        for entry in old['images']:
            path = root/entry['file']
            assert path.stat().st_size == entry['data_bytes']*33//32
            assert sha(path) == entry['sha256'], f'corrupted model image: {path}'
        assert sha(root/'layout.tsv') == old['layout_sha256']
        print('verified complete fixed model: 768 images',flush=True)
        return old
    images=[]; lines=[]; shared_hashes={}
    for number, ((chip,core,stage),t) in enumerate(sorted(templates.items())):
        ep,part,slot = chip//8, chip%8, logical[chip,core]
        count=OFFSETS[ep+1]-OFFSETS[ep]+1
        expected=(512,768) if stage==0 else (768,256)
        assert ((t[0]&65535)*128,(t[0]>>16)*64)==expected
        stride=expected[0]*expected[1]; assert t[7]==stride
        data_bytes=count*stride; filename=f'c{chip:02d}_p{core}_s{stage}.bin'
        path=root/filename
        with path.open('wb') as out: out.truncate(data_bytes*33//32)
        mapped=np.memmap(path,dtype=np.uint8,mode='r+',shape=(data_bytes*33//32,))
        hashes=[]
        for local in range(count):
            global_id=-1 if local==0 else OFFSETS[ep]+local-1
            values,scales=model_arrays(global_id,stage,part,slot)
            data,sf=pack(values,scales)
            assert len(data)==stride and len(sf)==stride//32
            mapped[local*stride:(local+1)*stride]=np.frombuffer(data,np.uint8)
            mapped[data_bytes+local*stride//32:data_bytes+(local+1)*stride//32]=np.frombuffer(sf,np.uint8)
            digest=hashlib.sha256(data+sf).hexdigest(); hashes.append(digest)
            if local==0:
                ref=shared_hashes.setdefault((stage,part,slot),digest)
                assert ref==digest, 'shared replicas must be identical'
        mapped.flush(); del mapped
        base=t[6]-0x48c00000
        assert 0<=base and base+data_bytes<=36*1024*1024
        images.append(dict(chip=chip,core=core,slot=slot,stage=stage,base=base,count=count,
                           stride=stride,data_bytes=data_bytes,file=filename,sha256=sha(path),expert_hashes=hashes))
        lines.append(f'{chip} {core} {stage} {base} {count} {stride} {data_bytes} {filename}\n')
        if (number+1)%16==0: print(f'complete model images {number+1}/768',flush=True)
    assert len(images)==768 and len(shared_hashes)==128
    # Each logical core's FC13 and FC2 allocations must not overlap.
    for chip,core in logical:
        regions=sorted((i['base'],i['base']+i['data_bytes']) for i in images if (i['chip'],i['core'])==(chip,core))
        assert len(regions)==2 and regions[0][1]<=regions[1][0]
    (root/'layout.tsv').write_text(''.join(lines))
    manifest=dict(version=VERSION,model_seed=MODEL_SEED,config_sha256=key,images=images,
                  routed_experts=256,shared_replicas=6,shared_identical=True,
                  coefficients='all positions, signed nonzero FP8 codes, E8M0 125..128',
                  fc13_shape=[4096,6144],fc2_shape=[6144,2048],
                  total_data_bytes=sum(i['data_bytes'] for i in images),layout_sha256=sha(root/'layout.tsv'))
    manifest_path.write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest


class DenseModel:
    def __init__(self,root,manifest):
        self.images={}
        for entry in manifest['images']:
            mm=np.memmap(root/entry['file'],dtype=np.uint8,mode='r')
            self.images[entry['chip'],entry['core'],entry['stage']]=(entry,mm)

    def matrix(self,chip,core,stage,local):
        entry,mm=self.images[chip,core,stage]
        rows,kdim=(512,768) if stage==0 else (768,256)
        nb,kb=rows//128,kdim//64; stride=rows*kdim
        values=mm[local*stride:(local+1)*stride].reshape(nb,kb,128,64).transpose(0,2,1,3).reshape(rows,kdim)
        begin=entry['data_bytes']+local*stride//32
        scales=mm[begin:begin+stride//32].reshape(nb,kb,128,2).transpose(0,2,1,3).reshape(rows,kdim//32)
        return values,scales


def dense_dot(values,scales,codes,input_scales):
    # Independent NumPy arithmetic: 32-element scale blocks, two blocks per
    # K64 primitive, FP32 accumulation in increasing primitive order.
    rows,kdim=values.shape
    products=(FP8[values]*FP8[codes][None,:]).reshape(rows,kdim//32,32)
    block=products.sum(axis=2,dtype=np.float32)
    multiplier=np.exp2(scales.astype(np.int32)+input_scales.astype(np.int32)[None,:]-254).astype(np.float32)
    block=(block*multiplier).astype(np.float32)
    primitives=(block[:,0::2]+block[:,1::2]).astype(np.float32)
    result=primitives[:,0].copy()
    for i in range(1,primitives.shape[1]): result=(result+primitives[:,i]).astype(np.float32)
    return result


def input_arrays(spec):
    rng=np.random.default_rng(spec['input_seed']); profile=spec['profile']
    codes=rng.choice(np.array([0x28,0x30,0x38,0x3c,0x40],np.uint8),6144)
    if profile=='negative': codes|=128
    if profile in ('signed','small','large','mixed_scale'): codes|=rng.integers(0,2,6144,dtype=np.uint8)*128
    if profile=='alternating': codes|=(np.arange(6144)%2).astype(np.uint8)*128
    if profile=='zero': codes[:]=0
    scales=np.full(192,127,np.uint8)
    if profile=='small': scales[:]=124
    if profile=='large': scales[:]=129
    if profile=='mixed_scale': scales=rng.integers(124,130,192,dtype=np.uint8)
    return codes,scales


def reference(spec,model,logical,routes):
    codes,scales=input_arrays(spec); partial={}; gates={}; shared_vectors={}
    for ep in range(6):
        if not spec['mask']>>ep&1: continue
        experts=([0] if ep==spec['shared_ep'] else [])+[i+1 for i in spec['expert_ids'][ep]]
        for local in experts:
            for part in range(8):
                chip=ep*8+part; sources={}
                for slot in range(8):
                    _,core=routes.reverse[chip,slot]
                    values,sf=model.matrix(chip,core,0,local)
                    sources[chip,core]=dense_dot(values,sf,codes[slot*768:(slot+1)*768],scales[slot*24:(slot+1)*24])
                merged=widen(bf16(routes.reduce(routes.fc13[chip],sources)))
                g,u=merged[:256],merged[256:]
                with np.errstate(over='ignore'):
                    activation=quantize((g/(1+np.exp(-g,dtype=np.float32))*u).astype(np.float32))
                # Preserve the encoded MXFP8 activation and its block scales.
                # Quantizing twice preserves the decoded result but can choose
                # another scale for a rounded block peak. Recover the original
                # encoding directly from the pre-quantization activation instead.
                with np.errstate(over='ignore'):
                    raw=(g/(1+np.exp(-g,dtype=np.float32))*u).astype(np.float32)
                ac,asc=encode_activation(raw)
                assert np.array_equal(FP8[ac]*np.repeat(np.exp2(asc.astype(int)-127).astype(np.float32),32),activation)
                gates[ep,local,part]=merged
                coefficient=1. if local==0 else spec['topk_weights'][ep][spec['expert_ids'][ep].index(local-1)]
                for slot in range(8):
                    _,core=routes.reverse[chip,slot]
                    values,sf=model.matrix(chip,core,1,local)
                    y=(dense_dot(values,sf,ac,asc)*np.float32(coefficient)).astype(np.float32)
                    key=ep,part,slot
                    partial[key]=(partial.get(key,np.zeros(768,np.float32))+y).astype(np.float32)
                    if local==0: shared_vectors[part,slot]=y
    total=np.zeros(6144,np.float32)
    for ep in range(6):
        if not spec['mask']>>ep&1: continue
        group=np.zeros(6144,np.float32)
        for slot in range(8):
            sources={routes.reverse[ep*8+part,slot]:partial.get((ep,part,slot),np.zeros(768,np.float32)) for part in range(8)}
            group[slot*768:(slot+1)*768]=routes.reduce(routes.fc2[ep,slot],sources)
        total=widen(bf16(total+group))
    assert np.isfinite(total).all()
    return bf16(total),hashlib.sha256(b''.join(shared_vectors[key].tobytes() for key in sorted(shared_vectors))).hexdigest()


def encode_activation(raw):
    blocks=raw.reshape(-1,32); peaks=np.max(np.abs(blocks),axis=1)
    exponent=np.ceil(np.log2(np.maximum(peaks,np.float32(2.**-126))/448)).astype(int)
    exponent=np.where(peaks==0,0,exponent)
    scaled=(blocks/np.exp2(exponent).astype(np.float32)[:,None]).reshape(-1)
    distance=np.abs(np.abs(scaled)[:,None]-FP8[:127]); distance[:,1::2]=np.nextafter(distance[:,1::2],np.float32(np.inf))
    codes=np.argmin(distance,axis=1).astype(np.uint8)|np.where(np.signbit(scaled),128,0).astype(np.uint8)
    return codes,(exponent+127).astype(np.uint8)


def fixture(spec,out):
    codes,scales=input_arrays(spec); path=out/(spec['name']+'.fixture')
    raw=bytearray(b'PCDTST02'+struct.pack('<III',spec['user'],spec['token'],spec['mask']))
    for ids,weights in zip(spec['expert_ids'],spec['topk_weights']):
        raw+=struct.pack('<I',len(ids))
        for expert,weight in zip(ids,weights): raw+=struct.pack('<If',expert,weight)
    raw+=codes.tobytes()+scales.tobytes()+struct.pack('<I',0)
    path.write_bytes(raw)
    assert spec['user']%6==spec['shared_ep']
    assert spec['actual_ep_mask']==spec['routed_mask']|(1<<spec['shared_ep'])
    metadata=dict(spec,fixture_sha256=sha(path),matrix_records_in_fixture=0,
                  input_positive=int(np.count_nonzero(FP8[codes]>0)),
                  input_negative=int(np.count_nonzero(FP8[codes]<0)),
                  input_zero=int(np.count_nonzero((codes&127)==0)),
                  input_scale_exponents=sorted(set(map(int,scales.astype(int)-127))))
    (out/(spec['name']+'.json')).write_text(json.dumps(metadata,indent=2)+'\n')
    return metadata


def run(args):
    assert 1 <= args.workers <= 4 and 1 <= args.threads <= 16
    args.out.mkdir(parents=True,exist_ok=True)
    templates,logical=layout(args.bundle,args.catalog); routes=PrecisionRoutes(args.bundle,args.catalog,logical)
    manifest=prepare_model(args.model,templates,logical); model=DenseModel(args.model,manifest)
    all_specs=specifications(); specs=[s for s in all_specs if args.filter in s['name']]
    if args.limit: specs=specs[:args.limit]
    assert specs
    (args.out/'planned_cases.json').write_text(json.dumps(all_specs,indent=2)+'\n')
    bundle_digest=hashlib.sha256()
    for path in sorted([args.bundle/'pcode_ep6_tp8.bachir',*args.bundle.glob('*.hex')]):
        bundle_digest.update(path.name.encode()+b'\0'+path.read_bytes())
    identity=dict(bundle_sha256=bundle_digest.hexdigest(),simulator_sha256=sha(args.simulator),
                  campaign_sha256=sha(__file__),reference_helpers_sha256=sha(Path(__file__).with_name('pcode_campaign.py')),
                  precision_routes_sha256=sha(Path(__file__).with_name('pcode_precision_reference.py')),
                  model_manifest_sha256=sha(args.model/'manifest.json'))
    def execute(spec):
        name=spec['name']; report_path=args.out/(name+'.report.json')
        if args.resume and report_path.exists():
            previous=json.loads(report_path.read_text())
            if previous.get('passed') and previous.get('threads')==args.threads and all(previous.get(k)==v for k,v in identity.items()):
                assert sha(args.out/(name+'.fixture'))==previous['fixture_sha256']
                assert sha(args.out/(name+'.actual.bf16'))==previous['actual_sha256']
                return previous
        start=time.monotonic(); meta=fixture(spec,args.out)
        expected,shared_hash=reference(spec,model,logical,routes)
        expected_path=args.out/(name+'.expected.bf16'); expected_path.write_bytes(expected.astype('<u2').tobytes())
        actual=args.out/(name+'.actual.bf16'); actual.unlink(missing_ok=True)
        env=dict(os.environ,PCODE_BUNDLE_ROOT=str(args.bundle.parent.resolve()),
                 PCODE_CASE_FILE=str((args.out/(name+'.fixture')).resolve()),PCODE_DENSE_MODEL=str(args.model.resolve()),
                 PCODE_ACTUAL=str(actual.resolve()),PCODE_THREADS=str(args.threads),PCODE_MAX_CYCLES='50000')
        print(f'start {name}: routed=0x{spec["routed_mask"]:02x}, shared={spec["shared_ep"]}, actual=0x{spec["mask"]:02x}',flush=True)
        try:
            with (args.out/(name+'.log')).open('w') as log:
                process=subprocess.run([str(args.simulator.resolve())],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=args.timeout)
            code=process.returncode
        except subprocess.TimeoutExpired: code=124
        text=(args.out/(name+'.log')).read_text()
        report=dict(meta,**identity,simulator_returncode=code,threads=args.threads,
                    wall_seconds=time.monotonic()-start,expected_sha256=sha(expected_path),shared_reference_sha256=shared_hash)
        stopped=re.search(r'pcode stopped at (\d+), output packets (\d+)',text)
        if stopped: report.update(completed_cycles=int(stopped[1]),output_packets=int(stopped[2]))
        preload=re.search(r'pcode dense model images (\d+), data bytes (\d+)',text)
        report['all_model_images_preloaded']=bool(preload and int(preload[1])==768 and int(preload[2])==manifest['total_data_bytes'])
        masks=re.search(r'pcode mask routed (\d+), shared EP (\d+), actual (\d+)',text)
        report['native_mask_verified']=bool(masks and tuple(map(int,masks.groups())) ==
                                           (spec['routed_mask'],spec['shared_ep'],spec['actual_ep_mask']))
        if code==0 and actual.exists(): report.update(compare(actual,expected_path))
        else: report.update(passed=False,error='simulator failed or output missing')
        report['passed']=bool(report['passed'] and report['all_model_images_preloaded'] and
                             report['native_mask_verified'] and report.get('output_packets')==1)
        report_path.write_text(json.dumps(report,indent=2)+'\n')
        return report
    reports=[]
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures=[pool.submit(execute,spec) for spec in specs]
        for future in as_completed(futures):
            report=future.result();reports.append(report)
            write_summary(args.out,reports,len(specs),identity)
            print(f'{len(reports)}/{len(specs)} {report["name"]}: {"PASS" if report["passed"] else "FAIL"} cos={report.get("cosine_similarity")} L2={report.get("relative_l2_error")} {report["wall_seconds"]:.1f}s',flush=True)
            if not report['passed']:
                for pending in futures: pending.cancel()
                raise SystemExit(1)
    # The shared reference contribution must be identical in all six logical
    # placements; final outputs may differ due to inter-EP BF16 rounding.
    replica=[r for r in reports if r['family']=='shared_replica']
    if len(replica)==6: assert len({r['shared_reference_sha256'] for r in replica})==1
    relations=[]
    for group in ['order_0','order_1','order_2','shared_replicas']:
        members=sorted([r for r in reports if r.get('relation')==group],key=lambda r:r['name'])
        if len(members)<2: continue
        for member in members[1:]:
            if group=='shared_replicas':
                metrics=shared_difference_check(args.out/(members[0]['name']+'.actual.bf16'),
                                               args.out/(member['name']+'.actual.bf16'),
                                               args.out/(members[0]['name']+'.expected.bf16'),
                                               args.out/(member['name']+'.expected.bf16'))
            else:
                metrics=compare(args.out/(member['name']+'.actual.bf16'),
                                args.out/(members[0]['name']+'.actual.bf16'))
            relations.append(dict(group=group,first=members[0]['name'],second=member['name'],**metrics))
            assert metrics['passed'],f'cross-case consistency failed: {group}'
    summary=json.loads((args.out/'summary.json').read_text())
    summary['relations']=relations
    summary['relations_passed']=all(r['passed'] for r in relations)
    (args.out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')


def write_summary(out,reports,planned,identity):
    cos=[r['cosine_similarity'] for r in reports if r.get('cosine_similarity') is not None]
    summary=dict(total=len(reports),planned=planned,passed=sum(r['passed'] for r in reports),
                 failed=[r['name'] for r in reports if not r['passed']],identity=identity,
                 coverage=dict(Counter(r['family'] for r in reports)),
                 minimum_nonzero_cosine=min(cos) if cos else None,
                 maximum_relative_l2=max((r.get('relative_l2_error',0) for r in reports),default=0),
                 cases=reports)
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--bundle',type=Path,required=True); parser.add_argument('--catalog',type=Path,required=True)
    parser.add_argument('--model',type=Path,default=Path('build-ep6/glm5-dense-model'))
    parser.add_argument('--out',type=Path,default=Path('build-ep6/glm5-dense-96'))
    parser.add_argument('--simulator',type=Path,default=Path('build-ep6/pcode_bundle'))
    parser.add_argument('--workers',type=int,default=2)
    parser.add_argument('--threads',type=int,default=2); parser.add_argument('--timeout',type=int,default=1200)
    parser.add_argument('--filter',default=''); parser.add_argument('--limit',type=int); parser.add_argument('--resume',action='store_true')
    run(parser.parse_args())
