"""Two tokens in one simulator, immutable model, independent per-token oracle."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import time

from pcode_campaign import specifications, layout, make_fixture, compare
from pcode_precision_reference import PrecisionRoutes


def pairs():
    single = {s['name']: s for s in specifications(64)}
    result = []
    def add(name, left, right, mode='overlap', gap=1, same_user=False, model=None):
        a, b = dict(single[left]), dict(single[right])
        if same_user:
            assert mode == 'sequential'
            owner = a['user'] % 6
            b['user'] = a['user']
            b['mask'] |= 1 << owner
        for index, spec in enumerate((a, b)):
            spec.update(name=f'{name}.token{index}', token=50000+2*len(result)+index)
            if model is not None:
                spec.update(matrix_seed=model, matrix_profile='mixed_scale')
        result.append(dict(name=name, mode=mode, gap=gap, tokens=[a,b]))
    routes = [('mask_3f','mask_2b'),('mask_2b','mask_3f'),
              ('mask_01','mask_20'),('mask_20','mask_01'),
              ('mask_11','mask_2a'),('mask_2a','mask_11')]
    for i,(a,b) in enumerate(routes):
        add(f'route_overlap_{i:02}',a,b)
        add(f'route_sequential_{i:02}',a,b,'sequential')
    for i in range(8):
        # Same immutable matrix coefficients even when IDs/topK/input change.
        model = 20261010 + (16*i if i < 2 else i)
        a,b=f'random_{i:03}',f'random_{i+8:03}'
        add(f'numeric_overlap_{i:02}',a,b,model=model)
        add(f'numeric_gap_{i:02}',b,a,gap=512,model=model)
        add(f'numeric_same_user_{i:02}',a,b,'sequential',same_user=True,model=model)
    for ep in range(6):
        add(f'shared_to_full_{ep}',f'shared_only_{ep}','mask_3f')
        add(f'full_to_shared_{ep}','mask_3f',f'shared_only_{ep}')
    # Maximum UID and same-UID reuse after a completed token.
    add('identity_1023_overlap','identity_1023','mask_2b')
    add('identity_1023_reuse','identity_1023','mask_3f','sequential',same_user=True)
    # The zero and nonzero inputs share exactly the same weights.
    add('nonzero_to_zero','random_000','random_003',model=20261042)
    add('zero_to_nonzero','random_003','random_000',model=20261042)
    return result


def read_records(path):
    """Matrix overlap audit independent of the generator's random seed logic."""
    data=path.read_bytes(); pos=8
    def u32(n=1):
        nonlocal pos
        v=struct.unpack_from('<'+'I'*n,data,pos);pos+=4*n
        return v[0] if n==1 else v
    user,token,mask=u32(3)
    for ep in range(6):
        count=u32();pos+=count*8
    pos+=6144+192
    records={}
    count=u32()
    for _ in range(count):
        chip,core,stage,expert,rows,taps=u32(6)
        size=4*taps+2*rows*taps
        records[chip,core,stage,expert]=(rows,taps,data[pos:pos+size]);pos+=size
    assert pos==len(data)
    return records


def audit_shared_model(left,right):
    a,b=read_records(left),read_records(right)
    shared=a.keys() & b.keys()
    assert all(a[k]==b[k] for k in shared),'overlapping expert matrices differ between tokens'
    return dict(common_matrices=len(shared),union_matrices=len(a.keys()|b.keys()),immutable=True)


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--bundle',type=Path,required=True)
    ap.add_argument('--catalog',type=Path,required=True)
    ap.add_argument('--out',type=Path,default=Path('build-ep6/two-token'))
    ap.add_argument('--filter',default='')
    ap.add_argument('--threads',type=int,default=2)
    ap.add_argument('--workers',type=int,default=4)
    ap.add_argument('--resume',action='store_true')
    args=ap.parse_args()
    assert 1<=args.workers<=4 and 1<=args.threads<=16
    args.out.mkdir(parents=True,exist_ok=True)
    tpl,logical=layout(args.bundle,args.catalog)
    routes=PrecisionRoutes(args.bundle,args.catalog,logical)
    cases=[p for p in pairs() if args.filter in p['name']]
    assert cases,'no matching pairs'
    identity={}
    for key,path in [('simulator',Path('build-ep6/pcode_bundle')),
                     ('runner',Path(__file__)),('fixture_generator',Path(__file__).with_name('pcode_campaign.py')),
                     ('precision_reference',Path(__file__).with_name('pcode_precision_reference.py'))]:
        identity[key+'_sha256']=hashlib.sha256(path.read_bytes()).hexdigest()
    digest=hashlib.sha256()
    for path in sorted([args.bundle/'pcode_ep6_tp8.bachir',*args.bundle.glob('*.hex')]):
        digest.update(path.name.encode()+b'\0'+path.read_bytes())
    identity['bundle_sha256']=digest.hexdigest()

    def execute(pair):
        started=time.monotonic();name=pair['name']
        report_path=args.out/(name+'.report.json')
        if args.resume and report_path.exists():
            previous=json.loads(report_path.read_text())
            valid=(previous.get('passed') and previous.get('threads')==args.threads and
                   all(previous.get(k)==v for k,v in identity.items()))
            if valid:
                for index,token in enumerate(previous['tokens']):
                    fixture=args.out/(token['name']+'.fixture')
                    output=args.out/(name+f'.actual.token{index}.bf16')
                    valid=valid and fixture.exists() and output.exists()
                    if valid:
                        valid=(hashlib.sha256(fixture.read_bytes()).hexdigest()==token['fixture_sha256'] and
                               hashlib.sha256(output.read_bytes()).hexdigest()==token['actual_sha256'])
            if valid:return previous
        meta=[make_fixture(s,tpl,logical,args.out,routes) for s in pair['tokens']]
        paths=[args.out/(m['name']+'.fixture') for m in meta]
        model_audit=audit_shared_model(*paths)
        actual=args.out/(name+'.actual')
        for index in range(2):Path(str(actual)+f'.token{index}.bf16').unlink(missing_ok=True)
        env=dict(os.environ,PCODE_BUNDLE_ROOT=str(args.bundle.parent.resolve()),
                 PCODE_CASE_FILE=str(paths[0].resolve()),PCODE_SECOND_CASE_FILE=str(paths[1].resolve()),
                 PCODE_ACTUAL=str(actual.resolve()),PCODE_THREADS=str(args.threads),
                 PCODE_MAX_CYCLES='50000',PCODE_INJECT_MODE=pair['mode'],PCODE_TOKEN_GAP=str(pair['gap']))
        log_path=args.out/(name+'.log')
        try:
            with log_path.open('w') as log:
                code=subprocess.run(['./build-ep6/pcode_bundle'],env=env,stdout=log,
                                    stderr=subprocess.STDOUT,timeout=1200).returncode
        except subprocess.TimeoutExpired:code=124
        text=log_path.read_text()
        sent={int(i):int(c) for i,c in re.findall(r'pcode sent token (\d+) at (\d+)',text)}
        received={int(i):int(c) for i,c in re.findall(r'pcode received token (\d+) at (\d+)',text)}
        stop=re.search(r'pcode stopped at (\d+), output packets (\d+)',text)
        report=dict(name=name,mode=pair['mode'],gap=pair['gap'],threads=args.threads,
                    **identity,simulator_returncode=code,model_audit=model_audit,
                    sent_at=sent,received_at=received,tokens=[],passed=False)
        if stop:report.update(completed_cycles=int(stop[1]),output_packets=int(stop[2]))
        for index,m in enumerate(meta):
            token=dict(m)
            try:
                token.update(compare(Path(str(actual)+f'.token{index}.bf16'),
                                     args.out/(m['name']+'.expected.bf16'),exact=m['family']!='numerical'))
            except (OSError,AssertionError,ValueError) as error:token.update(passed=False,error=str(error))
            report['tokens'].append(token)
        timing_ok=len(sent)==len(received)==2
        if timing_ok:
            report['overlapped']=sent[1]<received[0]
            timing_ok=(report['overlapped'] if pair['mode']=='overlap' else sent[1]>received[0])
        report['passed']=bool(code==0 and report.get('output_packets')==2 and timing_ok and
                              all(t['passed'] for t in report['tokens']))
        report['wall_seconds']=time.monotonic()-started
        report_path.write_text(json.dumps(report,indent=2)+'\n')
        return report

    reports=[]
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for f in as_completed([pool.submit(execute,p) for p in cases]):
            r=f.result();reports.append(r)
            summary=dict(total=len(reports),planned=len(cases),passed=sum(r['passed'] for r in reports),
                         failed=[r['name'] for r in reports if not r['passed']],cases=sorted(reports,key=lambda r:r['name']))
            (args.out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
            print(f"{len(reports)}/{len(cases)} {r['name']} {'PASS' if r['passed'] else 'FAIL'} "
                  f"cos={[t.get('cosine_similarity') for t in r['tokens']]} {r['wall_seconds']:.1f}s",flush=True)
    if summary['failed']:raise SystemExit(1)


if __name__=='__main__':main()
