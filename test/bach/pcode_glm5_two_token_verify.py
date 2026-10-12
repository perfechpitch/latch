"""Audit execution hashes and freshly rebuild all 192 dense references."""
from collections import Counter
from datetime import datetime
import hashlib
import json
from pathlib import Path
import tempfile
import platform
import subprocess
import sys
from zoneinfo import ZoneInfo
from pcode_glm5_two_token import pairs, parser, identity, native_evidence, alignment_only_failure, compatible_identity
from pcode_glm5_dense import (layout, prepare_model, DenseModel, reference, fixture, compare, sha, OFFSETS)
from pcode_glm5_dense_verify import verify_fixture
from pcode_precision_reference import PrecisionRoutes
from pcode_glm5_program_audit import audit_program


def run(args):
    planned=pairs();source=json.loads((args.out/'summary.json').read_text())
    assert source['total']==source['planned']==source['passed']==len(planned)==96 and not source['failed']
    reports={r['name']:r for r in source['cases']}
    assert set(reports)=={p['name'] for p in planned}
    expected_identity=identity(args); assert source['identity']==expected_identity
    templates,logical=layout(args.bundle,args.catalog)
    program=audit_program(args.bundle,args.catalog,templates)
    manifest=prepare_model(args.model,templates,logical)
    assert len(manifest['images'])==768 and manifest['total_data_bytes']==9890168832
    shared_hashes={};shard_count=0
    for entry in manifest['images']:
        ep=entry['chip']//8
        assert entry['count']==OFFSETS[ep+1]-OFFSETS[ep]+1
        shard_count+=entry['count']
        key=entry['stage'],entry['chip']%8,entry['slot']
        hashes=shared_hashes.setdefault(key,[]);hashes.append(entry['expert_hashes'][0])
    assert shard_count==33536 and len(shared_hashes)==128
    assert all(len(v)==6 and len(set(v))==1 for v in shared_hashes.values())
    model=DenseModel(args.model,manifest);routes=PrecisionRoutes(args.bundle,args.catalog,logical)
    verified=[];tokens=[]
    for i,pair in enumerate(planned):
        r=reports[pair['name']]
        assert compatible_identity(r['identity'],expected_identity,args.out) and r['passed'] and r['simulator_returncode']==0 and r['output_packets']==2
        order=r.get('injection_order',[0,1]);assert sorted(order)==[0,1]
        assert r['mode']==pair['mode']=='overlap' and r['requested_gap']==pair['gap'] and r['gap'] in {pair['gap'],2048,4096}
        log=args.out/(pair['name']+'.log');assert sha(log)==r['log_sha256']
        for trial in r['timing_alignment_attempts']:
            trial_log=args.out/trial['log_file'];assert sha(trial_log)==trial['log_sha256']
            assert compatible_identity(trial['identity'],expected_identity,args.out)
            assert alignment_only_failure(trial,trial_log.read_text())
            trial_evidence=native_evidence(trial_log.read_text(),pair,manifest['total_data_bytes'],trial.get('injection_order',[0,1]))
            for key,value in trial_evidence.items():
                recorded=trial[key]
                if isinstance(recorded,dict):recorded={int(k):v for k,v in recorded.items()}
                assert recorded==value,(pair['name'],'alignment attempt',key)
            for j,path in enumerate(trial['actual_files']):
                assert sha(args.out/path)==trial['tokens'][j]['actual_sha256']
        evidence=native_evidence(log.read_text(),pair,manifest['total_data_bytes'],order)
        for key,value in evidence.items():
            recorded=r[key]
            if isinstance(recorded,dict):recorded={int(k):v for k,v in recorded.items()}
            assert recorded==value,(pair['name'],key)
        assert all(evidence[k] for k in ['overlapped','simultaneous_compute_users_verified','native_masks_verified','all_model_images_preloaded','native_state_checks_passed'])
        fresh_tokens=[]
        with tempfile.TemporaryDirectory(prefix='glm5-two-token-verify-') as tmp:
            for j,spec in enumerate(pair['tokens']):
                old=r['tokens'][j];fresh=fixture(spec,Path(tmp))
                assert all(old[k]==v for k,v in fresh.items())
                path=args.out/(spec['name']+'.fixture');verify_fixture(path,spec)
                assert sha(path)==fresh['fixture_sha256']
                expected,shared_sha=reference(spec,model,logical,routes)
                raw=expected.astype('<u2').tobytes()
                assert hashlib.sha256(raw).hexdigest()==old['expected_sha256']
                assert shared_sha==old['shared_reference_sha256']
                assert raw==(args.out/(spec['name']+'.expected.bf16')).read_bytes()
                expected_path=args.out/(spec['name']+'.reverified.bf16');expected_path.write_bytes(raw)
                actual=args.out/(pair['name']+f'.actual.token{j}.bf16')
                metrics=compare(actual,expected_path)
                assert metrics['passed'] and all(old[k]==v for k,v in metrics.items())
                for trial in r['timing_alignment_attempts']:
                    prior_metrics=compare(args.out/trial['actual_files'][j],expected_path)
                    assert prior_metrics['passed']
                    assert all(trial['tokens'][j][k]==v for k,v in prior_metrics.items())
                fresh_tokens.append(dict(old,independently_reverified=True))
        verified.append(dict(r,**evidence,tokens=fresh_tokens,independently_reverified=True));tokens.extend(fresh_tokens)
        print(f'reverified {i+1}/96 {pair["name"]}',flush=True)
    nonzero=[s for s in tokens if not s['zero_reference']]
    # All-expert pairs reuse the 32 single-token inputs/TopK/shared placements.
    # Compare saved independent single-token outputs to concurrent execution,
    # in addition to rebuilding the arithmetic oracle for every token above.
    single_report=Path(__file__).with_name('pcode_glm5_dense_results.json')
    single=json.loads(single_report.read_text())
    assert single['status']=='passed' and single['passed']==96
    assert single['identity']['model_manifest_sha256']==expected_identity['model_manifest_sha256']
    baseline_program=dict(bundle_unchanged=single['identity']['bundle_sha256']==expected_identity['bundle_sha256'])
    if not baseline_program['bundle_unchanged']:
        old_bundle=args.single_baseline_bundle
        digest=hashlib.sha256()
        for path in sorted([old_bundle/'pcode_ep6_tp8.bachir',*old_bundle.glob('*.hex')]):
            digest.update(path.name.encode()+b'\0'+path.read_bytes())
        assert digest.hexdigest()==single['identity']['bundle_sha256']
        def semantic_ir(path):
            result=[]
            for line in path.read_text().splitlines():
                fields=line.split()
                if not fields or fields[0].startswith('#'):continue
                if fields[0]=='RTAB':fields[6:12]=['VC']*6
                if fields[0]=='TSRTAB':fields[5]='VC'
                result.append(fields)
            return result
        assert semantic_ir(old_bundle/'pcode_ep6_tp8.bachir')==semantic_ir(args.bundle/'pcode_ep6_tp8.bachir')
        old_hex={p.name:sha(p) for p in old_bundle.glob('*.hex')}
        new_hex={p.name:sha(p) for p in args.bundle.glob('*.hex')}
        used_hex={row[-1] for row in semantic_ir(args.bundle/'pcode_ep6_tp8.bachir') if row[0]=='KERNELCORE'}
        assert len(used_hex)==855 and used_hex <= old_hex.keys() and old_hex==new_hex
        baseline_program.update(only_rtab_and_tsrtab_vcs_changed=True,rv_hex_images_identical=855,
            cached_hex_files_identical=len(old_hex),
            original_bundle_sha256=single['identity']['bundle_sha256'])
    single_cases={s['name']:s for s in single['cases']};baselines=[]
    for pair in verified:
        if pair['family']!='all_experts':continue
        number=int(pair['name'].split('_')[-1])
        for j,token in enumerate(pair['tokens']):
            block=2*number+j;name=f'experts_{8*block:03d}_{8*block+7:03d}'
            old=single_cases[name]
            for key in ['global_experts','expert_ids','topk_weights','shared_ep','profile','input_seed','actual_ep_mask']:
                assert old[key]==token[key]
            assert old['expected_sha256']==token['expected_sha256']
            original=args.single_baseline/(name+'.actual.bf16')
            assert sha(original)==old['actual_sha256']
            metrics=compare(args.out/(pair['name']+f'.actual.token{j}.bf16'),original)
            assert metrics['passed']
            baselines.append(dict(pair=pair['name'],token_index=j,single_case=name,**metrics))
    assert len(baselines)==32
    identical=next(r for r in verified if r['name']=='contention_identical_payload')
    assert identical['tokens'][0]['expected_sha256']==identical['tokens'][1]['expected_sha256']
    identical_metrics=compare(args.out/(identical['name']+'.actual.token0.bf16'),
                              args.out/(identical['name']+'.actual.token1.bf16'))
    assert identical_metrics['passed']
    def git(root,*parameters):
        return subprocess.check_output(['git','-C',str(root),*parameters],text=True).strip()
    bach=args.bundle.resolve().parents[3];latch=Path.cwd()
    assert git(bach,'branch','--show-current')==git(latch,'branch','--show-current')=='pcode_dev_zlm'
    regressions={}
    for name,count in dict(memory=25,router_reduce=21,router_scenarios=7,ts_credit=7,bundle_load=13,
                          router_basic=8,router_arbiter=21,router_assembly=14,dte_inbound=7,dte_lane=15).items():
        path=args.out.parent/f'glm5-two-token-{name}-regression.log';text=path.read_text()
        assert f'[  PASSED  ] {count} tests.' in text and '[  FAILED  ]' not in text
        regressions[name]=dict(passed=count,log_sha256=sha(path),binary_sha256=sha(Path('build-ep6')/name))
    legacy_path=args.out.parent/'glm5-two-token-legacy-compat'/'summary.json'
    legacy=json.loads(legacy_path.read_text())
    assert legacy['total']==legacy['planned']==legacy['passed']==1 and not legacy['failed']
    assert legacy['cases'][0]['simulator_sha256']==expected_identity['simulator_sha256']
    assert legacy['cases'][0]['bundle_sha256']==expected_identity['bundle_sha256']
    compatibility=dict(sparse_two_token=dict(summary_sha256=sha(legacy_path),cases=legacy['cases']))
    single_compat_path=args.out.parent/'glm5-two-token-single-compat'/'summary.json'
    single_compat=json.loads(single_compat_path.read_text())
    assert single_compat['total']==single_compat['planned']==single_compat['passed']==1 and not single_compat['failed']
    for key in ['simulator_sha256','bundle_sha256','model_manifest_sha256']:
        assert single_compat['identity'][key]==expected_identity[key]
    compatibility['dense_single_token']=dict(summary_sha256=sha(single_compat_path),cases=single_compat['cases'])
    python_regression_path=args.out.parent/'glm5-two-token-python-regression.log'
    python_regression_text=python_regression_path.read_text()
    assert 'Ran 24 tests' in python_regression_text and python_regression_text.rstrip().endswith('OK')
    pcode_regression_path=args.out.parent/'glm5-two-token-pcode-all-regression.log'
    assert '72 passed, 14 skipped' in pcode_regression_path.read_text()
    failure_paths=dict(report=args.out.parent/'glm5-two-token-before-header-fix'/'cases'/'expert_blocks_09.report.json',
        unit_log=args.out.parent/'glm5-router-remap-before.log',
        diagnostic_log=args.out.parent/'glm5-two-token-header-diag'/'expert_blocks_09.log')
    failure_info=dict(available=all(p.exists() for p in failure_paths.values()),case='expert_blocks_09',
        reason='Different incoming VCs were interleaved after mapping to the same output VC. RouterStation now retains ownership per output direction and next-hop VC until the packet tail.')
    if failure_info['available']:
        old_failure=json.loads(failure_paths['report'].read_text())
        assert not old_failure['passed'] and old_failure['simulator_returncode']==-6
        assert '[  FAILED  ] BachRouter.VcRemappingKeepsEachOutgoingPacketContiguous' in failure_paths['unit_log'].read_text()
        diagnostic=failure_paths['diagnostic_log'].read_text()
        assert 'bytes 15616 expected 12288' in diagnostic and 'bytes 8960 expected 12288' in diagnostic
        failure_info.update({key+'_sha256':sha(path) for key,path in failure_paths.items()})
    vc_failure_dir=args.out.parent/'glm5-two-token-boundary15-reduce-owner-fixed'
    vc_failure_report=vc_failure_dir/'boundary_pair_15.report.json'
    vc_failure_info=dict(available=vc_failure_report.exists(),case='boundary_pair_15',
        reason='An ordinary p2p transfer was mapped onto VC3 reserved for streaming FC13 reduction, causing a packet ownership dependency. The exporter maps ordinary VC3 traffic to VC0 and updates TSRTAB consistently.')
    if vc_failure_info['available']:
        old=json.loads(vc_failure_report.read_text());old_log=vc_failure_dir/'boundary_pair_15.log'
        assert not old['passed'] and old['simulator_returncode']==1 and old['output_packets']==0
        assert old['completed_cycles']==old['maximum_cycles']==16000
        assert old['identity']['simulator_sha256']==expected_identity['simulator_sha256']
        assert old['identity']['bundle_sha256']==single['identity']['bundle_sha256']
        text=old_log.read_text()
        assert 'input 1 vc 3 user 774 path 33 head 1' in text
        assert 'core 2 output 2 vc 3 input+1 5 private credit 20 shared credit 20' in text
        vc_failure_info.update(report_sha256=sha(vc_failure_report),log_sha256=sha(old_log),
            before_outputs=0,after_outputs=2,before_completed_cycles=16000,
            after_completed_cycles=reports['boundary_pair_15']['completed_cycles'])
    result=dict(status='passed',verified_at=datetime.now(ZoneInfo('Asia/Shanghai')).isoformat(),
        total=96,passed=96,failed=[],token_outputs=len(tokens),identity=expected_identity,
        verifier_sha256=sha(__file__),program_audit=program,
        single_token_baseline_audit=dict(report_sha256=sha(single_report),outputs_recompared=32,
            all_passed=True,program_equivalence=baseline_program,cases=baselines),identical_payload_comparison=identical_metrics,
        versions=dict(branch='pcode_dev_zlm',bachflow_source_commit=git(bach,'rev-parse','HEAD'),
            latch_base_commit=git(latch,'rev-parse','HEAD'),python=sys.version,machine=platform.machine()),
        existing_native_regressions=regressions,compatibility_regressions=compatibility,
        python_regressions=dict(passed=24,log_sha256=sha(python_regression_path)),
        pcode_regressions=dict(passed=72,skipped=14,log_sha256=sha(pcode_regression_path)),
        changed_source_sha256={str(p.relative_to(root)):sha(p) for root,p in [
            (bach,bach/'pcode/src/bachflow_pcode/latch_export.py'),
            (bach,bach/'pcode/tests/test_latch_export_intents.py'),
            (latch,latch/'src/bach/ip/chip/core/router/router_station.h'),
            (latch,latch/'src/bach/ip/chip/core/router/xbar.h'),
            (latch,latch/'src/bach/ip/chip/core/dte/header_parser.h'),
            (latch,latch/'test/bach/ip/chip/core/router/router_basic.cpp'),
            (latch,latch/'test/bach/ip/chip/core/router/router_arbiter.cpp')]},
        regression_before_fix=failure_info,
        vc_mapping_regression_before_fix=vc_failure_info,
        configuration=dict(ep=6,tp=8,chips=48,dim=6144,moe_inter_dim=2048,n_routed_experts=256,
            n_shared_experts=1,n_activated_experts=8,tokens_per_fresh_process=2,
            shared_execution='user ID % 6; coefficient 1',
            full_fixed_model_preloaded_once_per_pair=True,internal_latch_moe_compiler_used=False,
            cosine_threshold=0.99999,relative_l2_threshold=0.02),
        model_audit=dict(images=768,data_bytes=manifest['total_data_bytes'],expert_shards=shard_count,
            routed_experts=256,shared_replicas=6,shared_identical=True),
        coverage=dict(families=dict(Counter(p['family'] for p in planned)),concurrent_pairs=96,
            all_routed_experts=sorted({e for s in tokens for e in s['global_experts']}),
            routed_masks=sorted({s['routed_mask'] for s in tokens}),actual_ep_masks=sorted({s['actual_ep_mask'] for s in tokens}),
            shared_ep_pairs=sorted({tuple(s['shared_ep'] for s in p['tokens']) for p in planned}),
            routed_mask_pairs=sorted({tuple(s['routed_mask'] for s in p['tokens']) for p in planned}),
            same_shared_ep_pairs=sum(p['tokens'][0]['shared_ep']==p['tokens'][1]['shared_ep'] for p in planned),
            expert_overlap_pairs=sum(bool(set(p['tokens'][0]['global_experts'])&set(p['tokens'][1]['global_experts'])) for p in planned)),
        statistics=dict(minimum_nonzero_cosine=min(s['cosine_similarity'] for s in nonzero),
            maximum_relative_l2=max(s['relative_l2_error'] for s in tokens),
            bit_exact_tokens=sum(s['bf16_mismatches']==0 for s in tokens),
            zero_reference_tokens=sum(s['zero_reference'] for s in tokens),
            maximum_bf16_mismatches=max(s['bf16_mismatches'] for s in tokens),
            completed_cycle_range=[min(r['completed_cycles'] for r in verified),max(r['completed_cycles'] for r in verified)],
            compute_overlap_cycle_range=[min(r['compute_overlap_cycle'] for r in verified),max(r['compute_overlap_cycle'] for r in verified)],
            timing_alignment_retry_pairs=sum(bool(r['timing_alignment_attempts']) for r in verified),
            reversed_injection_pairs=sum(r.get('injection_order',[0,1])==[1,0] for r in verified),
            executed_runner_sha256=sorted({r['identity']['runner_sha256'] for r in verified}),
            injection_gaps=dict(Counter(r['gap'] for r in verified))),
        cases=verified)
    assert len(tokens)==192 and result['coverage']['routed_masks']==list(range(1,64))
    assert len(result['coverage']['shared_ep_pairs'])==36
    args.report.write_text(json.dumps(result,indent=2)+'\n')
    lines=['GLM5 EP6+TP8完整权重双token：96组/192个输出全部通过',f'独立复核时间：{result["verified_at"]}',
        '同实例并发；每token严格8个routed + 1个shared；全模型只预置一次；全部执行pcode导出程序。',
        f'最低非零余弦：{result["statistics"]["minimum_nonzero_cosine"]}',
        f'最大相对L2：{result["statistics"]["maximum_relative_l2"]}',
        '全零参考的 cosine=None 表示余弦未定义；这类输出以严格数值全零验收，仍检查身份、并发和结束状态。',
        '组号 | 用例 | token | user | global experts | input profile/seed | routed mask | shared EP | actual EP mask | cosine | relative L2 | BF16差异 | 发送周期 | 输出周期 | 并发计算证据周期']
    for i,r in enumerate(verified,1):
        for j,s in enumerate(r['tokens']):
            lines.append(f'{i:02d} | {r["name"]} | {j} | {s["user"]} | {s["global_experts"]} | {s["profile"]}/{s["input_seed"]} | 0x{s["routed_mask"]:02x} | {s["shared_ep"]} | 0x{s["actual_ep_mask"]:02x} | {s["cosine_similarity"]} | {s["relative_l2_error"]:.8g} | {s["bf16_mismatches"]}/6144 | {r["sent_at"][j]} | {r["received_at"][j]} | {r["compute_overlap_cycle"]}')
    args.report.with_suffix('.txt').write_text('\n'.join(lines)+'\n')
    print(json.dumps(result['statistics'],indent=2),flush=True)

if __name__=='__main__':
    p=parser();p.add_argument('--report',type=Path,default=Path('test/bach/pcode_glm5_two_token_results.json'))
    p.add_argument('--single-baseline',type=Path,default=Path('build-ep6/glm5-dense-96'))
    p.add_argument('--single-baseline-bundle',type=Path,default=Path('build-ep6/glm5-two-token-before-transfer-vc-fix/exported_bundle'))
    run(p.parse_args())
