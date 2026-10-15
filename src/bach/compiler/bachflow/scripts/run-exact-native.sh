#!/usr/bin/env bash
# Bounded EP1 exact-token run inside the latch Docker image.
#
# Transport-only and the approved full-run experiments are different exits.
# A full run requires both explicit opt-ins and never claims numerical
# acceptance; compare_output.py is the only numerical gate.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
latch_root="${LATCH_ROOT:-$(cd "$script_dir/../../../../.." && pwd)}"
image="${LATCH_IMAGE:-latch-dev:latest}"
build_dir="${LATCH_BUILD_DIR:-build-merge-verify}"
jobs="${LATCH_BUILD_JOBS:-4}"

bundle_parent="${BACHFLOW_BUNDLE_PARENT:-}"
bundle_name="${BACHFLOW_BUNDLE_NAME:-ep1_adapter}"
golden_dir="${BACHFLOW_GOLDEN_DIR:-}"
out_dir="${BACHFLOW_OUT_DIR:-}"

cycles="${BACHFLOW_CYCLE_LIMIT:-3000}"
users="${BACHFLOW_USER_COUNT:-1}"
wall="${BACHFLOW_WALL_LIMIT:-300}"
skip_build="${LATCH_SKIP_BUILD:-0}"
npz_dir="${BACHFLOW_NPZ_DIR:-}"

transport="${BACHFLOW_TRANSPORT_ONLY:-0}"
legacy="${BACHFLOW_LEGACY_REDUCE_MAPPING:-0}"
sf_one="${BACHFLOW_INPUT_SF_PREFILL:-0}"

usage() {
  cat <<'EOF'
Usage: run-exact-native.sh

Required:
  BACHFLOW_BUNDLE_PARENT   host directory that contains <bundle name>/
  BACHFLOW_GOLDEN_DIR      host directory with user101.token (and user202.token for 2 users)
  BACHFLOW_OUT_DIR         new or empty host directory for this run's capture

Mode (exactly one):
  BACHFLOW_TRANSPORT_ONLY=1
      stops after broadcast. Not numerical acceptance.
  BACHFLOW_LEGACY_REDUCE_MAPPING=1 and BACHFLOW_INPUT_SF_PREFILL=1
      bounded full execution of the approved experiments. Still not a
      numerical pass; run compare_output.py on the capture afterwards.

Optional:
  LATCH_ROOT               latch checkout mounted at /work/latch
                           (default: five levels above this script)
  LATCH_IMAGE              Docker image (default: latch-dev:latest)
  LATCH_BUILD_DIR          CMake build dir inside the latch root, already
                           configured or not (default: build-merge-verify).
                           The latch checkout mount includes this directory.
  LATCH_SKIP_BUILD         1 runs the binary already in LATCH_BUILD_DIR and
                           does not call cmake. Default 0 configures a Release
                           Ninja build and builds the two targets.
  LATCH_BUILD_JOBS         cmake --parallel (default: 4)
  BACHFLOW_NPZ_DIR         host directory of user101.npz / user202.npz, used
                           only in the printed compare command. Default is
                           BACHFLOW_GOLDEN_DIR.
  BACHFLOW_BUNDLE_NAME     bundle directory name (default: ep1_adapter)
  BACHFLOW_CYCLE_LIMIT     driver cycle budget (default: 3000)
  BACHFLOW_USER_COUNT      1 or 2 (default: 1). The driver hardcodes users
                           101 and 202; any other count is rejected here.
  BACHFLOW_WALL_LIMIT      timeout(1) seconds around the binary (default: 300)
  BACHFLOW_INGRESS_CHIP/PHYS/ADDR
  BACHFLOW_EGRESS_CHIP/PHYS/ADDR
                           EP1 endpoints. Defaults: chip 0 phys 5 addr
                           0x48c00000 in; chip 47 phys 8 addr 1212156672 out.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi
if [[ $# -ne 0 ]]; then
  usage >&2
  exit 2
fi

fail() {
  printf '%s\n' "$1" >&2
  exit 2
}

[[ -n "$bundle_parent" ]] || fail "BACHFLOW_BUNDLE_PARENT is required (host directory containing the bundle)."
[[ -n "$golden_dir" ]] || fail "BACHFLOW_GOLDEN_DIR is required."
[[ -n "$out_dir" ]] || fail "BACHFLOW_OUT_DIR is required (a new directory for this run)."
[[ "$users" == 1 || "$users" == 2 ]] || fail "BACHFLOW_USER_COUNT must be 1 or 2 (driver hardcodes users 101 and 202)."
[[ "$cycles" =~ ^[0-9]+$ && "$cycles" -gt 0 ]] || fail "BACHFLOW_CYCLE_LIMIT must be a positive integer."
[[ "$wall" =~ ^[0-9]+$ && "$wall" -gt 0 ]] || fail "BACHFLOW_WALL_LIMIT must be a positive integer."
[[ "$skip_build" == 0 || "$skip_build" == 1 ]] || fail "LATCH_SKIP_BUILD must be 0 or 1."

full=0
if [[ "$transport" == 1 && "$legacy" == 0 && "$sf_one" == 0 ]]; then
  :
elif [[ "$transport" == 0 && "$legacy" == 1 && "$sf_one" == 1 ]]; then
  full=1
else
  fail "Choose transport-only (BACHFLOW_TRANSPORT_ONLY=1) or the approved full run (BACHFLOW_LEGACY_REDUCE_MAPPING=1 and BACHFLOW_INPUT_SF_PREFILL=1). Do not combine them. A full run is not numerical acceptance."
fi

[[ -d "$latch_root" ]] || fail "LATCH_ROOT is not a directory: $latch_root"
[[ -d "$bundle_parent/$bundle_name" ]] || fail "Bundle not found: $bundle_parent/$bundle_name"
[[ -d "$golden_dir" ]] || fail "Golden directory not found: $golden_dir"
[[ -f "$golden_dir/user101.token" ]] || fail "Missing $golden_dir/user101.token"
if [[ "$users" == 2 ]]; then
  [[ -f "$golden_dir/user202.token" ]] || fail "Missing $golden_dir/user202.token"
fi

if [[ -e "$out_dir" ]]; then
  [[ -d "$out_dir" ]] || fail "BACHFLOW_OUT_DIR exists and is not a directory: $out_dir"
  [[ -z "$(ls -A "$out_dir")" ]] || fail "BACHFLOW_OUT_DIR is not empty: $out_dir"
else
  mkdir -p "$out_dir"
fi

latch_root="$(cd "$latch_root" && pwd)"
bundle_parent="$(cd "$bundle_parent" && pwd)"
golden_dir="$(cd "$golden_dir" && pwd)"
out_dir="$(cd "$out_dir" && pwd)"

ingress_chip="${BACHFLOW_INGRESS_CHIP:-0}"
ingress_phys="${BACHFLOW_INGRESS_PHYS:-5}"
ingress_addr="${BACHFLOW_INGRESS_ADDR:-0x48c00000}"
egress_chip="${BACHFLOW_EGRESS_CHIP:-47}"
egress_phys="${BACHFLOW_EGRESS_PHYS:-8}"
egress_addr="${BACHFLOW_EGRESS_ADDR:-1212156672}"

mode_note="transport-only; not a numerical result"
if [[ "$full" == 1 ]]; then
  mode_note="full run with explicit legacy mapping and SF-one; compare_output.py decides acceptance"
fi
printf '%s\n' "exact-native: $mode_note"
printf '%s\n' "latch=$latch_root image=$image build=$build_dir users=$users cycles=$cycles wall=${wall}s"

set +e
docker run --rm \
  -v "$latch_root:/work/latch" \
  -v "$bundle_parent:/work/bundle:ro" \
  -v "$golden_dir:/work/golden:ro" \
  -v "$out_dir:/work/exact-out" \
  -w /work/latch \
  -e BACHFLOW_EXACT_RUN=1 \
  -e BACHFLOW_BUNDLE_ROOT=/work/bundle \
  -e BACHFLOW_BUNDLE_NAME="$bundle_name" \
  -e BACHFLOW_GOLDEN_DIR=/work/golden \
  -e BACHFLOW_OUT_DIR=/work/exact-out \
  -e BACHFLOW_INGRESS_CHIP="$ingress_chip" \
  -e BACHFLOW_INGRESS_PHYS="$ingress_phys" \
  -e BACHFLOW_INGRESS_ADDR="$ingress_addr" \
  -e BACHFLOW_EGRESS_CHIP="$egress_chip" \
  -e BACHFLOW_EGRESS_PHYS="$egress_phys" \
  -e BACHFLOW_EGRESS_ADDR="$egress_addr" \
  -e BACHFLOW_TRANSPORT_ONLY="$transport" \
  -e BACHFLOW_LEGACY_REDUCE_MAPPING="$legacy" \
  -e BACHFLOW_INPUT_SF_PREFILL="$sf_one" \
  -e BACHFLOW_CYCLE_LIMIT="$cycles" \
  -e BACHFLOW_USER_COUNT="$users" \
  -e BACHFLOW_WALL_LIMIT="$wall" \
  -e LATCH_BUILD_DIR="$build_dir" \
  -e LATCH_BUILD_JOBS="$jobs" \
  -e LATCH_SKIP_BUILD="$skip_build" \
  "$image" bash -lc \
  'set -euo pipefail
   bin="$LATCH_BUILD_DIR/test/bach/bachflow_bundle"
   if [ "$LATCH_SKIP_BUILD" = 1 ]; then
     [ -x "$bin" ] || { printf "%s\n" "Missing built binary: $bin" >&2; exit 2; }
   else
     cmake -S . -B "$LATCH_BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
     cmake --build "$LATCH_BUILD_DIR" --parallel "$LATCH_BUILD_JOBS" --target bachflow_bundle ep1_exact_token
   fi
   timeout "$BACHFLOW_WALL_LIMIT" "$bin"'
status=$?

if [[ "$full" == 1 ]]; then
  compare_golden="${npz_dir:-$golden_dir}"
  user_args="101"
  if [[ "$users" == 2 ]]; then
    user_args="101 202"
  fi
  printf '%s\n' "numerical comparison was not run; driver exit ${status} is not acceptance"
  printf '%s\n' "next: PYTHONPATH=${latch_root}/src python -m bach.compiler.bachflow.compare_output --run ${out_dir} --golden ${compare_golden} --users ${user_args} --out ${out_dir}/metrics.json"
else
  printf '%s\n' "transport-only finished (exit ${status}); do not run numerical compare on this capture"
fi
exit "$status"
