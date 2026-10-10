"""Independent scalar reference for the nonzero EP6/TP8 exported-program fixture.

No simulator code or simulator-generated reference values are imported. Eight
TP contributions produce FC1=FC3=1. Each expert's matrices have one nonzero
coefficient per tile. All FC2/EP sums are exactly representable in BF16.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def bf16(value):
    word = struct.unpack('<I', struct.pack('<f', value))[0]
    return ((word + 0x7FFF + ((word >> 16) & 1)) >> 16) & 0xFFFF


def decode_fp8(code):
    exp, mantissa = code >> 3, code & 7
    return mantissa * 2.0**-9 if exp == 0 else (1 + mantissa / 8) * 2.0**(exp - 7)


def decode_bf16(word):
    return struct.unpack('<f', struct.pack('<I', word << 16))[0]


def expected_values(case='full'):
    sigmoid = f32(1 / f32(1 + f32(math.exp(-1))))
    activation = f32(sigmoid * 1)
    # VU configuration has MXFP8_SCALE_ROUND=1: ceil the E8M0 exponent.
    scale = 2.0 ** math.ceil(math.log2(activation / 448))
    value = min(448, activation / scale)
    code = min(range(127), key=lambda c: (abs(decode_fp8(c) - value), c & 1))
    quantized = decode_fp8(code) * scale
    counts = [2, 2, 1, 1, 1, 1] if case == 'full' else [2, 2, 0, 2, 0, 2]
    shared = 8 * quantized / 32
    totals = [shared + quantized * (ep+1) * counts[ep] / 64 for ep in range(6)]
    expected = []
    for i in range(6144):
        total = totals[(i % 128) % 6]
        expected.append(bf16(total if i % 2 == 0 else -total))
    return expected, {
        'case': case, 'expert_counts_per_ep': counts,
        'ep': 6, 'tp': 8, 'tokens': 1, 'routed_experts': sum(counts),
        'active_ep_groups': sum(c > 0 for c in counts), 'shared_expert_ep': 0,
        'fc1_fc3': 1.0, 'silu': activation,
        'mxfp8_activation': quantized, 'expected_output_abs_per_ep_column': totals,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('actual', type=Path)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--case', choices=['full', 'gap'], default='full')
    parser.add_argument('--min-cosine', type=float, default=0.99999)
    args = parser.parse_args()
    actual = args.actual.read_bytes()
    expected, report = expected_values(args.case)
    if len(actual) != 12288:
        raise AssertionError(f'expected 12288 output bytes, got {len(actual)}')
    words = struct.unpack('<6144H', actual)
    mismatches = [i for i, (a, e) in enumerate(zip(words, expected)) if a != e]
    actual_values = [decode_bf16(w) for w in words]
    reference_values = [decode_bf16(w) for w in expected]
    dot = math.fsum(a * e for a, e in zip(actual_values, reference_values))
    norms = math.sqrt(math.fsum(a*a for a in actual_values) *
                      math.fsum(e*e for e in reference_values))
    cosine = dot / norms if norms else float('nan')
    max_error = max(abs(a-e) for a, e in zip(actual_values, reference_values))
    report.update(elements=6144, mismatches=len(mismatches),
                  cosine_similarity=cosine, min_cosine=args.min_cosine,
                  max_absolute_error=max_error,
                  actual_sha256=hashlib.sha256(actual).hexdigest())
    print(json.dumps(report, indent=2))
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    if not math.isfinite(cosine) or cosine < args.min_cosine:
        raise AssertionError(f'cosine similarity {cosine} < {args.min_cosine}')
    if mismatches:
        i = mismatches[0]
        raise AssertionError(f'first mismatch at {i}: actual=0x{words[i]:04x}, expected=0x{expected[i]:04x}')


if __name__ == '__main__':
    main()
