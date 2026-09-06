# cpu-barrier-fission-loops — a barrier inside a loop on the CPU backend

Status: **active** — approved 2026-09-06. Plan: [`agents/cpu-barrier-fission-loops-plan.md`](../agents/cpu-barrier-fission-loops-plan.md).

## 1. Definition

The CPU backend runs a `@Kernel` that calls `Barrier.workgroup()` by
loop fission (`src/cajeta/xpu/cpu/CpuBarrierFission.cpp`): the body is
split at each barrier into regions, each region is wrapped in a loop over
the block's work-items, and a block-uniform loop that contains a barrier is
kept as an outer scalar scaffold whose header and latch run once per
iteration while its body regions run per work-item.

Three kernels of the xpu-tile baseline (`reduceSum`, `finalSum2`,
`matmulTiled` in `test/xpu/bench`) are declined by that pass with
"unstructured barrier control flow (a block is reached by more than one
region path)". They are ordinary GPU code: an LDS tree reduce whose stride
loop ends in a barrier, and an LDS-tiled GEMM whose K-loop has two
barriers. The cause is one case in the region walk: when the code after a
loop's **last** barrier is the loop's own latch block, the walk starts a
region at the latch instead of leaving the latch to the scaffold, and the
region grows through the header and around the loop until a block is
reached twice. Loops whose post-barrier code sits in a separate block
before the latch already work, which is why the cooperative-matrix GEMM
tests pass.

This spec scopes that fix and nothing else: the pass keeps its rule of
declining, by name, any shape it cannot run correctly.

**Non-goals.** Barriers under work-item-divergent control flow; loops with
a work-item-dependent trip count around a barrier; splitting a latch that
mixes per-work-item and uniform code (declined, see 2.3); any change to
the GPU backends.

## 2. Use cases

- **2.1** When a block-uniform loop's body ends with a barrier and its
  latch block holds only block-uniform instructions (an induction update,
  a uniform load or compare), the kernel lowers on the CPU backend: the
  latch runs once per iteration as scaffold, the regions before the
  barriers run per work-item, and the result equals the scalar reference.
  Witnesses: the stride-loop tree reduce, the two-array final reduce, the
  LDS-tiled GEMM with two barriers per K-step.
- **2.2** When such a loop contains a per-work-item inner loop with no
  barrier (a strided accumulation over a partial array), that inner loop is
  part of a region and runs per work-item, unchanged from today.
- **2.3** When the latch block after a loop's last barrier contains a
  per-work-item instruction (a store indexed by the work-item id, a value
  derived from it), the pass declines the kernel with a message that names
  the latch and the reason. It never runs that code once per iteration.
- **2.4** When the CPU fission declines a kernel, the build prints the
  `[xpu-kernel-skipped]` note with the fission's reason (already in place
  since the baseline), and the runtime counts the launch as failed.
- **2.5** When the fix lands, the xpu-tile baseline's CPU leg is rerun and
  the pending rows for `dot`, `reduceSum`, `matmulTiled`, `cg` and
  `degenerate` become measured rows in the report, with the same identity
  as the rest of the leg.

## 3. Acceptance

Every use case has a test that asserts it fires and, where a decline is
involved, a test that asserts the accepted shape stays silent. The three
baseline kernels compile on the CPU backend with no skip note and agree
with their scalar references. The report's residual row for the CPU leg is
closed by measured rows.
