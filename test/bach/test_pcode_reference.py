"""Validate the acceptance check, independently of any simulator run."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from pcode_reference import bf16, expected_values


class ReferenceAcceptance(unittest.TestCase):
    def run_check(self, words):
        with tempfile.TemporaryDirectory() as directory:
            actual = Path(directory) / 'actual.bf16'
            actual.write_bytes(struct.pack('<6144H', *words))
            result = subprocess.run([
                sys.executable, str(Path(__file__).with_name('pcode_reference.py')),
                str(actual)], capture_output=True, text=True)
        return result, json.loads(result.stdout)

    def test_exact_output_has_unit_cosine(self):
        result, report = self.run_check(expected_values()[0])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(report['cosine_similarity'], 1.0)
        self.assertEqual(report['max_absolute_error'], 0.0)

    def test_missing_ep_contribution_fails_cosine(self):
        words = expected_values()[0]
        for i in range(6144):
            if (i % 128) % 6 == 5:
                words[i] = bf16(0.1875 if i % 2 == 0 else -0.1875)
        result, report = self.run_check(words)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('cosine similarity', result.stderr)
        self.assertLess(report['cosine_similarity'], report['min_cosine'])


if __name__ == '__main__':
    unittest.main()
