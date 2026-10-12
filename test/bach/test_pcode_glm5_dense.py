import unittest
import tempfile
from pathlib import Path
from collections import Counter
import numpy as np
from pcode_glm5_dense import specifications, model_arrays, pack, dense_dot, input_arrays, ep_of, OFFSETS, fixture, bf16
from pcode_glm5_dense_verify import verify_fixture
from pcode_glm5_relations import shared_difference_check

class Glm5DenseTests(unittest.TestCase):
    def test_96_cases_and_model_semantics(self):
        cases=specifications()
        self.assertEqual(len(cases),96)
        self.assertEqual(set(sum((s['global_experts'] for s in cases[:32]),[])),set(range(256)))
        self.assertEqual(Counter(s['family'] for s in cases),dict(expert_coverage=32,routing=18,boundary=18,numerical=16,topk_order=6,shared_replica=6))
        for s in cases:
            self.assertEqual(sum(s['counts']),8)
            self.assertEqual(len(set(s['global_experts'])),8)
            self.assertEqual(s['user']%6,s['shared_ep'])
            routed=sum(1<<e for e,c in enumerate(s['counts']) if c)
            self.assertEqual(s['routed_mask'],routed)
            self.assertEqual(s['mask'],routed|1<<s['shared_ep'])
            self.assertAlmostEqual(sum(map(sum,s['topk_weights'])),1,places=6)
            for ep,ids in enumerate(s['expert_ids']):
                self.assertTrue(all(0<=i<OFFSETS[ep+1]-OFFSETS[ep] for i in ids))
    def test_boundaries(self):
        for ep in range(6):
            self.assertEqual(ep_of(OFFSETS[ep]),ep)
            self.assertEqual(ep_of(OFFSETS[ep+1]-1),ep)
        cases={c['name']:c for c in specifications()}
        self.assertEqual(cases['boundary_global_ends']['user'],1023)
        self.assertTrue(any(c['mask']!=c['routed_mask'] for c in cases.values()))
    def test_full_pack_reconstructs_gate_up_and_every_k(self):
        v,s=model_arrays(0,0,3,7);data,sf=pack(v,s)
        self.assertEqual(v.shape,(512,768));self.assertTrue(np.all(v&127))
        restored=np.frombuffer(data,np.uint8).reshape(4,12,128,64).transpose(0,2,1,3).reshape(512,768)
        restored_s=np.frombuffer(sf,np.uint8).reshape(4,12,128,2).transpose(0,2,1,3).reshape(512,24)
        np.testing.assert_array_equal(restored,v);np.testing.assert_array_equal(restored_s,s)
        self.assertFalse(np.array_equal(v[:256],v[256:]))
    def test_model_is_immutable_and_shared_has_no_ep_identity(self):
        for stage in (0,1):
            a,b=model_arrays(-1,stage,0,0);c,d=model_arrays(-1,stage,0,0)
            np.testing.assert_array_equal(a,c);np.testing.assert_array_equal(b,d)
            e,_=model_arrays(0,stage,0,0);self.assertFalse(np.array_equal(a,e))
    def test_topk_reversal_keeps_bindings(self):
        cases={c['name']:c for c in specifications()}
        for i in range(3):
            a,b=cases[f'order_{i}_forward'],cases[f'order_{i}_reverse']
            for ep in range(6):
                self.assertEqual(dict(zip(a['expert_ids'][ep],a['topk_weights'][ep])),dict(zip(b['expert_ids'][ep],b['topk_weights'][ep])))
            for x,y in zip(input_arrays(a),input_arrays(b)): np.testing.assert_array_equal(x,y)
    def test_dense_dot_all_k_contributes(self):
        v=np.full((128,768),0x38,np.uint8);s=np.full((128,24),127,np.uint8)
        x=np.full(768,0x38,np.uint8);xs=np.full(24,127,np.uint8)
        np.testing.assert_array_equal(dense_dot(v,s,x,xs),np.full(128,768,np.float32))
        v[:,-1]=0
        np.testing.assert_array_equal(dense_dot(v,s,x,xs),np.full(128,767,np.float32))
    def test_saved_fixture_rejects_wrong_mask_and_topk_binding(self):
        spec=specifications()[0]
        with tempfile.TemporaryDirectory() as temp:
            out=Path(temp);fixture(spec,out);path=out/(spec['name']+'.fixture')
            verify_fixture(path,spec)
            original=path.read_bytes();bad=bytearray(original)
            bad[16] ^= 2;path.write_bytes(bad)
            with self.assertRaises(AssertionError):verify_fixture(path,spec)
            bad=bytearray(original);bad[28] ^= 1;path.write_bytes(bad)
            with self.assertRaises(AssertionError):verify_fixture(path,spec)
    def test_shared_placement_accounts_for_expected_rounding_without_relaxing_output_thresholds(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            a=np.ones(6144,np.float32);b=a.copy();b[::2]=1.03125
            paths=[root/name for name in ['a','b','ra','rb']]
            for path,values in zip(paths,[a,b,a,b]):path.write_bytes(bf16(values).astype('<u2').tobytes())
            result=shared_difference_check(*paths)
            self.assertTrue(result['passed'])
            self.assertFalse(result['raw_actual_comparison']['passed'])
            self.assertEqual(result['difference_residual_relative_l2'],0)
            paths[1].write_bytes(bf16(b*2).astype('<u2').tobytes())
            self.assertFalse(shared_difference_check(*paths)['passed'])

if __name__=='__main__':unittest.main()
