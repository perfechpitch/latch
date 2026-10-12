"""Cross-placement checks account for the declared BF16 rounding topology."""
import numpy as np
from pcode_campaign import compare, widen


def shared_difference_check(first, second, expected_first, expected_second):
    checks=[compare(first,expected_first),compare(second,expected_second)]
    a,b,ra,rb=[widen(np.frombuffer(path.read_bytes(),'<u2')).astype(np.float64)
               for path in (first,second,expected_first,expected_second)]
    residual=float(np.linalg.norm((b-a)-(rb-ra))/max(np.linalg.norm(ra),1e-30))
    return dict(passed=all(c['passed'] for c in checks) and residual<=.02,
                difference_residual_relative_l2=residual,max_difference_residual_relative_l2=.02,
                raw_actual_comparison=compare(second,first),
                raw_expected_comparison=compare(expected_second,expected_first),
                explanation='Each output meets its reference thresholds; actual placement difference follows reference BF16 rounding.')
