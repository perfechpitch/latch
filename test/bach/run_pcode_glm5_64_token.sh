#!/usr/bin/env bash
set -euo pipefail
latch_root=$(cd "$(dirname "$0")/../.." && pwd)
bach_root=${BACH_ROOT:-"$latch_root/../BachFlow-ep6-single-token"}
cd "$latch_root"
export PYTHONNOUSERSITE=1
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 VECLIB_MAXIMUM_THREADS=1
"$bach_root/.venv/bin/cmake" --build build-ep6 --target pcode_bundle -j "${BUILD_JOBS:-4}"
"$bach_root/.venv/bin/python" -B -u test/bach/pcode_glm5_64_token.py \
  --bundle "$bach_root/pcode/output/latch_bundle/pcode_ep6_tp8" \
  --catalog "$bach_root/pcode/output/ep6_single_token/route/route.resolved.json" \
  --threads 4 --workers 1 "$@"
