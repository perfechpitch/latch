"""Audit fresh 96-single/96-pair campaigns without historical-run dependencies.

Every numerical reference is rebuilt from the immutable model and pcode routing
configuration. Simulator outputs are never used to construct a reference.
"""
import argparse
from collections import Counter
from datetime import datetime
import hashlib
import json
from pathlib import Path
import tempfile
from zoneinfo import ZoneInfo

from pcode_glm5_dense import (specifications, layout, prepare_model, DenseModel,
                               reference, fixture, compare, sha)
from pcode_glm5_dense_verify import verify_fixture
from pcode_glm5_program_audit import audit_program
from pcode_glm5_relations import shared_difference_check
from pcode_glm5_two_token import pairs, identity, native_evidence, alignment_only_failure
from pcode_precision_reference import PrecisionRoutes


def read_complete(out, planned):
    summary=json.loads((out/'summary.json').read_text())
    assert summary['total']==summary['planned']==summary['passed']==96
    assert not summary['failed']
    cases={r['name']:r for r in summary['cases']}
    assert len(cases)==96 and set(cases)=={s['name'] for s in planned}
    return summary,cases


def verify(args):
    singles=specifications(); paired=pairs()
    single_summary,single_cases=read_complete(args.single_out,singles)
    pair_summary,pair_cases=read_complete(args.two_out,paired)
    templates,logical=layout(args.bundle,args.catalog)
    program=audit_program(args.bundle,args.catalog,templates)
    model_manifest=prepare_model(args.model,templates,logical)
    assert len(model_manifest['images'])==768
    assert sum(e['count'] for e in model_manifest['images'])==33536
    model=DenseModel(args.model,model_manifest)
    routes=PrecisionRoutes(args.bundle,args.catalog,logical)
    digest=hashlib.sha256()
    for path in sorted([args.bundle/'pcode_ep6_tp8.bachir',*args.bundle.glob('*.hex')]):
        digest.update(path.name.encode()+b'\0'+path.read_bytes())
    single_identity=single_summary['identity']
    for key,value in dict(bundle_sha256=digest.hexdigest(),simulator_sha256=sha(args.simulator),
                          campaign_sha256=sha(Path(__file__).with_name('pcode_glm5_dense.py')),
                          reference_helpers_sha256=sha(Path(__file__).with_name('pcode_campaign.py')),
                          precision_routes_sha256=sha(Path(__file__).with_name('pcode_precision_reference.py')),
                          model_manifest_sha256=sha(args.model/'manifest.json')).items():
        assert single_identity[key]==value,key
    args.out=args.two_out
    pair_identity=identity(args)
    assert pair_summary['identity']==pair_identity
    verified_single=[];verified_pairs=[]

    def numeric(spec,record,out,actual):
        verify_fixture(out/(spec['name']+'.fixture'),spec)
        with tempfile.TemporaryDirectory(prefix='merged-glm5-reference-') as tmp:
            generated=fixture(spec,Path(tmp))
            assert all(record[k]==v for k,v in generated.items())
            expected,shared_hash=reference(spec,model,logical,routes)
            expected_path=Path(tmp)/'expected.bf16'
            expected_path.write_bytes(expected.astype('<u2').tobytes())
            assert sha(expected_path)==record['expected_sha256']
            assert shared_hash==record['shared_reference_sha256']
            assert expected_path.read_bytes()==(out/(spec['name']+'.expected.bf16')).read_bytes()
            metrics=compare(actual,expected_path)
            assert metrics['passed'],(spec['name'],metrics)
            assert all(record[k]==v for k,v in metrics.items())
        return dict(record,independently_reverified=True)

    for i,spec in enumerate(singles,1):
        r=single_cases[spec['name']]
        assert r['passed'] and r['simulator_returncode']==0 and r['output_packets']==1
        assert r['native_mask_verified'] and r['all_model_images_preloaded']
        assert all(r[k]==v for k,v in single_identity.items())
        text=(args.single_out/(spec['name']+'.log')).read_text()
        assert '[  PASSED  ] 1 test.' in text and '[  FAILED  ]' not in text
        actual=args.single_out/(spec['name']+'.actual.bf16')
        verified_single.append(numeric(spec,r,args.single_out,actual))
        print(f'reverified single {i}/96 {spec["name"]}',flush=True)
    relations=[]
    for group in ['order_0','order_1','order_2','shared_replicas']:
        members=sorted([r for r in verified_single if r.get('relation')==group],key=lambda r:r['name'])
        assert len(members)==(6 if group=='shared_replicas' else 2)
        first=members[0]
        for second in members[1:]:
            a=args.single_out/(first['name']+'.actual.bf16')
            b=args.single_out/(second['name']+'.actual.bf16')
            if group=='shared_replicas':
                assert first['shared_reference_sha256']==second['shared_reference_sha256']
                metrics=shared_difference_check(a,b,args.single_out/(first['name']+'.expected.bf16'),
                                                args.single_out/(second['name']+'.expected.bf16'))
            else:metrics=compare(b,a)
            assert metrics['passed'],(group,metrics)
            relations.append(dict(group=group,first=first['name'],second=second['name'],**metrics))
    for i,pair in enumerate(paired,1):
        r=pair_cases[pair['name']]
        assert r['passed'] and r['simulator_returncode']==0 and r['output_packets']==2
        assert r['identity']==pair_identity
        log=args.two_out/(pair['name']+'.log')
        assert sha(log)==r['log_sha256']
        text=log.read_text()
        assert '[  PASSED  ] 1 test.' in text and '[  FAILED  ]' not in text
        evidence=native_evidence(text,pair,model_manifest['total_data_bytes'],r.get('injection_order',[0,1]))
        for key,value in evidence.items():
            recorded=r[key]
            if isinstance(recorded,dict):recorded={int(k):v for k,v in recorded.items()}
            assert recorded==value,(pair['name'],key)
        assert all(evidence[k] for k in ['overlapped','simultaneous_compute_users_verified',
                     'native_masks_verified','all_model_images_preloaded','native_state_checks_passed'])
        tokens=[]
        for j,spec in enumerate(pair['tokens']):
            actual=args.two_out/(pair['name']+f'.actual.token{j}.bf16')
            tokens.append(numeric(spec,r['tokens'][j],args.two_out,actual))
        for trial in r['timing_alignment_attempts']:
            assert trial['identity']==pair_identity
            trial_log=args.two_out/trial['log_file']
            assert sha(trial_log)==trial['log_sha256']
            assert alignment_only_failure(trial,trial_log.read_text())
            for j,path in enumerate(trial['actual_files']):
                numeric(pair['tokens'][j],trial['tokens'][j],args.two_out,args.two_out/path)
        if pair['family']=='all_experts':
            for j,spec in enumerate(pair['tokens']):
                baseline=next(s for s in singles if all(s[k]==spec[k] for k in
                    ['global_experts','topk_weights','shared_ep','profile','input_seed']))
                metrics=compare(args.two_out/(pair['name']+f'.actual.token{j}.bf16'),
                                args.single_out/(baseline['name']+'.actual.bf16'))
                assert metrics['passed'],(pair['name'],'single/concurrent',j)
        verified_pairs.append(dict(r,tokens=tokens,independently_reverified=True))
        print(f'reverified pair {i}/96 {pair["name"]}',flush=True)
    all_tokens=verified_single+[t for r in verified_pairs for t in r['tokens']]
    nonzero=[r for r in all_tokens if not r['zero_reference']]
    result=dict(status='passed',verified_at=datetime.now(ZoneInfo('Asia/Shanghai')).isoformat(),
        single_cases=96,pairs=96,token_outputs=288,failed=[],
        identity=dict(single=single_identity,two_token=pair_identity,verifier_sha256=sha(__file__)),
        program_audit=program,model_manifest_sha256=sha(args.model/'manifest.json'),
        acceptance=dict(min_cosine=0.99999,max_relative_l2=0.02,zero_reference_requires_exact_zero=True),
        statistics=dict(minimum_nonzero_cosine=min(t['cosine_similarity'] for t in nonzero),
                        maximum_relative_l2=max(t['relative_l2_error'] for t in all_tokens),
                        zero_reference_outputs=sum(t['zero_reference'] for t in all_tokens)),
        coverage=dict(single_families=dict(Counter(t['family'] for t in singles)),
                      pair_families=dict(Counter(t['family'] for t in paired))),
        relations=relations,single=verified_single,two_token=verified_pairs)
    args.report.parent.mkdir(parents=True,exist_ok=True)
    args.report.write_text(json.dumps(result,indent=2)+'\n')
    lines=['GLM5 EP6+TP8 合并回归：96 单 token、96 双 token 组、288 个输出全部通过。',
           f'重新生成独立参考的复核时间：{result["verified_at"]}',
           f'最低非零余弦：{result["statistics"]["minimum_nonzero_cosine"]}',
           f'最大相对 L2：{result["statistics"]["maximum_relative_l2"]}',
           '类别 | 用例 | token | routed mask | shared EP | actual EP mask | cosine | relative L2']
    for t in verified_single:
        lines.append(f'single | {t["name"]} | 0 | 0x{t["routed_mask"]:02x} | {t["shared_ep"]} | 0x{t["actual_ep_mask"]:02x} | {t["cosine_similarity"]} | {t["relative_l2_error"]}')
    for r in verified_pairs:
        for j,t in enumerate(r['tokens']):
            lines.append(f'pair | {r["name"]} | {j} | 0x{t["routed_mask"]:02x} | {t["shared_ep"]} | 0x{t["actual_ep_mask"]:02x} | {t["cosine_similarity"]} | {t["relative_l2_error"]}')
    args.report.with_suffix('.txt').write_text('\n'.join(lines)+'\n')
    print(json.dumps(result['statistics'],indent=2),flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('--bundle',type=Path,required=True)
    p.add_argument('--catalog',type=Path,required=True)
    p.add_argument('--single-out',type=Path,required=True)
    p.add_argument('--two-out',type=Path,required=True)
    p.add_argument('--report',type=Path,required=True)
    p.add_argument('--model',type=Path,default=Path('build-ep6/glm5-dense-model'))
    p.add_argument('--simulator',type=Path,default=Path('build-ep6/pcode_bundle'))
    verify(p.parse_args())
