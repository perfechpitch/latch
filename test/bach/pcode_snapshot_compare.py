"""Compare the supported addresses in optional, post-Join debug snapshots.

These snapshots are diagnostic only. The complete numerical acceptance uses
the final output packet, not these buffers. The current C++ dumper samples
physical core 9 and FC2 address 0x1200; skip other placements explicitly.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from pcode_campaign import FP8, widen, layout

p=argparse.ArgumentParser()
p.add_argument('case',type=Path)
p.add_argument('--bundle',type=Path,required=True)
p.add_argument('--catalog',type=Path,required=True)
a=p.parse_args()
meta=json.loads(a.case.with_suffix('.json').read_text())
gold=np.load(a.case.with_suffix('.intermediates.npz'))
templates,logical=layout(a.bundle,a.catalog)
for chip in range(48):
    if not meta['mask']>>(chip//8)&1:continue
    base=Path(str(a.case)+f'.snapshot.chip{chip}')
    if logical.get((chip,9)) != 7:
        print(chip,'SKIP: FC13/VU are not on sampled physical core 9')
        continue
    gate=widen(np.fromfile(str(base)+'.fc13',dtype='<u2')).reshape(9,512)
    q=FP8[np.fromfile(str(base)+'.quant',dtype=np.uint8)]
    sf=np.fromfile(str(base)+'.scales',dtype=np.uint8)
    q=(q*np.repeat(np.exp2(sf.astype(int)-127),32)).reshape(9,256)
    count=meta['counts'][chip//8]+1
    for stage,actual,expected in [('FC13',gate,gold['fc13'][chip]),('quant',q,gold['quant'][chip])]:
        diff=actual[:count]-expected[:count]
        nz=np.argwhere(diff!=0)
        print(chip,stage,'mismatch',len(nz),'max',np.max(np.abs(diff)),
              'L2',np.linalg.norm(diff)/(np.linalg.norm(expected[:count]) or 1))
        if len(nz):
            e,i=nz[0];print(' first',e,i,'actual',actual[e,i],'expected',expected[e,i])
    for (c,core),slot in sorted(logical.items()):
        if c!=chip:continue
        if templates[c,core,1][3] != 0x1200:
            print(' FC2',chip,core,slot,'SKIP: C address differs from sampled 0x1200')
            continue
        actual=np.fromfile(str(base)+f'.core{core}.fc2',dtype='<f4')
        expected=gold['fc2'][chip,slot]
        diff=actual-expected
        if np.max(np.abs(diff))>1e-5:
            print(' FC2',chip,core,slot,'max',np.max(np.abs(diff)),
                  'L2',np.linalg.norm(diff)/(np.linalg.norm(expected) or 1))
