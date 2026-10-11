"""Reproducible, isolated EP6/TP8 single-token numerical campaign.

Fixtures contain quantized input and compact sparse matrix rows. The independent
NumPy oracle computes the GEMVs, nonlinear activation and MXFP8 quantization;
it does not import simulator or pcode implementations.
"""
from concurrent.futures import ThreadPoolExecutor, as_completed
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import random
import re
import struct
import subprocess
import time
import numpy as np
from pcode_precision_reference import PrecisionRoutes

FP8 = np.array([(c&7)*2.**-9 if (c&127)>>3 == 0 else
                (1+(c&7)/8)*2.**(((c&127)>>3)-7) for c in range(256)],np.float32)
FP8[128:] *= -1
FP8[127] = FP8[255] = np.nan


def bf16(a):
    a=np.asarray(a,np.float32);u=a.view(np.uint32)
    return ((u+0x7fff+((u>>16)&1))>>16).astype(np.uint16)


def widen(a):
    return (np.asarray(a,np.uint16).astype(np.uint32)<<16).view(np.float32)


def quantize(a):
    a=np.asarray(a,np.float32).reshape(-1,32)
    peaks=np.max(np.abs(a),axis=1)
    exponents=np.ceil(np.log2(np.maximum(peaks, np.float32(2.**-126))/448)).astype(int)
    exponents=np.where(peaks==0,0,exponents)
    scales=np.exp2(exponents).astype(np.float32)
    scaled=(a/scales[:,None]).reshape(-1)
    candidates=FP8[:127]
    distance=np.abs(np.abs(scaled)[:,None]-candidates)
    # FP8 RNE: prefer an even LSB on a tie.
    distance[:,1::2]=np.nextafter(distance[:,1::2],np.float32(np.inf))
    codes=np.argmin(distance,axis=1).astype(np.uint8)
    codes |= np.where(np.signbit(scaled),128,0).astype(np.uint8)
    return (FP8[codes].reshape(-1,32)*scales[:,None]).reshape(-1)


def layout(bundle,catalog):
    tpl={};bad={}
    for line in (bundle/'pcode_ep6_tp8.bachir').read_text().splitlines():
        t=line.split()
        if not t:continue
        if t[0]=='CHIP':bad[int(t[1])]=int(t[2],0)
        if t[0]=='DSAREG' and t[3]=='1' and 0x1000<=int(t[4],0)<0x1100:
            off=int(t[4],0);key=(int(t[1]),int(t[2]),(off-0x1000)//128)
            fields=tpl.setdefault(key,[0]*10);f=(off-0x1000)%128//4
            if f<10:fields[f]=int(t[5],0)
    cat=json.loads(catalog.read_text());logical={}
    for chip in range(48):
        hs=cat['chips'][f'c{chip}']['harvests']
        h=next(v for k,v in sorted(hs.items(),key=lambda kv:int(kv[0][1:])) if v['disable_mask']==bad[chip])
        for pk,core in h['cores'].items():
            ls={int(e['l']) for e in core.get('entries',[]) if e.get('l') is not None and 0<=int(e['l'])<8}
            if ls:
                assert len(ls)==1, f'ambiguous logical core: chip {chip}, {pk}'
                logical[chip,int(pk[1:])]=next(iter(ls))
    return tpl,logical


def specifications(random_count):
    cases=[]
    for mask in range(1,64):
        active=[e for e in range(6) if mask>>e&1];counts=[0]*6
        for i in range(8):counts[active[i%len(active)]]+=1
        shared=active[-1] if mask%2==0 else active[0]
        user=shared+6*((mask*13)%170)
        cases.append(dict(name=f'mask_{mask:02x}',family='routing',mask=mask,counts=counts,user=user,seed=mask,profile='routing'))
    for shared in range(6):
        cases.append(dict(name=f'shared_only_{shared}',family='boundary',mask=1<<shared,counts=[0]*6,user=shared,seed=100+shared,profile='routing'))
    cases.append(dict(name='identity_1023',family='boundary',mask=63,counts=[2,2,1,1,1,1],user=1023,seed=1023,profile='routing'))
    profiles=['signed','positive','negative','zero','small','large','cancellation','mixed_scale']
    for seed in range(random_count):
        r=random.Random(20261010+seed);shared=seed%6;counts=[0]*6
        for _ in range(8):counts[r.randrange(6)]+=1
        mask=(1<<shared)|sum(1<<e for e,c in enumerate(counts) if c)
        cases.append(dict(name=f'random_{seed:03d}',family='numerical',mask=mask,counts=counts,user=shared+6*(seed%170),seed=20261010+seed,profile=profiles[seed%len(profiles)]))
    return cases


def make_fixture(spec,tpl,logical,directory,routes=None):
    rng=np.random.default_rng(spec['seed']);profile=spec['profile'];routing=profile=='routing'
    ids=[];weights=[]
    for ep,count in enumerate(spec['counts']):
        limit=41 if ep==5 else 43
        selected=list(range(count)) if routing else rng.choice(limit,count,replace=False).tolist()
        if count and spec['seed']%3==0:
            selected=list(range(limit-count,limit))[::-1]
        ids.append(selected)
        w=[.125]*count if routing else rng.integers(1,9,count).astype(float).tolist()
        weights.append(w)
    if not routing:
        norm=sum(map(sum,weights));weights=[[float(np.float32(v/norm)) for v in g] for g in weights]
    codes=np.full(6144,0x38,np.uint8);sf=np.full(192,127,np.uint8)
    if not routing:
        codes=rng.choice(np.array([0,0x28,0x30,0x38,0x3c,0x40],np.uint8),6144)
        if profile in ('signed','negative','cancellation','mixed_scale'):
            codes |= (np.full(6144,128,np.uint8) if profile=='negative' else rng.integers(0,2,6144,dtype=np.uint8)*128)
        if profile=='zero':codes[:]=0
        if profile=='small':sf[:]=124
        if profile=='large':sf[:]=130
        if profile=='mixed_scale':sf=rng.integers(124,131,192,dtype=np.uint8)
    x=FP8[codes]*np.repeat(np.exp2(sf.astype(int)-127).astype(np.float32),32)
    records=[];gate={};gate_sources={};fc2_records=[]
    for (chip,core,stage),t in sorted(tpl.items()):
        ep=chip//8;part=chip%8;slot=logical[chip,core]
        if not (spec['mask']>>ep&1):continue
        rows=(t[0]&65535)*128;kdim=(t[0]>>16)*64
        experts=[0]+[i+1 for i in ids[ep]]
        for expert in experts:
            # Paired tokens use an immutable common model: its coefficients
            # depend on the matrix identity, never on token/order/topK.
            matrix_seed=spec.get('matrix_seed',spec['seed'])
            matrix_profile=spec.get('matrix_profile',profile)
            wrng=(np.random.default_rng(np.random.SeedSequence([matrix_seed,chip,core,stage,expert]))
                  if 'matrix_seed' in spec else rng)
            if routing: indices=np.array([0],np.uint32)
            else:
                tile=(matrix_seed+slot+part+stage)%(kdim//64)
                other=(tile+kdim//128)%(kdim//64)
                indices=np.array([tile*64, tile*64+31,other*64+32,other*64+63],np.uint32)
                if (matrix_seed-20261010)%16==0:
                    # Exercise accumulation through every K tile, including
                    # both scale blocks and the final element of each tile.
                    indices=np.array([k*64+i for k in range(kdim//64)
                                      for i in (0,31,32,63)],np.uint32)
            taps=len(indices)
            if routing:
                values=np.full((rows,1),0x20,np.uint8) # 1/8
                if stage==1:
                    coeff=.03125 if expert==0 else (ep+1)/64
                    code=int(np.argmin(np.abs(FP8[:127]-coeff)))
                    values[:]=code
                    if expert:values[np.arange(rows)%128%6!=ep]=0
                    values[np.arange(rows)%2==1]|=128
                scales=np.full((rows,taps),127,np.uint8)
            else:
                pool=np.array([0,0x08,0x10,0x18,0x20],np.uint8) # 1/64..1/8
                values=wrng.choice(pool,(rows,taps))
                if stage==1:
                    # A column's sign is consistent across TP contributors;
                    # activation signs still exercise arithmetic cancellation.
                    values[np.arange(rows)%2==1]|=128
                scales=np.full((rows,taps),127,np.uint8)
                if matrix_profile=='mixed_scale':
                    scales[:, :]=wrng.integers(125,129,(rows,1),dtype=np.uint8)
            records.append((chip,core,stage,expert,rows,indices,values,scales))
            decoded=FP8[values]*np.exp2(scales.astype(int)-127).astype(np.float32)
            if stage==0:
                y=np.sum(decoded*x[slot*768+indices][None,:],axis=1,dtype=np.float32)
                key=(ep,expert,part)
                gate[key]=gate.get(key,np.zeros(rows,np.float32))+y
                gate_sources[chip,core,expert]=y
            else:fc2_records.append((ep,expert,part,slot,indices,decoded))
    if routes is not None:
        for ep,expert,part in gate:
            chip=ep*8+part
            values={(c,core):gate_sources[c,core,expert] for c,core in logical if c==chip}
            gate[ep,expert,part]=routes.reduce(routes.fc13[chip],values)
    activated={}
    for key,y in gate.items():
        if key[1]==0 and key[0]!=spec['user']%6:y=np.zeros_like(y)
        y=widen(bf16(y));g,u=y[:256],y[256:]
        silu=g/(1+np.exp(-g,dtype=np.float32))
        activated[key]=quantize((silu*u).astype(np.float32))
    partial={}
    for ep,expert,part,slot,indices,decoded in fc2_records:
        q=activated[ep,expert,part]
        y=np.sum(decoded*q[indices][None,:],axis=1,dtype=np.float32)
        ew=1. if expert==0 and ep==spec['user']%6 else 0. if expert==0 else weights[ep][ids[ep].index(expert-1)]
        y=(y*np.float32(ew)).astype(np.float32)
        key=ep,part,slot
        partial[key]=partial.get(key,np.zeros(768,np.float32))+y
    # Honor the configured FP32/BF16 transport conversions before quantizing
    # or adding the next EP. The reference never uses simulator-produced data.
    reference=np.zeros(6144,np.float32)
    for ep in range(6):
        group=np.zeros(6144,np.float32)
        for slot in range(8):
            if routes is not None:
                values={routes.reverse[ep*8+part,slot]:partial.get((ep,part,slot),np.zeros(768,np.float32)) for part in range(8)}
                group[slot*768:(slot+1)*768]=routes.reduce(routes.fc2[ep,slot],values)
            else:
                for part in range(8):group[slot*768:(slot+1)*768]+=partial.get((ep,part,slot),0)
        reference=widen(bf16(reference+group)) if routes is not None else reference+group
    expected=bf16(reference)
    gates=np.zeros((48,9,512),np.float32)
    quants=np.zeros((48,9,256),np.float32)
    fc2s=np.zeros((48,8,768),np.float32)
    for ep in range(6):
        for part in range(8):
            chip=ep*8+part
            for index,expert in enumerate([0]+[i+1 for i in ids[ep]]):
                key=ep,expert,part
                if key in gate:
                    gates[chip,index]=widen(bf16(gate[key])) if expert or ep==spec['user']%6 else 0
                    quants[chip,index]=activated[key]
            for slot in range(8):fc2s[chip,slot]=partial.get((ep,part,slot),0)
    np.savez_compressed(directory/(spec['name']+'.intermediates.npz'),fc13=gates,quant=quants,fc2=fc2s)
    f=directory/(spec['name']+'.fixture')
    with f.open('wb') as out:
        def u32(*v):out.write(struct.pack('<'+'I'*len(v),*map(int,v)))
        out.write(b'PCDTST02');u32(spec['user'],spec.get('token',1000+spec['seed']%100000),spec['mask'])
        for ep in range(6):
            u32(len(ids[ep]))
            for eid,w in zip(ids[ep],weights[ep]):u32(eid);out.write(struct.pack('<f',w))
        out.write(codes.tobytes());out.write(sf.tobytes());u32(len(records))
        for chip,core,stage,expert,rows,indices,values,scales in records:
            u32(chip,core,stage,expert,rows,len(indices));out.write(indices.astype('<u4').tobytes());out.write(values.tobytes());out.write(scales.tobytes())
    (directory/(spec['name']+'.expected.bf16')).write_bytes(expected.astype('<u2').tobytes())
    meta=dict(spec,expert_ids=ids,topk_weights=weights,fixture_sha256=hashlib.sha256(f.read_bytes()).hexdigest(),records=len(records))
    (directory/(spec['name']+'.json')).write_text(json.dumps(meta,indent=2)+'\n')
    return meta


def compare(actual,expected,exact=False):
    raw=actual.read_bytes();assert len(raw)==12288,f'output has {len(raw)} bytes'
    aw=np.frombuffer(raw,dtype='<u2');ew=np.frombuffer(expected.read_bytes(),dtype='<u2')
    a=widen(aw).astype(np.float64);e=widen(ew).astype(np.float64)
    assert np.all(np.isfinite(a)), 'nonfinite output'
    an=np.linalg.norm(a);en=np.linalg.norm(e);zero=en==0
    cosine=float(np.dot(a,e)/(an*en)) if an and en else None
    relative=float(np.linalg.norm(a-e)/en) if en else float(np.linalg.norm(a))
    mismatch=int(np.count_nonzero(aw!=ew))
    if not zero and mismatch==0:cosine=1.0
    error=float(np.max(np.abs(a-e)))
    passed=(np.count_nonzero(a)==0 if zero else cosine is not None and cosine>=.99999 and relative<=.02)
    if exact:passed=passed and mismatch==0
    return dict(passed=bool(passed),cosine_similarity=cosine,min_cosine=.99999,relative_l2_error=relative,max_relative_l2=.02,max_absolute_error=error,bf16_mismatches=mismatch,elements=6144,zero_reference=bool(zero),actual_sha256=hashlib.sha256(raw).hexdigest())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--catalog', type=Path, required=True)
    parser.add_argument('--out', type=Path, default=Path('build-ep6/campaign'))
    parser.add_argument('--random-cases', type=int, default=64)
    parser.add_argument('--filter', default='')
    parser.add_argument('--limit', type=int)
    parser.add_argument('--threads', type=int, default=16)
    parser.add_argument('--workers', type=int, default=1)
    parser.add_argument('--resume', action='store_true')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    tpl, logical = layout(args.bundle, args.catalog)
    routes = PrecisionRoutes(args.bundle, args.catalog, logical)
    specs = [s for s in specifications(args.random_cases) if args.filter in s['name']][:args.limit]
    assert 1 <= args.workers <= 4 and 1 <= args.threads <= 16
    if not specs:
        parser.error('no matching cases')
    identity = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in
                [('bundle_sha256', args.bundle/'pcode_ep6_tp8.bachir'),
                 ('simulator_sha256', Path('build-ep6/pcode_bundle')),
                 ('campaign_sha256', Path(__file__)),
                 ('precision_reference_sha256', Path(__file__).with_name('pcode_precision_reference.py'))]}

    bundle_hash = hashlib.sha256()
    for path in sorted([args.bundle/'pcode_ep6_tp8.bachir', *args.bundle.glob('*.hex')]):
        bundle_hash.update(path.name.encode()+b'\0'+path.read_bytes())
    identity['bundle_sha256'] = bundle_hash.hexdigest()

    def execute(spec):
        name = spec['name']
        report_path = args.out/(name+'.report.json')
        if args.resume and report_path.exists():
            previous = json.loads(report_path.read_text())
            if previous.get('passed') and previous.get('threads')==args.threads and all(previous.get(k)==v for k,v in identity.items()):
                return previous
        if report_path.exists():
            previous=json.loads(report_path.read_text())
            tag='.'.join(str(previous.get(k,'legacy'))[:12] for k in
                         ('simulator_sha256','campaign_sha256','fixture_sha256'))
            history=args.out/'history'/(name+'.'+tag)
            history.mkdir(parents=True,exist_ok=True)
            for suffix in ('fixture','json','expected.bf16','actual.bf16','intermediates.npz','log','report.json'):
                source=args.out/(name+'.'+suffix)
                if source.exists():(history/source.name).write_bytes(source.read_bytes())
        start = time.monotonic()
        meta = make_fixture(spec, tpl, logical, args.out, routes)
        actual = args.out/(name+'.actual.bf16')
        actual.unlink(missing_ok=True)
        env = dict(os.environ, PCODE_BUNDLE_ROOT=str(args.bundle.parent.resolve()),
                   PCODE_CASE_FILE=str((args.out/(name+'.fixture')).resolve()),
                   PCODE_ACTUAL=str(actual.resolve()), PCODE_THREADS=str(args.threads),
                   PCODE_MAX_CYCLES='30000')
        try:
            with (args.out/(name+'.log')).open('w') as log:
                run = subprocess.run(['./build-ep6/pcode_bundle'], env=env,
                                     stdout=log, stderr=subprocess.STDOUT, timeout=600)
            code = run.returncode
        except subprocess.TimeoutExpired:
            code = 124
        report = dict(meta, **identity, simulator_returncode=code,
                      wall_seconds=time.monotonic()-start, threads=args.threads)
        match = re.search(r'pcode stopped at (\d+), output packets (\d+)',
                          (args.out/(name+'.log')).read_text())
        if match:
            report.update(completed_cycles=int(match[1]), output_packets=int(match[2]))
        try:
            if code==0 and actual.exists():
                report.update(compare(actual, args.out/(name+'.expected.bf16'),
                                      exact=spec['family']!='numerical'))
            else:
                report.update(passed=False, error='simulator failed or output missing')
        except (AssertionError, ValueError) as error:
            report.update(passed=False, error=str(error))
        report_path.write_text(json.dumps(report, indent=2)+'\n')
        return report

    reports = []
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(execute, spec) for spec in specs]
        for future in as_completed(futures):
            report = future.result()
            reports.append(report)
            summary = dict(total=len(reports), planned=len(specs),
                           passed=sum(r['passed'] for r in reports),
                           failed=[r['name'] for r in reports if not r['passed']],
                           cases=sorted(reports, key=lambda r:r['name']))
            (args.out/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
            print(f"{len(reports)}/{len(specs)} {report['name']}: "
                  f"{'PASS' if report['passed'] else 'FAIL'} "
                  f"cosine={report.get('cosine_similarity')} "
                  f"L2={report.get('relative_l2_error')} {report['wall_seconds']:.1f}s", flush=True)
    if summary['failed']:
        raise SystemExit(1)

if __name__=='__main__':
    main()
