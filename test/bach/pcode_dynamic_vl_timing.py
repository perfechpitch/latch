"""Match pre/post dynamic-VL task spans by case, physical core and token user.

The measurement includes RV, DSA and the observed TS-to-completion envelope.
The count-plus-VU span is measured on the time axis, not by summing rows.
"""
import argparse
from collections import defaultdict
import csv
import json
from pathlib import Path
import statistics
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'src/utils'))
from tracetto import spans as S
from tracetto.reader import TraceReader
from pcode_glm5_dense import sha, specifications


def extract(entry):
    meta = json.loads(Path(entry['meta']).read_text())
    catalog = json.loads(Path(entry['catalog']).read_text())
    harvest = {}
    for line in Path(entry['ir']).read_text().splitlines():
        f=line.split()
        if f and f[0]=='CHIP':
            chip,mask=int(f[1]),int(f[2],0)
            harvest[chip]=min(int(k[1:]) for k,v in catalog['chips'][f'c{chip}']['harvests'].items()
                              if v['disable_mask']==mask)
    specs = {s['name']:s for s in specifications()}
    spec = specs[entry['case']]
    rows=[]
    with TraceReader(str(Path(entry['trace']).with_suffix(''))) as reader:
        chips, signals=S.core_paths(reader)
        end=S.trace_t_end(reader,signals)
        for chip, cores in sorted(chips.items()):
            for core in sorted(cores):
                tasks=[t for t in meta.get(f'{chip*10+core},{harvest[chip]}',[])
                       if t['op'] in ('silu_dot_quant','silu_dot_quant_count')]
                if not tasks or f'{chip}.{core}' not in signals:
                    continue
                spans=S.core_spans(reader,signals[f'{chip}.{core}'],end)
                for task in tasks:
                    found=[]
                    for name,parts in S.ROWS:
                        for lane,_ in parts:
                            _,group,unit=S.LANES[lane]
                            if S.UNITS[unit]!=task['unit']:
                                continue
                            for a,b,user,tid in spans[group][unit]:
                                if tid==task['task_id'] and user==spec['user']:
                                    found.append((name,a,b))
                    if not found:
                        continue  # EP mask suppressed this core's computation.
                    assert max(b for _,_,b in found)<end
                    durations={name:b-a for name,a,b in found}
                    rows.append(dict(case=entry['case'],variant=entry['variant'],chip=chip,
                        core=core,user=spec['user'],r=spec['counts'][chip//8],
                        expected_vl=(spec['counts'][chip//8]+1)*256,
                        op=task['op'],fn=task['fn'],task=task['task_id'],
                        start=min(a for _,a,_ in found),end=max(b for _,_,b in found),
                        envelope=max(b for _,_,b in found)-min(a for _,a,_ in found),
                        rv=durations.get(task['unit']+'-Core',0),
                        dsa=durations.get(task['unit']+'-DSA',0),
                        ts=durations.get('TS-'+task['unit'],0),
                        isq=durations.get('VU-DSA-ISQ',0)))
                reader.cache.clear();reader._segs.clear()
    return rows


def run(args):
    manifest=json.loads(args.manifest.read_text())
    outputs=defaultdict(dict)
    for entry in manifest:
        outputs[entry['case']][entry['variant']]=Path(entry['actual']).read_bytes()
    output_checks=[]
    for case,variants in outputs.items():
        assert set(variants)=={'before','after'}
        assert len(variants['before'])==6144*2 and variants['before']==variants['after'],case
        entry=next(e for e in manifest if e['case']==case and e['variant']=='after')
        output_checks.append(dict(case=case,bf16_values=6144,bit_exact=True,
                                  actual_sha256=sha(Path(entry['actual']))))
    rows=[row for entry in manifest for row in extract(entry)]
    matched={}
    for row in rows:
        key=row['case'],row['chip'],row['core'],row['user']
        matched.setdefault(key,{})[row['variant'],row['op']]=row
    pairs=[]
    for key,group in sorted(matched.items()):
        before=group.get(('before','silu_dot_quant'))
        after=group.get(('after','silu_dot_quant'))
        count=group.get(('after','silu_dot_quant_count'))
        if not before or not after:
            continue
        assert count and count['dsa']==0
        assert count['end']<=after['start']
        pairs.append(dict(case=key[0],chip=key[1],core=key[2],user=key[3],r=after['r'],
            vl=after['expected_vl'],before_vu_dsa=before['dsa'],after_vu_dsa=after['dsa'],
            before_vu_rv=before['rv'],after_vu_rv=after['rv'],count_rv=count['rv'],
            count_envelope=count['envelope'],before_vu_envelope=before['envelope'],
            after_vu_envelope=after['envelope'],after_count_and_vu=after['end']-count['start']))
    assert pairs
    grouped=defaultdict(list)
    for pair in pairs:grouped[pair['r']].append(pair)
    columns=[k for k in pairs[0] if k not in ('case','chip','core','user','r','vl')]
    summary=[]
    for r,values in sorted(grouped.items()):
        summary.append(dict(r=r,vl=(r+1)*256,samples=len(values),**{
            col:dict(min=min(v[col] for v in values),median=statistics.median(v[col] for v in values),
                     max=max(v[col] for v in values)) for col in columns}))
    result=dict(status='measured',units='cycles',threads=1,summary=summary,pairs=pairs,
        output_checks=output_checks,
        source=[dict(entry,trace_sha256=sha(Path(entry['trace'])),
                     meta_sha256=sha(Path(entry['meta'])),ir_sha256=sha(Path(entry['ir'])),
                     actual_sha256=sha(Path(entry['actual'])),
                     simulator_sha256=sha(Path(entry['simulator'])),
                     fixture_sha256=sha(Path(entry['fixture'])))
                for entry in manifest])
    args.out.write_text(json.dumps(result,indent=2)+'\n')
    with args.out.with_suffix('.csv').open('w',newline='') as out:
        writer=csv.DictWriter(out,fieldnames=list(pairs[0]),lineterminator="\n");writer.writeheader();writer.writerows(pairs)
    print(json.dumps(summary,indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    run(parser.parse_args())
