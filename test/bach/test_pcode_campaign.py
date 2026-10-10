"""Independent checks for fixture generation and acceptance, with deliberate errors."""
import tempfile
import unittest
from pathlib import Path
import numpy as np
from pcode_precision_reference import evaluate
from pcode_campaign import FP8, bf16, widen, quantize, specifications, compare


class CampaignAcceptance(unittest.TestCase):
    def check(self, actual, expected):
        with tempfile.TemporaryDirectory() as directory:
            a, e = Path(directory)/'actual.bf16', Path(directory)/'expected.bf16'
            a.write_bytes(bf16(actual).astype('<u2').tobytes())
            e.write_bytes(bf16(expected).astype('<u2').tobytes())
            return compare(a, e)

    def test_all_nonempty_masks_and_single_tokens(self):
        cases = specifications(64)
        self.assertEqual({s['mask'] for s in cases if s['family']=='routing'},set(range(1,64)))
        for s in cases:
            self.assertLessEqual(sum(s['counts']),8)
            self.assertTrue(s['mask'] & (1 << (s['user']%6)))
            self.assertTrue(0 <= s['user'] <= 1023)
            self.assertTrue(all(not count or s['mask'] & (1 << ep) for ep,count in enumerate(s['counts'])))
        self.assertEqual(len(cases),134)

    def test_numeric_formats_and_quantization(self):
        self.assertEqual(FP8[0x38],1)
        self.assertEqual(FP8[0x08],1/64)
        self.assertEqual(FP8[0x7e],448)
        self.assertEqual(FP8[0xb8],-1)
        np.testing.assert_array_equal(widen(bf16([1,-2,.125])),[1,-2,.125])
        np.testing.assert_array_equal(quantize(np.full(32,.7310586,np.float32)),np.full(32,.75))
        np.testing.assert_array_equal(quantize(np.zeros(32,np.float32)),np.zeros(32))

    def test_missing_contribution_and_wrong_scale_are_rejected(self):
        expected = np.ones(6144,np.float32)
        self.assertTrue(self.check(expected,expected)['passed'])
        omitted=expected.copy();omitted[::6]=0
        self.assertFalse(self.check(omitted,expected)['passed'])
        # Cosine alone cannot detect a globally wrong scale.
        scaled=self.check(expected*2,expected)
        self.assertAlmostEqual(scaled['cosine_similarity'],1)
        self.assertFalse(scaled['passed'])

    def test_reference_honors_intermediate_bf16_rounding(self):
        a,b,sink=(0,0),(0,1),(0,2)
        nodes={a:(True,False),b:(True,True),sink:(True,False)}
        parents={sink:[a,b]}
        sources={a:np.array([1.00390625],np.float32),b:np.array([.0078125],np.float32)}
        actual=evaluate(nodes,parents,sources,sink)
        np.testing.assert_array_equal(actual,[1.0078125])
        with self.assertRaisesRegex(AssertionError,'missing/duplicated'):
            evaluate(nodes,{sink:[a,a,b]},sources,sink)

    def test_zero_does_not_fabricate_cosine(self):
        zero=np.zeros(6144,np.float32)
        report=self.check(zero,zero)
        self.assertTrue(report['passed'])
        self.assertIsNone(report['cosine_similarity'])
        self.assertFalse(self.check(np.ones(6144,np.float32),zero)['passed'])

if __name__=='__main__':
    unittest.main()
