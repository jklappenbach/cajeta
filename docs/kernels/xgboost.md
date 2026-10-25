# Gradient boosting on the device: the cajeta-xgboost kernels

How `dev.cajeta.xgboost` trains gradient-boosted trees on an accelerator,
kernel by kernel: what each one computes for the algorithm, how the work is
cut across lanes, waves and workgroups, what it adapts to the device it lands
on, and where it measures instead of assuming. The library is written in
Cajeta against the portable `cajeta.xpu` surface described in
[One kernel, any device](README.md); nothing here is vendor code.

The design has one governing requirement that shapes every kernel:
**the same bits on every device**. A model trained on an RTX 4090, a Radeon
8060S, a Vulkan part or the CPU backend must be identical, not merely close,
and it must reproduce upstream XGBoost's `gpu_hist` to the bit
([Determinism](https://github.com/jklappenbach/cajeta-xgboost/blob/main/docs/Determinism.md)
in the project). That requirement decides what the kernels may do with floating
point, which reductions they may use, and which adaptations are available to
them.

## 1. The algorithm, and where the device enters

A boosting round fits one tree to the current gradients.

1. **Gradients.** For every row, the objective's first and second derivatives
   at the current margin.
2. **Quantisation.** Gradients become integer fixed point so that histograms
   can be summed exactly.
3. **Binning.** Each feature value is replaced by the index of the quantile
   cut it falls in (done once per fit, not per round).
4. **Histograms.** For every tree node being grown, per feature and bin, the
   sums of gradient and hessian over the node's rows.
5. **Split finding.** For every node and feature, the bin boundary with the
   best gain; then the best feature per node.
6. **Partition.** Rows of a split node are moved to its two children, stably.
7. **Leaves.** Leaf values are added to the rows' margins.

The quantile sketch that produces the cuts runs on the host, fanned across
fibers; everything from binning onward has a device form. The device path is
chosen explicitly by the caller (`Params.device = "gpu"`), never by a
heuristic, and the CPU builder is the oracle every device kernel is held to.

## 2. The kernels

Thirty-three kernels, all in the `gpu` package. Grouped by stage:

### 2.1 Gradients (`GpuObjective`)

| kernel | decomposition | notes |
|---|---|---|
| `squaredError` | one thread per row | |
| `logistic` | one thread per row | `expf` through the device exp2 model |
| `softmax` | one thread per (row, class); each re-scans its row for the max and the sum | |

The transcendental is the interesting part. Upstream's GPU gradients go
through CUDA's special function unit (`MUFU.EX2`, `MUFU.RCP`), whose results
differ from correctly rounded math in the last bit. To be bit-exact against
them on any device, the library does not call the device's own `exp`: it
indexes a 2^23-entry table of the captured unit's answers, uploaded once per
process, and a software fused multiply-add model around it. On a machine
without the capture the table is filled on the host with correctly rounded
values, in parallel across fibers, and the model records `sfuCaptured = false`.
That is adaptation by **declining to depend on the device**: the kernel's
answer is a function of the table, not of the silicon.

### 2.2 Quantisation (`GpuQuantiser`)

| kernel | decomposition | notes |
|---|---|---|
| `clipWords` | 40 slots × up to 16 blocks of 256 threads, each thread striding over rows, finishing with `int64` atomic adds | four 320-bit fixed-point sums (positive and negative gradient and hessian), exact |
| `serialSums` | one thread | the fallback that replays the host's ascending `float64` sum |
| `quantize` | one thread per row | writes the `(g, h)` fixed-point pairs |

The exact multiword sums let the host fix the quantisation scale without a
device-to-host round trip per round. When the sums cannot settle the scale
within the rounding bound, the one-thread `serialSums` replays the host's
order and the fit's log counts the fallback.

### 2.3 Binning (`GpuBinner`)

`binCells` runs one thread per (row, feature) cell and binary-searches the
cuts on **integer order keys**, so the bin a value lands in does not depend on
the device's float compare. It launches in 256-thread blocks.

### 2.4 Histograms (`GpuHistogram`, twelve kernels)

This is where the device time goes, and the one place the library measures
to decide.

| kernel | decomposition | notes |
|---|---|---|
| `histTiles` | grid `[tiles, featureGroups]`; a 256-thread workgroup covers one tile of rows for one feature group; `Shared<int64>` of 4098 words (2048 bins × G and H, two totals) | shared-memory atomics, one barrier, then only the non-zero words flush to global with `int64` atomics |
| `histLevel` | one thread per (row, feature) cell, 64-thread blocks | builds every child of a level; each cell binary-searches which split it belongs to |
| `histSegment` | one thread per cell | one node's row segment |
| `histScatter` | one thread per cell, with a position filter | the legacy whole-dataset build |
| `childSlots` | one thread per split | picks the smaller child to build; the larger is derived |
| `levelTiles` | one workgroup per split | maps tiles to splits |
| `zeroLevel`, `zeroRange`, `zeroWords`, `copyWords` | one thread per word | bookkeeping |
| `subtractLevel`, `subtractSlot` | one thread per word | the sibling trick: parent minus the built child |

**Why the sums are bit-identical in any atomic order.** Gradients are
integers by the time they reach a histogram, and integer addition is
associative and commutative. The order in which lanes, waves and workgroups
land their atomics varies between backends, wave sizes and runs, and cannot
change the result. That is upstream's own argument for its GPU histogram,
reproduced; every sum is one native `int64` atomic add (Vulkan's
`Int64Atomics`, native 64-bit atomics on CUDA, HIP and the CPU backend).

A level is built in five or six launches with no synchronisation between
them: `childSlots`, `levelTiles`, `zeroLevel`, the two histogram kernels, the
subtraction. Splits whose smaller half has at least `minShared` rows take the
shared-memory tile kernel; the rest take the global-atomic kernel. Those two
thresholds are the measured part (§3).

Feature groups are packed so that no group exceeds 2048 bins, the shared
tile's capacity. A single feature wider than that disables the shared path
for the fit and every split takes global atomics. Nothing is sized from the
device's shared-memory limit: the tile is a fixed 32 KiB, which every backend
the library ships for admits.

### 2.5 Split finding (`GpuSplitFinder`)

| kernel | decomposition | notes |
|---|---|---|
| `featureScan` | one 32-lane workgroup per feature of one node; each lane owns one bin boundary of a 32-boundary tile | per-node growth |
| `levelScan` | one 32-lane workgroup per (node, feature) | level growth |
| `levelPick` | one thread per node, serial over features, 64-thread blocks | the cross-feature pick for a level |

The scans are the library's wave kernels. `Wave.prefixSumI64` gives the exact
left-side sums at every boundary; `Wave.reduceSumI64` the missing-value sums
and the carry from tile to tile; `Wave.reduceMaxF32` with `Wave.ballotSync`
and the trailing-zero count picks the first lane holding a tile's maximum, so
ties resolve the same way everywhere; `Wave.shuffleSync` broadcasts it. The
gain itself is computed by a `@Device` helper through the reciprocal table
(the `__fdividef` capture), for the same reason as the exponential above.

Both scans declare `@Wave(width = 32)`: the lane-per-boundary indexing assumes
exactly 32 lanes. The declaration is what makes that assumption portable. The
compiler versions the kernel for a 32-lane wave on every backend, including the
CPU backend, where the work-item loop is vectorised at the declared width
rather than the host's. See [One kernel, any device](README.md), the section
on wave width.

### 2.6 Partition (`GpuPartition`, ten kernels)

| kernel | decomposition | notes |
|---|---|---|
| `segmentCount` | 256-row blocks, `Workgroup.reduce(GroupOp.Add)` | left-row counts per block |
| `segmentPlace` | 256-row blocks, `Workgroup.scan(GroupOp.Add)` | stable placement |
| `levelBlocks`, `levelCount`, `levelBases`, `levelPlace`, `levelCopy` | the level form of the same, five launches and no sync | `levelBases` is one workgroup per split, looping a scan plus a reduce with a carry |
| `partitionScatter` | one thread per row, `int64` atomic counts | legacy |
| `copySegment`, `identity` | one thread per element | utilities |

A stable partition is a sort with two keys, and the count, scan, place
sequence is how the library does it without a radix sort: the row order
inside each child is the parent's order, on every device, so the next level's
histograms see the same rows in the same places. The per-segment form
downloads the block counts to the host for the prefix sum; the level form
keeps everything on the device.

### 2.7 Leaves (`GpuLeaves`)

`addLeaves` runs one thread per row position, binary-searches the leaf
segment the row sits in, and adds the leaf value to the row's margin in
`float32`. Rows are unique, so no atomic is needed. Inference has no kernel:
prediction walks the trees on the host.

## 3. What adapts, and what is measured

The library's adaptations are deliberately few, because the determinism
requirement forbids most of the usual ones (a reduction order that depends on
the wave width would change bits).

**Measured once per shape and device: the histogram tile.** On the first fit
of a given shape (feature groups and row count) the root node's histogram is
built with tile sizes 256, 512, 1024, 2048 and 4096 rows and with the
global-atomic build, three interleaved rounds each, and the median wins. The
tuner then quarters the segment down to 256 rows to find the smallest segment
at which tiles still beat global atomics; that becomes `minShared`. Both
answers are remembered through `cajeta.xpu.Autotune` under a key that names
the shape and the library build:

```
xgb.hist.tile.g<groups>.n<log2 rows>        the winning tile rows
xgb.hist.tile.g<groups>.n<log2 rows>.min    the smallest segment that tiles
```

The store is per device (the `Autotune` device key is the backend plus the
geometry the answer depends on) and per build (a value tuned against another
library version is discarded, not trusted). It lives under `XGB_TUNE_DIR`, or
the platform cache directory, and `useTuneDir("")` keeps it in memory for one
process. The tuner's own cost is reported separately in the fit's phases, and
a run on a box where the store is not writable tunes once per process rather
than once per shape.

**Timed on the host clock, around a stream sync.** The tuner first measures
the launch-plus-sync round trip of a one-row build, the median of nine, and
sizes each sample so that it covers sixteen such round trips: a sample is
never a single launch, and never fewer launches than it takes to rise above
the sync cost.

**Per-split choice at run time, without measuring.** Every level, each split's
smaller half is compared against `minShared`: at or above it the shared-tile
kernel builds it, below it the global-atomic kernel does.

**Fallbacks that keep the bits.** The shared tile falls back to global atomics
when a feature is wider than the tile. The exact quantiser falls back to the
one-thread serial replay when the scale is ambiguous. The special-function
tables fall back to correctly rounded values when the capture is absent. Each
fallback is recorded in the fit's log or on the model.

**What it does not do.** It never reads the device's shared-memory limit,
register file or wave width to size a kernel; the shapes are fixed at values
every shipped backend admits (32 KiB tiles, blocks of 256, 64 and 32). It
never chooses between GPU and CPU by timing: `Params.device` is the caller's
decision. It never uses floating-point atomics or cooperative-matrix verbs.

## 4. Where the kernels run

The published archive carries device code for Vulkan (SPIR-V) and the CPU
backend; a build with CUDA's assembler present adds NVPTX, and one with ROCm
adds AMDGPU. At launch the runtime takes the first available of CUDA, HIP,
Vulkan and CPU, or the backend `CAJETA_XPU_BACKEND` names, and the library
does not ask which it got: the kernels are the same source, the results the
same bits, and only the tile tuner's answer differs by device.

## 5. Measured against upstream

On an RTX 4090 under WSL2, with upstream XGBoost 3.1.2 on CUDA as the
reference, synthetic data, 10 rounds, depth 6, 256 bins:

| fit, 1M rows × 50 features | cajeta-xgboost 0.7.0 | upstream |
|---|---|---|
| end to end | 1418 ms | 1413 ms |
| training only (tuned tile) | 111 ms | 140 ms |
| training only (global atomics) | 154 ms | |

The tuned tile against the global build is the measured value of the one
adaptation the library makes: 111 against 154 ms over 21 interleaved pairs.
On Windows the same fit at 1M rows trains in 150 ms where upstream has no
functional GPU path. Earlier in the project's history the device path was
300 times slower than upstream, through per-node buffer allocation and
whole-matrix uploads; device-resident level-at-a-time training removed that,
and the kernels described above are the ones that remained.
