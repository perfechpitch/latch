import unittest
import tempfile
from pathlib import Path
from collections import Counter
import numpy as np
from pcode_glm5_two_token import pairs, native_evidence, alignment_only_failure, compatible_identity, sha
from pcode_glm5_dense import fixture, input_arrays, OFFSETS
from pcode_glm5_dense_verify import verify_fixture

class Glm5TwoTokenTests(unittest.TestCase):
    def test_complete_masks_shared_pairs_and_experts(self):
        cases=pairs()
        self.assertEqual(len(cases),96)
        self.assertEqual(Counter(p['family'] for p in cases),dict(mask_shared_cross=36,all_experts=16,boundary=18,numerical=16,contention=10))
        self.assertEqual({s['routed_mask'] for p in cases for s in p['tokens']},set(range(1,64)))
        self.assertEqual({tuple(s['shared_ep'] for s in p['tokens']) for p in cases[:36]}, {(a,b) for a in range(6) for b in range(6)})
        self.assertEqual({e for p in cases[36:52] for s in p['tokens'] for e in s['global_experts']},set(range(256)))
        for p in cases:
            self.assertEqual(p['mode'],'overlap')
            self.assertNotEqual(p['tokens'][0]['user'],p['tokens'][1]['user'])
            self.assertNotEqual(p['tokens'][0]['token'],p['tokens'][1]['token'])
            for s in p['tokens']:
                self.assertEqual(len(s['global_experts']),8)
                self.assertEqual(len(set(s['global_experts'])),8)
                self.assertEqual(s['user']%6,s['shared_ep'])
                self.assertEqual(s['mask'],s['routed_mask']|1<<s['shared_ep'])
                self.assertAlmostEqual(sum(map(sum,s['topk_weights'])),1,places=6)
                for ep,ids in enumerate(s['expert_ids']):
                    self.assertTrue(all(0<=i<OFFSETS[ep+1]-OFFSETS[ep] for i in ids))
    def test_all_fixtures_decode_and_mask_is_not_cleared_for_zero(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            for p in pairs():
                for s in p['tokens']:
                    fixture(s,root);verify_fixture(root/(s['name']+'.fixture'),s)
                    self.assertGreater(s['mask'],0)
    def test_identical_payload_and_expert_overlap(self):
        cases={p['name']:p for p in pairs()}
        a,b=cases['contention_identical_payload']['tokens']
        self.assertEqual(a['global_experts'],b['global_experts'])
        self.assertEqual(a['topk_weights'],b['topk_weights'])
        for x,y in zip(input_arrays(a),input_arrays(b)):np.testing.assert_array_equal(x,y)
        self.assertEqual([a['user'],b['user']],[3,1023])
        a,b=cases['contention_partial_expert_overlap']['tokens']
        self.assertEqual(len(set(a['global_experts'])&set(b['global_experts'])),4)
    def test_requires_compute_overlap_and_two_independent_masks(self):
        p=pairs()[0];users=sorted(s['user'] for s in p['tokens'])
        masks=''.join(f'pcode mask routed {s["routed_mask"]}, shared EP {s["shared_ep"]}, actual {s["mask"]}\n' for s in p['tokens'])
        text=masks+f'pcode dense model images 768, data bytes 99\npcode sent token 0 at 2\npcode sent token 1 at 420\npcode concurrent compute users at 500 {users[0]} {users[1]}\npcode received token 0 at 8000\npcode received token 1 at 8100\npcode stopped at 8132, output packets 2\n[  PASSED  ] 1 test.\n'
        good=native_evidence(text,p,99)
        self.assertTrue(all(good[k] for k in ['overlapped','simultaneous_compute_users_verified','native_masks_verified','all_model_images_preloaded','native_state_checks_passed']))
        no_compute=text.replace(f'pcode concurrent compute users at 500 {users[0]} {users[1]}','')
        self.assertFalse(native_evidence(no_compute,p,99)['simultaneous_compute_users_verified'])
        self.assertFalse(native_evidence(text.replace('token 1 at 420','token 1 at 9000'),p,99)['overlapped'])
        self.assertFalse(native_evidence(text.replace(masks.splitlines()[1],'wrong mask'),p,99)['native_masks_verified'])
        self.assertFalse(native_evidence(text.replace('data bytes 99','data bytes 98'),p,99)['all_model_images_preloaded'])
        self.assertFalse(native_evidence(text+'[  FAILED  ] check',p,99)['native_state_checks_passed'])
    def test_timing_retry_cannot_hide_functional_failures(self):
        report=dict(simulator_returncode=1,output_packets=2,overlapped=True,
            simultaneous_compute_users_verified=False,native_masks_verified=True,
            all_model_images_preloaded=True,tokens=[dict(passed=True),dict(passed=True)])
        text='harness.cpp:287: Failure\nExpected: (h.compute_overlap_cycle) > (0u), actual: 0 vs 0\n'
        self.assertTrue(alignment_only_failure(report,text))
        self.assertFalse(alignment_only_failure(report,text+'other.cpp:123: Failure\n'))
        self.assertFalse(alignment_only_failure(dict(report,simulator_returncode=124),text))
        self.assertFalse(alignment_only_failure(dict(report,native_masks_verified=False),text))
        self.assertFalse(alignment_only_failure(dict(report,tokens=[dict(passed=False),dict(passed=True)]),text))

    def test_reverse_injection_keeps_logical_identity(self):
        p=pairs()[0];a,b=p['tokens']
        text=''.join(f'pcode mask routed {s["routed_mask"]}, shared EP {s["shared_ep"]}, actual {s["mask"]}\n' for s in [b,a])
        text+=f'pcode dense model images 768, data bytes 99\npcode sent token 0 at 2\npcode sent token 1 at 2048\npcode concurrent compute users at 2500 {a["user"]} {b["user"]}\npcode received token 0 at 8000\npcode received token 1 at 8100\npcode stopped at 8132, output packets 2\n[  PASSED  ] 1 test.\n'
        evidence=native_evidence(text,p,99,(1,0))
        self.assertTrue(evidence['native_masks_verified'])
        self.assertTrue(evidence['overlapped'])
        self.assertEqual(evidence['sent_at'],{1:2,0:2048})
        self.assertEqual(evidence['received_at'],{1:8000,0:8100})
        self.assertFalse(native_evidence(text,p,99)['native_masks_verified'])

    def test_resume_cannot_reuse_changed_model_or_binary(self):
        with tempfile.TemporaryDirectory() as temp:
            out=Path(temp);archive=out/'runner_sources';archive.mkdir()
            path=archive/'old.py';path.write_text('immutable old driver')
            old_sha=sha(path);path.rename(archive/(old_sha+'.py'))
            old=dict(runner_sha256=old_sha,simulator_sha256='same',model_manifest_sha256='model')
            current=dict(old,runner_sha256='new')
            self.assertTrue(compatible_identity(old,current,out))
            self.assertFalse(compatible_identity(old,dict(current,simulator_sha256='different'),out))
            self.assertFalse(compatible_identity(old,dict(current,model_manifest_sha256='different'),out))
            (archive/(old_sha+'.py')).write_text('tampered')
            self.assertFalse(compatible_identity(old,current,out))

if __name__=='__main__':unittest.main()
