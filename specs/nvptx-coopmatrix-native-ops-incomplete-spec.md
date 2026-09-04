# NVPTX cooperative-matrix lowering is incomplete, so 50 quant kernels get no
# device code and silently produce zeros on NVIDIA

**draft** — filed 2026-09-04. Root-caused from a full `cajeta-llm` run on the
NVIDIA (nvptx) backend on the RTX 4090: **319 passed, 37 failed**, every failure
a device kernel returning all-zeros. This spec is the compiler-side defect
behind the "nvptx wrong-output" blocker that stops the NVIDIA inference path and
therefore any llama.cpp comparison on NVIDIA.

## Symptom

`cajeta-llm` compiled for `--xpu-backend=nvptx` builds WITHOUT error (exit 0, no
ptxas failure), then at runtime 37 QuantKernel/Wmma/Ragged/Moe tests fail with
"every value is zero, which is what a kernel that never ran produces" and the
launch path prints `cajeta.xpu: no registered kernel '<name>' to launch` for 56
distinct kernels (q4kWmmaKernel, q4kWidenKernel, qNkF16CoopX3Kernel,
qNkWmmaDeq*, …). Simple kernels are fine — `saxpy` (float32) and int8 buffer
round-trip both run correctly on the same device, so this is neither an int8
KernelBuffer bug nor a general nvptx-execution bug.

## Cause

`lowerKernel` THROWS in `NvptxKernelLowering` for 50 kernels at BUILD time; the
`[xpu-kernel-skipped]` path catches it, emits a note to stderr, and `continue`s
— the kernel gets no NVPTX cubin, is never registered, and a launch produces a
zero-initialised output. The distinct reasons, counted from the build stderr:

- **32×** `CooperativeMatrix.load got a non-row-major or non-constant layout`
  ("NVPTX native cooperative matrix (v1) supports only row-major operands
  (layout N)"). NVIDIA's `nvvm.wmma.load` HAS column-major variants; the cajeta
  NVPTX lowering only wired layout N and rejects the rest.
- **~11×** `CooperativeMatrix.scaledAccumIntoS` / `scaledAccumIntoNS` are
  NATIVE-ONLY and unimplemented on NVPTX (the scalar colF/colG is this lane's
  column factor; needs a native lane-column mapping).
- **~4×** `CooperativeMatrix.fromWords` NATIVE-ONLY, unimplemented on NVPTX
  (the words are this lane's fragment row/column; needs a native lane mapping).

These WMMA quant kernels were authored and validated on AMD gfx1151 (the
`cajeta-llm` Unit 32 work); their NVPTX cooperative-matrix path was never
completed, so every kernel that uses a col-major fragment load or the
`scaledAccumInto{,N}` / `fromWords` lane-column ops falls off the native tier
and is skipped.

## Why this is a defect and not the guard working as intended

Two independent failures compound:

1. **The lowering gap** — NVPTX has no path for constructs the kernels legitimately
   use (col-major load exists in hardware; the lane-column ops have an AMD
   implementation to port or a portable-tier fallback to route to).
2. **The silent cliff** — a kernel that fails to lower does NOT fail the build.
   It prints one note among dozens in a build log the test harness discards
   (`run-tests.sh` sends the build to `/dev/null`), then ships a binary that
   computes zeros. This is the same "vanished silently" hazard as the fixed
   `nvptx-coop-bf16-fragment-abort`, now at emit rather than at assert. A device
   inference binary silently returning wrong answers is worse than a build that
   fails loudly.

## Fix sketch

Two tracks; the second is the safety net for whatever the first does not cover.

1. **Complete the NVPTX native path** for the three constructs:
   - `CooperativeMatrix.load` column-major / layout-M via the `_col` NVVM
     `wmma.load` intrinsic variants (the row-major-only guard becomes a
     layout→intrinsic selection).
   - `scaledAccumIntoS` / `scaledAccumIntoNS` and `fromWords` on NVPTX — port
     the AMD lane-column mapping, or express them on the NVVM fragment ABI.
2. **Demote, do not skip** — mirror `amdgpu-coopmatrix-tier-straddle`'s fix: when
   a cooperative-matrix op has no native NVPTX path, route the kernel to the
   PORTABLE/software tile (the `CAJETA_GPU_COOPMATRIX_IMPL=software` path) as a
   GROUP, emitting an `[mma-tiering]` note, instead of throwing and skipping.
   A kernel then always lowers — accelerated where native exists, correct
   (unaccelerated) where it does not — and never silently vanishes.

Additionally: an `--xpu-backend=nvptx` build that skips ANY @Kernel for lack of
device code should FAIL by default (or under a `--strict-device` flag the test
harness sets), converting the silent cliff into a build error. The current note
is necessary but not sufficient.

## Workaround until fixed — MEASURED, and only partial (2026-09-04)

`CAJETA_GPU_COOPMATRIX_IMPL=software cajeta … --xpu-backend=nvptx` forces the
portable tile for every cooperative matrix. Measured on the 4090, it moved the
suite from **319 passed / 37 failed to 330 passed / 26 failed** — it recovers
the 11 kernels that were skipped PURELY for the coop-matrix native-only reason,
but **26 still fail**, and the remaining failures wear a DIFFERENT signature:
"expected condition to be true" (a logic/numeric divergence), not "every value
is zero / kernel that never ran". Sampled classes: `WmmaIdTileTest`
(q4k/q6k Epi/Mw id-tile agreement), `SafetensorsTest`, `ResidentMoeDecodeTest`,
`RaggedTest`.

So there is NO one-env-var correct NVIDIA path: the coop-matrix skip is the
DOMINANT cause (11 of the family) but not the only one. The remaining 26 are
either (a) constructs the software tier still cannot lower, or (b) genuine
software-tier / portable-path correctness bugs on NVPTX that AMD never exercised
because AMD took the native tier. Enumerating and splitting the 26 is Unit 0's
first task — the tail-only capture of the measuring run showed 7 of them; a
full-output rerun is needed to list all 26 with their messages.

## Acceptance

- A full `cajeta-llm` `--xpu-backend=nvptx` run on the 4090 with no override:
  0 device tests fail with "kernel that never ran", 0 `no registered kernel`
  at runtime.
- With `CAJETA_GPU_COOPMATRIX_IMPL=software`: 0 device tests fail. MEASURED
  2026-09-04 to be INSUFFICIENT alone (37 -> 26), so this acceptance requires
  fixing the residual 26 as well, not just the coop-matrix skip.
- A kernel using a col-major `CooperativeMatrix.load` lowers on NVPTX and runs
  on device, matching the host reference (a new `XpuCooperativeMatrixDeviceTests`
  case — currently only `nvptxCoopMatrixLowersToWmma` exists, row-major, emit
  only).
- An `--xpu-backend=nvptx` build that cannot give a @Kernel device code fails
  the build under strict mode, rather than shipping a zero-producing binary.
- `saxpy` and the int8-buffer round-trip stay green (negative controls: the fix
  is scoped to cooperative matrix, not buffers).
