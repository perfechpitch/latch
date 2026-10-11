"""Reject paired tests that silently replace an expert's weights."""
import struct
import tempfile
import unittest
from pathlib import Path
from pcode_two_token import audit_shared_model


class SharedModelAcceptance(unittest.TestCase):
    def fixture(self,path,coefficient,expert=0):
        header=b'PCDTST02'+struct.pack('<III',0,1,63)+struct.pack('<6I',*([0]*6))
        matrix=struct.pack('<6I',0,0,0,expert,1,1)+struct.pack('<I',0)+bytes([coefficient,127])
        path.write_bytes(header+bytes(6144+192)+struct.pack('<I',1)+matrix)

    def test_same_expert_cannot_have_different_weights(self):
        with tempfile.TemporaryDirectory() as directory:
            a,b=Path(directory)/'a',Path(directory)/'b'
            self.fixture(a,0x20);self.fixture(b,0x20)
            self.assertTrue(audit_shared_model(a,b)['immutable'])
            self.fixture(b,0x18)
            with self.assertRaisesRegex(AssertionError,'matrices differ'):audit_shared_model(a,b)

    def test_distinct_experts_can_coexist(self):
        with tempfile.TemporaryDirectory() as directory:
            a,b=Path(directory)/'a',Path(directory)/'b'
            self.fixture(a,0x20,0);self.fixture(b,0x18,1)
            result=audit_shared_model(a,b)
            self.assertEqual(result['common_matrices'],0)
            self.assertEqual(result['union_matrices'],2)


if __name__=='__main__':unittest.main()
