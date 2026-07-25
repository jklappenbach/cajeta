# GPU numeric fidelity — reproducing the device's float arithmetic bit-for-bit

## 1. Definition

### 1.1 Purpose
cajeta-xgboost reproduces XGBoost's NVIDIA GPU `hist` algorithm bit-for-bit. The
tree structure, the histogram sums, the boosting loop, prediction, serialization,
and the quantile sketch structure are all bit-exact (see `cajeta-xgboost-spec.md`,
units U3–U11). What remains are a small family of residuals that share one root
cause: **the GPU evaluates certain float32 expressions in an operation order — or
with silicon primitives — that cajeta's mathematically-faithful CPU code does not
reproduce to the last bit.** Where the true value is on a rounding boundary, that
last bit changes an observable output.

This is not hardware error. The GPU is deterministic (the pinned reference
reproduces identically). It is a **match-the-silicon's-arithmetic** problem: to be
bit-exact, cajeta must evaluate the same float32 ops in the same order, and model
the SFU/transcendental primitives the device uses.

### 1.2 Scope
Three residuals, one class:

1. **Split-selection near-tie** (the headline "rounding issue"). At a node where two
   candidate splits have gains equal to within float32 rounding, the GPU's argmax and
   cajeta's disagree — different feature/boundary, hence different tree topology.
2. **Device `expf`** (transcendental fidelity). Binary/multiclass gradients past
   round 0 use device `expf`; cajeta uses libm `expf`. They differ by ~1 ULP, which
   crosses the gradient quantiser boundary on some rows.
3. **Pruned-sketch interior cut values** (float-scan fidelity). The GPU device
   sketch selects interior quantile cuts via a float prefix-scan/prune whose
   accumulation tips ~83% of interior boundaries one distinct value off cajeta's
   exact integer-summary `SetPrune`. (Structure and endpoints are already bit-exact.)

**Non-goal:** the actual GPU *kernels* (`cajeta.xpu` histogram/split, cross-backend
NVIDIA→Strix Halo). That is `cajeta-xgboost` U12; this spec is the numeric-fidelity
prerequisite it depends on, not the kernel port.

### 1.3 Precedent (why this is tractable)
Two residuals of this exact class were already resolved by matching the arithmetic,
not by touching hardware:
- **`__fdividef`** (U4): probed on RTX 4090 → `__fdividef == div.approx.f32 ==
  a·rcp.approx(b)`; reproduced on CPU via the captured 2^23 `MUFU.RCP` mantissa table.
- **Signed-zero leaf weight** (2026-07-24): the GPU's `CalcWeight = −G/(H+λ)` is a
  *unary* negation, so `Σgrad = 0` gives `−0.0`; cajeta wrote `0.0 − G` (`+0.0`).
  A one-line fix (unary `−`) made balanced-data trees bit-exact. **Not every residual
  needs hardware** — some are plain CPU op-order bugs; the method (probe → model →
  reproduce in emulated float32) is the same either way.

### 1.4 Method
For each residual: (a) capture the device's exact behavior on real NVIDIA hardware
(a focused `.cu` probe over the relevant domain), (b) model it as a reproducible
float32 computation (op order + any SFU table), (c) validate the CPU model bit-for-bit
against the capture, (d) wire it into cajeta and re-enable the gated parity test.

### 1.5 Systems
`xgboost-ref` (3.1.2 source, the algorithm), an NVIDIA GPU (probe capture; the same
box that generates the golden fixtures), `tools/fdividef` + `tools/expf` (existing
captures), cajeta `FastMath` (the SFU-op host models). All captures are `.gitignore`d
build inputs, regenerated on the box; the CPU models + validation live in the repo.

---

## 2. Split-selection near-tie *(the rounding issue)*

The GPU scores each candidate boundary's gain `G²/(H+λ)` in float32 and takes an
argmax across features and boundaries. cajeta reproduces the gain arithmetic
faithfully but not yet in the device's exact op order (histogram reduction order +
`MUFU.RCP` reciprocal + the fixed-point dequantise). Where two gains are equal to
within rounding, the two argmaxes pick different splits.

Confirmed **tiny-fixture-only**: `tiny_reg_mcw10` node 14, `tiny_binary` node 11,
`tiny_multi` round-0 k-trees diverge; **`large_reg` (20k×50, depth 6) and
`large_binary` are bit-exact per node** — real-scale data rarely produces exact
float32 gain ties. So this is a correctness-completeness item (small/degenerate
inputs), not a scale blocker.

- **2.1** As a parity author, when I instrument the actual GPU evaluate kernel on the
  mcw10 node, then I recover the exact float32 op order (reduction, reciprocal,
  dequantise) the device uses to form each candidate gain.
- **2.2** As a parity author, when cajeta forms split gains in that exact order, then
  the tiny-fixture argmax matches the reference and `tiny_reg_mcw10` node 14 is
  bit-identical.
- **2.3** As a maintainer, when the near-tie fix lands, then `ConfigParityTest::
  minChildWeightTreeBitIdentical`, `BinaryTreeTest`, and `MultiTreeTest` are
  re-enabled and green — with no regression to the large-fixture parity.
- **2.4** As a parity author, when no CPU op-order reproduces the device pick at a
  node, then that node is documented as a genuine hardware-primitive tie (like
  `MUFU.RCP`) requiring a captured table, not an open bug.

## 3. Device `expf` *(transcendental fidelity)*

Binary `sigmoid` and multiclass `softmax` gradients past round 0 evaluate device
`expf`. cajeta uses the libm intrinsic (`Math.exp`); they differ ~1 ULP, and the
gradient quantiser does not always absorb it, so later-round trees diverge.

Structure is confirmed (`tools/expf/README.md`, 2026-07-24): Cody–Waite reduction
`j = rint(x·log2e)`, `f = x − j·ln2`; `e^f` via a **degree-6 minimax polynomial**
(tuned float32 coefficients, not Taylor); scale by `2^j`. Fit residual ~1 ULP — the
shape is right; the exact float32 coefficients + `ln2` split + FMA order remain.

- **3.1** As a parity author, when I recover the exact float32 minimax coefficients,
  the two-part `ln2` split, and the FMA evaluation order, then a CPU `FastMath.expf`
  reproduces `expf_sweep.npy` bit-for-bit over the domain XGBoost uses (`x ≤ 88.7`).
- **3.2** As a parity author, when `Logistic`/`Softmax` route their `expf` through
  that model, then binary and multiclass gradients are bit-identical to the device
  dump every round.
- **3.3** As a maintainer, when `expf` fidelity lands, then `BinaryTreeTest`
  (rounds 1+), `LogisticObjectiveTest::multiRoundGradHess…`, and the multiclass
  softmax parity tests are re-enabled and green.
- **3.4** As a parity author, if the minimax coefficients cannot be recovered by fit,
  then I capture device `expf` over the reduced domain and table it (the `ex2.approx`
  path), the `__fdividef` fallback pattern.

## 4. Pruned-sketch interior *(float-scan fidelity)*

The GPU device sketch prunes a weighted-quantile summary to the cut points. cajeta's
`SetPrune` port is bit-exact as *integer-summary* arithmetic (validated in
`SketchPruneTest`), and reproduces `cut_ptrs`, both per-feature sentinels, and
monotonicity bit-for-bit against `large_reg`. The **interior** cut values drift ≤~1
summary rank (max abs 0.02) because the device forms the summary `rmin/rmax` via a
float prefix-scan (`ScanInput`) + `FixError` whose accumulation differs from exact
integer ranks.

- **4.1** As a parity author, when I model the device's float scan/`FixError`
  accumulation for the summary ranks, then `Sketch.cuts` interior values match the
  `large_reg` reference bit-for-bit.
- **4.2** As a maintainer, when it lands, then `LargeSketchTest`'s exact-interior
  assertions (currently structure+endpoints only) are enabled and green.
- **4.3** As a parity author, this is the lowest-priority residual — the raw→bins
  pipeline is already bit-exact wherever `#distinct ≤ max_bin`, and cut structure is
  exact everywhere; only >`max_bin`-distinct interior *values* on large data drift.

---

## 5. Acceptance criteria (spec-level)
- The split-selection near-tie is either reproduced (tiny fixtures bit-exact, gated
  tests re-enabled) or each residual node is documented as a captured hardware-tie.
- Device `expf` reproduces `expf_sweep.npy` bit-for-bit over XGBoost's domain, and
  binary/multiclass parity holds every round.
- The pruned-sketch interior matches the reference at scale, or is explicitly parked
  with its ≤1-rank bound recorded.
- No regression: `large_reg`/`large_binary` parity and every existing bit-exact test
  stay green.
- Each fix follows probe→model→validate; captures are regenerable, models are in-repo.

## 6. Open questions (resolve at plan time)
- **Hardware access cadence** — the probes need the NVIDIA box; batch them (one
  session: evaluate-kernel instrumentation + `expf` domain sweep + sketch-scan dump)
  vs. iterate.
- **Near-tie: op-order vs. table** — is the divergence a reproducible op-order
  difference, or does it bottom out in `MUFU.RCP` such that a captured table is the
  only route (as for `__fdividef`)?
- **`expf` coefficient recovery** — fit from the sweep vs. lift the exact constants
  from the CUDA math headers / SASS of the pinned toolchain.
- **Priority/sequence** — `expf` unblocks the most tests (binary/multi rounds 1+);
  the near-tie is tiny-only; the sketch interior is large-only and lowest value.
