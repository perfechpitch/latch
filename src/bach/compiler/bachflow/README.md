# BachFlow packed input adapter

The adapter reads `.buff/.bwgt` through `bachflow_runtime`, binds the selected
harvest masks, and writes the existing latch BACHIR11 and Intel HEX inputs.
It does not compile or patch kernels. Execution weights come only from the
packed spans, not the checkpoint or importer intermediates.

Run with BachFlow's shared Python environment and latch's `src` on `PYTHONPATH`:

```bash
PYTHONPATH=<latch>/src <BachFlow>/.venv/bin/python -m bach.compiler.bachflow.input \
  layer.buff layer.bwgt --token user101.token --token user202.token \
  --out-dir <new-directory> --legacy-reduce-mapping
```

The output is `<new-directory>/ep1_adapter/`. An existing destination is rejected.
The adjacent `test/bach/ip/chip/bachflow_input.h` selects the compiler ABI before
loading its configuration and images, then preloads weights through existing `Poke` and
`PokeScale` APIs. `core-map.txt` preserves the packed logical index and role;
inactive cores are spare only in the native-loader configuration.

## Supported input

This adapter is deliberately limited to the current EP1 MXFP8 scene:

- Bach chip shape 4x4 and core shape 2x5. Boundary columns use mask 0; middle columns
  use mask 0x084. Cases are joined by disable mask, not harvest index.
- One shared and 256 routed experts, eight selected per saved 6392-byte token.
- MU template 0, PrimN128/PrimK64, local N128/K768 or N768/K128,
  Bstride 98304. A and both expert weight types are MXFP8.
- Compact NK weight bytes and padded SF with low/high 16-byte halves in each 32-byte
  slot. Data and scales are retiled together, preserving original quantized codes.
- Sparse preload includes the shared expert and the union of token-selected
  routed experts. Routed id `e` stays at physical slot `1+e`; it is not renumbered.
- ITCM at 0 and DTCM at 0x100000. Intel HEX preserves packed BSS zeros and task PCs.
- Packed TS_INIT_FINISH must be 1 on every active core.

Other formats or geometries fail conversion. This is not a general checkpoint
loader or a simulator storage-layout change.

## Explicit experiment

`--legacy-reduce-mapping` is off by default. The current scene requires this
previously approved simulator-only mapping for two same-face reduce shapes.
It premerges two external inputs at their forwarding predecessor and introduces
an extra BF16 rounding boundary. Packed routes and native Router code are not
changed. The default rejects the unsupported packet multiplicity.

The full-run test also exposes `BACHFLOW_INPUT_SF_PREFILL=1` for the approved
activation-scale-one experiment. That prefill is restricted to packed input
activation ranges; original weight scales and VU-generated FC2 scales are kept.
Do not present these experiments as proof of exact hardware equivalence.

The independent reference in `compiler/reference/checkpoint_golden.py` reads the
original complete global safetensors tensors and exact saved token bytes.
It must never read this adapter's weight files or `.bwgt`.

## Reproducing the EP1 checkpoint comparison

Paths below are arguments. Nothing in these commands is a host scratch path.
`<latch>` is the latch checkout. `<BachFlow>` is the BachFlow checkout whose
`defs` and `runtime` the adapter imports. It requires `bachflow.router_word`,
`bachflow.mmem_layout`, `Extent.path` / `offset`, and Runtime's bound-machine
`coord` / `logical` / `role` APIs. These companion BachFlow changes must be
installed; an older checkout is insufficient. `<py>` is its `.venv/bin/python`.

The captured result is terminal CoreMem, read before SID/allocation retirement.
External egress packet collection has not been validated. `compute` reads a local
checkpoint directory in this recipe. A doubled vector, a missing user,
and an unfinished task chain are failures. A transport-only run is not a
numerical result.

The validated full run is an experiment. It needs both opt-ins, and it still
does not by itself accept the numbers:

- `BACHFLOW_LEGACY_REDUCE_MAPPING=1` loads the simulator-only premerge. Two
  same-face reduces are summed at the forwarding predecessor, which adds one
  BF16 rounding the packed route does not have.
- `BACHFLOW_INPUT_SF_PREFILL=1` writes activation scale one into the packed
  input ranges only. Weight scales and VU-generated FC2 scales stay as packed.

`BACHFLOW_TRANSPORT_ONLY=1` stops after the broadcast check. Do not pass it
together with the full-run opt-ins, and do not compare its capture.

The C++ driver accepts `BACHFLOW_USER_COUNT` of 1 or 2 only. User 1 is file
`user101.token`. User 2 adds `user202.token`. Other user ids are not a parameter.

Documented EP1 endpoints, overridable by the same variables:

| end | chip | physical | address |
| --- | --- | --- | --- |
| ingress | 0 | 5 | `0x48c00000` |
| egress | 47 | 8 | `1212156672` |

Egress logical core is 7 (`fc2_concat`). Do not treat it as logical 8.

### 1. Original checkpoint golden

From `<latch>`, with BachFlow's importer importable (`PYTHONPATH` includes
`<BachFlow>` importer src, or the shared venv already has it). Create empty token
and golden directories first. The reference module does not read packed weights.
Use the existing exact token files if available; the seeds below reproduce the
validated inputs.

```bash
cd <latch>
PYTHONPATH=src:<BachFlow>/importer/src \
  <py> -m bach.compiler.reference.checkpoint_golden token \
  --seed 193 --out <token-dir>/user101.token
PYTHONPATH=src:<BachFlow>/importer/src \
  <py> -m bach.compiler.reference.checkpoint_golden token \
  --seed 887 --out <token-dir>/user202.token
PYTHONPATH=src:<BachFlow>/importer/src \
  <py> -m bach.compiler.reference.checkpoint_golden compute \
  --checkpoint <local-checkpoint-dir> --layer 3 \
  --token <token-dir>/user101.token --out <golden-dir>/user101.npz
PYTHONPATH=src:<BachFlow>/importer/src \
  <py> -m bach.compiler.reference.checkpoint_golden compute \
  --checkpoint <local-checkpoint-dir> --layer 3 \
  --token <token-dir>/user202.token --out <golden-dir>/user202.npz
```

`<token-dir>` is what the driver reads (`user101.token`, `user202.token`).
`<golden-dir>` is what the comparison reads (`user101.npz`, `user202.npz`).
They may be the same directory.

### 2. Adapter

`<new-bundle-parent>` must not already contain `ep1_adapter/`.

```bash
PYTHONPATH=<latch>/src \
  <py> -m bach.compiler.bachflow.input \
  <scene>/layer.buff <scene>/layer.bwgt \
  --token <token-dir>/user101.token --token <token-dir>/user202.token \
  --out-dir <new-bundle-parent> --legacy-reduce-mapping
```

### 3. Build and run

`scripts/run-exact-native.sh` lives next to this adapter. Invoke it with `bash`
or as an executable. `LATCH_ROOT` defaults to the latch checkout that contains
the script. The image default is `latch-dev:latest`. The build directory default
is `build-merge-verify` inside that checkout, and the latch mount includes it,
so an existing configured build is reused. `LATCH_SKIP_BUILD=1` runs
`$LATCH_BUILD_DIR/test/bach/bachflow_bundle` and does not call cmake. Cycle
budget default is 3000. Wall-clock default is 300 seconds. User count default
is 1.

```bash
LATCH_ROOT=<latch> \
LATCH_IMAGE=latch-dev:latest \
LATCH_BUILD_DIR=build-merge-verify \
LATCH_SKIP_BUILD=1 \
BACHFLOW_BUNDLE_PARENT=<new-bundle-parent> \
BACHFLOW_BUNDLE_NAME=ep1_adapter \
BACHFLOW_GOLDEN_DIR=<token-dir> \
BACHFLOW_NPZ_DIR=<golden-dir> \
BACHFLOW_OUT_DIR=<new-empty-capture-dir> \
BACHFLOW_USER_COUNT=2 \
BACHFLOW_CYCLE_LIMIT=24000 \
BACHFLOW_WALL_LIMIT=1200 \
BACHFLOW_LEGACY_REDUCE_MAPPING=1 \
BACHFLOW_INPUT_SF_PREFILL=1 \
  bash <latch>/src/bach/compiler/bachflow/scripts/run-exact-native.sh
```

Drop `LATCH_SKIP_BUILD` to configure a Release Ninja build and build
`bachflow_bundle` and `ep1_exact_token`. The short 3000-cycle default is useful
for diagnostics, not a complete two-user run; use the full budget above. The script rejects any user count other than 1 or 2
before Docker starts. A second run needs a new empty `BACHFLOW_OUT_DIR`.

The script does not compare. On a full run it prints the exact next command and
says the driver exit is not acceptance, then returns the driver status. On a
transport-only run it says not to compare.

### 4. Compare

Completion, unique (SID, allocation) ownership, 6144 finite BF16 elements, and the
reference thresholds are all required. The thresholds are `REL_L2 = 0.15`,
norm ratio `[0.85, 1.15]`, cosine `>= 0.99` in `checkpoint_golden.py`. This
command does not change them. Exit 0 is the numerical pass. Exit 1 is a
threshold miss. Any other failure exits nonzero.

```bash
PYTHONPATH=<latch>/src \
  <py> -m bach.compiler.bachflow.compare_output \
  --run <new-empty-capture-dir> \
  --golden <golden-dir> \
  --users 101 \
  --out <new-empty-capture-dir>/metrics.json
```

Two users: `--users 101 202`. Omit `--reference-module`; the default import is
the same-repo `bach.compiler.reference.checkpoint_golden`.
