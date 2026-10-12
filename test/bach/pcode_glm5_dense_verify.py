"""Rebuild all 96 references and audit the saved pcode/latch/model results."""
import argparse
from collections import Counter
from datetime import datetime
import hashlib
import json
from pathlib import Path
import struct
from zoneinfo import ZoneInfo
import numpy as np
from pcode_glm5_dense import (specifications, layout, prepare_model, DenseModel,
                              reference, compare, input_arrays, sha, OFFSETS)
from pcode_precision_reference import PrecisionRoutes
from pcode_glm5_relations import shared_difference_check
from pcode_glm5_program_audit import audit_program


def verify_fixture(path,spec):
    raw=path.read_bytes(); assert raw[:8]==b'PCDTST02'
    assert struct.unpack_from('<III',raw,8)==(spec['user'],spec['token'],spec['actual_ep_mask'])
    offset=20; routed=0; count=0; global_ids=[]
    for ep in range(6):
        n,=struct.unpack_from('<I',raw,offset);offset+=4
        assert n==len(spec['expert_ids'][ep])
        if n:routed|=1<<ep
        for i in range(n):
            expert,weight=struct.unpack_from('<If',raw,offset);offset+=8
            assert expert==spec['expert_ids'][ep][i] and weight==spec['topk_weights'][ep][i]
            global_ids.append(OFFSETS[ep]+expert); count+=1
    assert count==len(set(global_ids))==8
    assert set(global_ids)==set(spec['global_experts'])
    assert routed==spec['routed_mask'] and spec['mask']==routed|1<<spec['shared_ep']
    codes,scales=input_arrays(spec)
    assert raw[offset:offset+6144]==codes.tobytes();offset+=6144
    assert raw[offset:offset+192]==scales.tobytes();offset+=192
    assert struct.unpack_from('<I',raw,offset)==(0,) and len(raw)==offset+4


def run(args):
    specs=specifications(); source=json.loads((args.out/'summary.json').read_text())
    assert source['total']==source['planned']==source['passed']==96 and not source['failed']
    reports={r['name']:r for r in source['cases']}
    assert len(reports)==96 and set(reports)=={s['name'] for s in specs}
    templates,logical=layout(args.bundle,args.catalog)
    program_audit=audit_program(args.bundle,args.catalog,templates)
    manifest=prepare_model(args.model,templates,logical)
    assert len(manifest['images'])==768 and manifest['total_data_bytes']==9890168832
    assert sum(i['count'] for i in manifest['images'])==33536
    # Audit full allocation sizes and all 128 logical shared shards in each EP.
    shared=Counter();shared_hashes={}
    for entry in manifest['images']:
        ep=entry['chip']//8
        assert entry['count']==OFFSETS[ep+1]-OFFSETS[ep]+1
        assert len(entry['expert_hashes'])==entry['count']
        key=entry['stage'],entry['chip']%8,entry['slot']
        shared[key]+=1
        prior=shared_hashes.setdefault(key,entry['expert_hashes'][0])
        assert prior==entry['expert_hashes'][0]
    assert len(shared)==128 and set(shared.values())=={6}
    identity=source['identity']
    bundle_hash=hashlib.sha256()
    for path in sorted([args.bundle/'pcode_ep6_tp8.bachir',*args.bundle.glob('*.hex')]):
        bundle_hash.update(path.name.encode()+b'\0'+path.read_bytes())
    assert bundle_hash.hexdigest()==identity['bundle_sha256']
    assert sha(args.simulator)==identity['simulator_sha256']
    # Preserve the exact executed source. Only cross-placement postprocessing
    # was corrected after the run; all 96 fixtures/references are regenerated
    # below using the current source and must match their execution hashes.
    executed=args.out/'executed_campaign.py'
    assert sha(executed if executed.exists() else Path(__file__).with_name('pcode_glm5_dense.py'))==identity['campaign_sha256']
    assert sha(Path(__file__).with_name('pcode_campaign.py'))==identity['reference_helpers_sha256']
    assert sha(Path(__file__).with_name('pcode_precision_reference.py'))==identity['precision_routes_sha256']
    assert sha(args.model/'manifest.json')==identity['model_manifest_sha256']
    model=DenseModel(args.model,manifest); routes=PrecisionRoutes(args.bundle,args.catalog,logical)
    verified=[]
    for index,spec in enumerate(specs):
        r=reports[spec['name']]
        assert r['passed'] and r['simulator_returncode']==0 and r['output_packets']==1
        assert r['native_mask_verified'] and r['all_model_images_preloaded']
        assert all(r[k]==v for k,v in identity.items())
        for key in ['global_experts','counts','expert_ids','topk_weights','routed_mask','shared_ep','actual_ep_mask','user','token']:
            assert r[key]==spec[key],(spec['name'],key)
        base=args.out/spec['name']
        verify_fixture(Path(str(base)+'.fixture'),spec)
        assert sha(Path(str(base)+'.fixture'))==r['fixture_sha256']
        actual=Path(str(base)+'.actual.bf16')
        assert sha(actual)==r['actual_sha256']
        expected,shared_sha=reference(spec,model,logical,routes)
        assert hashlib.sha256(expected.astype('<u2').tobytes()).hexdigest()==r['expected_sha256']
        assert shared_sha==r['shared_reference_sha256']
        path=Path(str(base)+'.reverified.bf16');path.write_bytes(expected.astype('<u2').tobytes())
        metrics=compare(actual,path);assert metrics['passed']
        verified.append(dict(r,**metrics,independently_reverified=True))
        print(f'reverified {index+1}/96 {spec["name"]}',flush=True)
    nonzero=[r for r in verified if not r['zero_reference']]
    relations=[]
    for group in ['order_0','order_1','order_2','shared_replicas']:
        members=sorted([r for r in verified if r.get('relation')==group],key=lambda r:r['name'])
        assert len(members)==(6 if group=='shared_replicas' else 2)
        for member in members[1:]:
            first=args.out/(members[0]['name']+'.actual.bf16'); second=args.out/(member['name']+'.actual.bf16')
            if group=='shared_replicas':
                assert member['shared_reference_sha256']==members[0]['shared_reference_sha256']
                metrics=shared_difference_check(first,second,args.out/(members[0]['name']+'.reverified.bf16'),
                                                args.out/(member['name']+'.reverified.bf16'))
            else: metrics=compare(second,first)
            assert metrics['passed'],group
            relations.append(dict(group=group,first=members[0]['name'],second=member['name'],**metrics))
    source.update(relations=relations,relations_passed=True,
                  postprocessing_sha256=sha(Path(__file__).with_name('pcode_glm5_relations.py')))
    (args.out/'summary.json').write_text(json.dumps(source,indent=2)+'\n')
    result=dict(status='passed',verified_at=datetime.now(ZoneInfo('Asia/Shanghai')).isoformat(),
                total=96,passed=96,failed=[],coverage=dict(Counter(r['family'] for r in verified)),
                configuration=dict(dim=6144,moe_inter_dim=2048,n_routed_experts=256,n_shared_experts=1,
                                   n_activated_experts=8,ep=6,tp=8,chips=48,
                                   complete_fixed_synthetic_model=True,all_experts_preloaded_each_case=True,
                                   internal_latch_moe_compiler_used=False,single_token_per_fresh_process=True),
                identity=identity,program_audit=program_audit,
                verification_sources=dict(campaign_sha256=sha(Path(__file__).with_name('pcode_glm5_dense.py')),
                       verifier_sha256=sha(__file__),relations_sha256=sha(Path(__file__).with_name('pcode_glm5_relations.py')),
                       program_audit_sha256=sha(Path(__file__).with_name('pcode_glm5_program_audit.py'))),
                postprocessing_correction=dict(execution_snapshot='build-ep6/glm5-dense-96/executed_campaign.py',
                     native_cases_rerun=False,all_96_references_regenerated=True,
                     reason='The extra raw comparison between shared placements ignored BF16 rounding location. Per-output thresholds remain unchanged; raw comparison metrics are preserved.'),
                model_audit=dict(images=768,routed_experts=256,shared_replicas=6,
                         shared_identical=True,expert_shards=33536,data_bytes=manifest['total_data_bytes']),
                statistics=dict(minimum_nonzero_cosine=min(r['cosine_similarity'] for r in nonzero),
                                maximum_relative_l2=max(r['relative_l2_error'] for r in verified),
                                bit_exact_cases=sum(r['bf16_mismatches']==0 for r in verified),
                                zero_reference_cases=sum(r['zero_reference'] for r in verified),
                                maximum_bf16_mismatches=max(r['bf16_mismatches'] for r in verified),
                                completed_cycle_range=[min(r['completed_cycles'] for r in verified),max(r['completed_cycles'] for r in verified)],
                                routed_masks=sorted({r['routed_mask'] for r in verified}),
                                actual_ep_masks=sorted({r['actual_ep_mask'] for r in verified})),
                relations=relations,cases=verified)
    args.report.write_text(json.dumps(result,indent=2)+'\n')
    lines=['GLM5 EP6+TP8完整权重单token：96/96通过',f'独立复核时间：{result["verified_at"]}',
           '每例：8个不同routed expert + 1个shared；全部专家权重预置；仅执行pcode导出程序。',
           f'最低非零余弦：{result["statistics"]["minimum_nonzero_cosine"]}',
           f'最大相对L2：{result["statistics"]["maximum_relative_l2"]}',
           '编号 | 用例 | routed mask | shared EP | actual EP mask | cosine | relative L2 | BF16差异 | 周期']
    for i,r in enumerate(verified,1):
        lines.append(f'{i:02d} | {r["name"]} | 0x{r["routed_mask"]:02x} | {r["shared_ep"]} | 0x{r["actual_ep_mask"]:02x} | {r["cosine_similarity"]} | {r["relative_l2_error"]:.8g} | {r["bf16_mismatches"]}/6144 | {r["completed_cycles"]}')
    args.report.with_suffix('.txt').write_text('\n'.join(lines)+'\n')
    print(json.dumps(result['statistics'],indent=2),flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('--bundle',type=Path,required=True);p.add_argument('--catalog',type=Path,required=True)
    p.add_argument('--model',type=Path,default=Path('build-ep6/glm5-dense-model'))
    p.add_argument('--out',type=Path,default=Path('build-ep6/glm5-dense-96'))
    p.add_argument('--simulator',type=Path,default=Path('build-ep6/pcode_bundle'))
    p.add_argument('--report',type=Path,default=Path('test/bach/pcode_glm5_dense_results.json'))
    run(p.parse_args())
