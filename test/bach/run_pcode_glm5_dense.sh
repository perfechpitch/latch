#!/usr/bin/env bash
set -euo pipefail
latch_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
bach_root="${BACH_ROOT:-$latch_root/../BachFlow-ep6-single-token}"
cd "$latch_root"
export PYTHONNOUSERSITE=1
"$bach_root/.venv/bin/cmake" --build build-ep6 --target pcode_bundle -j "${BUILD_JOBS:-4}"
"$bach_root/.venv/bin/python" -u test/bach/pcode_glm5_dense.py \
  --bundle "$bach_root/pcode/output/latch_bundle/pcode_ep6_tp8" \
  --catalog "$bach_root/pcode/output/ep6_single_token/route/route.resolved.json" "$@"
