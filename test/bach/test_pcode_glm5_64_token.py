import tempfile
import unittest
from pathlib import Path
import numpy as np
from pcode_glm5_64_token import groups, evidence, verdict, N
from pcode_glm5_dense import fixture, input_arrays, OFFSETS
from pcode_glm5_dense_verify import verify_fixture

class Glm5Stream64Tests(unittest.TestCase):
    def test_plan_and_all_serialized_inputs(self):
        plan=groups();self.assertEqual(len(plan),10)
        self.assertEqual({e for t in plan[0]['tokens'] for e in t['global_experts']},set(range(256)))
        self.assertEqual({e for t in plan[1]['tokens'] for e in t['global_experts']},set(range(256)))
        with tempfile.TemporaryDirectory() as temp:
            for g in plan:
                self.assertEqual(len(g['tokens']),N);self.assertEqual(len({t['user'] for t in g['tokens']}),N)
                for t in g['tokens']:
                    self.assertTrue(0<=t['token']<=65535);self.assertEqual(t['user']%6,t['shared_ep']);self.assertTrue(0<=t['user']<=1023)
                    self.assertEqual(len(set(t['global_experts'])),8)
                    self.assertEqual(t['actual_ep_mask'],t['routed_mask']|1<<t['shared_ep'])
                    self.assertAlmostEqual(sum(map(sum,t['topk_weights'])),1,places=6)
                    self.assertTrue(all(w>=0 for ws in t['topk_weights'] for w in ws))
                    for ep,ids in enumerate(t['expert_ids']):self.assertTrue(all(0<=e<OFFSETS[ep+1]-OFFSETS[ep] for e in ids))
                    fixture(t,Path(temp));verify_fixture(Path(temp)/(t['name']+'.fixture'),t)
        for g in plan[4:6]:self.assertEqual({t['routed_mask'] for t in g['tokens']},set(range(1,64)))

    def test_payload_and_permutation_controls(self):
        p=groups();a,b=p[2]['tokens'][:2];self.assertEqual([a['user'],b['user']],[3,1023])
        for x,y in zip(input_arrays(a),input_arrays(b)):np.testing.assert_array_equal(x,y)
        for batch in range(5):
            for ep in range(6):
                a,b=[p[3]['tokens'][batch*12+ep+off] for off in [0,6]]
                self.assertEqual(a['global_experts'],b['global_experts'][::-1])
                for x,y in zip(input_arrays(a),input_arrays(b)):np.testing.assert_array_equal(x,y)

    def test_native_evidence_and_functional_verdict_do_not_require_64_simultaneous(self):
        g=groups()[0];lines=['pcode dense model images 768, data bytes 99']
        for i,t in enumerate(g['tokens']):
            lines += [f'pcode mask routed {t["routed_mask"]}, shared EP {t["shared_ep"]}, actual {t["actual_ep_mask"]}',
                f'pcode sent token {i} at {2+i} user {t["user"]} token {t["token"]}',
                f'pcode wire token {i} head at {100+30*i} user {t["user"]} token {t["token"]}',
                f'pcode wire token {i} tail at {125+30*i} user {t["user"]} token {t["token"]}']
        for i in reversed(range(N)):
            t=g['tokens'][i];lines.append(f'pcode received token {i} at {9000+i} user {t["user"]} token {t["token"]}')
        lines += ['pcode concurrency summary peak 31 core peak 16 overlapped users 64 stream mask 65535',
                  'pcode stopped at 9500, output packets 64','[  PASSED  ] 1 test.']
        text='\n'.join(lines);e=evidence(text,g,99)
        r=dict(e,simulator_returncode=0,maximum_cycles=10000,tokens=[dict(passed=True) for _ in range(N)])
        self.assertEqual(verdict(r,text),'PASS')
        self.assertFalse(evidence(text.replace('wire token 63 tail','missing token 63 tail'),g,99)['wire_ingress_verified'])
        line=next(s for s in lines if 'received token' in s)
        self.assertFalse(evidence(text+'\n'+line,g,99)['identities_verified'])
        self.assertFalse(evidence(text.replace('data bytes 99','data bytes 98'),g,99)['all_model_images_preloaded'])
        r.update(simulator_returncode=-6)
        self.assertEqual(verdict(r,'assertion failed: SameNet'),'REDUCE_STAGE_ASSERT')
        r.update(simulator_returncode=1,completed_cycles=10000)
        self.assertEqual(verdict(r,text),'CYCLE_LIMIT_REQUIRES_DEADLOCK_ANALYSIS')

if __name__=='__main__':unittest.main()
