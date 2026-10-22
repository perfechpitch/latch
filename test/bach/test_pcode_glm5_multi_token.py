import tempfile
import unittest
from pathlib import Path

from pcode_glm5_multi_token import groups, evidence, summarize
from pcode_glm5_dense import fixture, input_arrays, OFFSETS
from pcode_glm5_dense_verify import verify_fixture
import numpy as np


class Glm5MultiTokenTests(unittest.TestCase):
    def test_missing_output_is_not_counted_as_an_output(self):
        from types import SimpleNamespace
        tokens=[dict(s,passed=False,error='missing') for s in groups()[0]['tokens']]
        tokens[0].update(passed=True,actual_sha256='present')
        report=dict(name='partial',passed=False,output_packets=1,completed_cycles=None,
                    peak_compute_users=16,peak_core_users=8,tokens=tokens)
        with tempfile.TemporaryDirectory() as tmp:
            result=summarize(SimpleNamespace(out=Path(tmp)),[report],1,{})
            self.assertEqual(result['token_reports'],16)
            self.assertEqual(result['token_outputs'],1)
            self.assertEqual(result['token_outputs_passed'],1)

    def test_plan_covers_experts_masks_and_legal_identities(self):
        plan = groups()
        self.assertEqual(len(plan), 10)
        self.assertEqual({e for g in plan[:2] for t in g['tokens'] for e in t['global_experts']}, set(range(256)))
        self.assertEqual({t['routed_mask'] for g in plan[4:8] for t in g['tokens']}, set(range(1,64)))
        for g in plan:
            self.assertEqual(len(g['tokens']),16)
            self.assertEqual(len({t['user'] for t in g['tokens']}),16)
            self.assertEqual(len({t['token'] for t in g['tokens']}),16)
            for t in g['tokens']:
                self.assertTrue(0 <= t['user'] <= 1023)
                self.assertEqual(t['user']%6,t['shared_ep'])
                self.assertEqual(len(set(t['global_experts'])),8)
                self.assertEqual(t['mask'],t['routed_mask'] | 1<<t['shared_ep'])
                self.assertAlmostEqual(sum(map(sum,t['topk_weights'])),1,places=6)
                for ep,ids in enumerate(t['expert_ids']):
                    self.assertTrue(all(0<=i<OFFSETS[ep+1]-OFFSETS[ep] for i in ids))

    def test_all_160_serialized_fixtures_match_specs(self):
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp)
            for g in groups():
                for t in g['tokens']:
                    fixture(t,out)
                    verify_fixture(out/(t['name']+'.fixture'),t)

    def test_identical_payload_and_permutation_controls(self):
        plan=groups(); a,b=plan[2]['tokens'][:2]
        self.assertEqual((a['user'],b['user']),(3,1023))
        self.assertEqual(a['expert_ids'],b['expert_ids'])
        for x,y in zip(input_arrays(a),input_arrays(b)): np.testing.assert_array_equal(x,y)
        for a,b in zip(plan[3]['tokens'][:6],plan[3]['tokens'][6:12]):
            self.assertEqual(a['shared_ep'],b['shared_ep'])
            self.assertEqual(a['global_experts'],b['global_experts'][::-1])
            for x,y in zip(input_arrays(a),input_arrays(b)): np.testing.assert_array_equal(x,y)

    def test_native_evidence_rejects_missing_duplicate_identity_and_fake_concurrency(self):
        g=groups()[0]
        masks=''.join(f'pcode mask routed {t["routed_mask"]}, shared EP {t["shared_ep"]}, actual {t["mask"]}\n' for t in g['tokens'])
        sends=''.join(f'pcode sent token {i} at {i+2} user {t["user"]} token {t["token"]}\n' for i,t in enumerate(g['tokens']))
        # Results deliberately arrive out of injection order.
        receives=''.join(f'pcode received token {i} at {9000+i} user {t["user"]} token {t["token"]}\n' for i,t in reversed(list(enumerate(g['tokens']))))
        users=' '.join(str(t['user']) for t in g['tokens'])
        text=masks+'pcode dense model images 768, data bytes 99\n'+sends+receives
        for i,t in enumerate(g['tokens']):
            for kind,cycle in [('head',100+30*i),('tail',125+30*i)]:
                text+=f'pcode wire token {i} {kind} at {cycle} user {t["user"]} token {t["token"]}\n'
        text+=f'pcode concurrent compute users at 1000 {users}\n'
        text+='pcode concurrency summary peak 16 core peak 12 overlapped users 16 stream mask 65535\n'
        text+='pcode stopped at 9500, output packets 16\n[  PASSED  ] 1 test.\n'
        good=evidence(text,g,99)
        self.assertTrue(good['identities_verified'] and good['all_sixteen_compute_verified'] and good['wire_ingress_verified'])
        self.assertFalse(evidence(text.replace('wire token 15 tail','missing token 15 tail'),g,99)['wire_ingress_verified'])
        self.assertFalse(evidence(text+receives.splitlines()[0]+'\n',g,99)['identities_verified'])
        self.assertFalse(evidence(text.replace('users at 1000','users at 9900'),g,99)['all_sixteen_compute_verified'])
        self.assertFalse(evidence(text.replace('peak 16 core','peak 15 core'),g,99)['all_sixteen_compute_verified'])
        self.assertFalse(evidence(text.replace(masks.splitlines()[0],'wrong mask'),g,99)['native_masks_verified'])
        self.assertFalse(evidence(text.replace('data bytes 99','data bytes 98'),g,99)['all_model_images_preloaded'])
        self.assertFalse(evidence(text+'[  FAILED  ] 1 test.\n',g,99)['native_state_checks_passed'])


if __name__=='__main__': unittest.main()
