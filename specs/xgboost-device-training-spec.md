# cajeta-xgboost device-resident training (spec)

Status: draft, 2026-10-17. Written at Julian's request, relayed by Phoenix
with the 4090 measurements in 1.2. The open questions are in 10.

## 1. Definition

### 1.1 Purpose

cajeta-xgboost trains bit-exactly to XGBoost's GPU `hist` builder on every
backend (cajeta-xgboost spec 1.2, 6). Its device path has been correct since
U12, but slow. Each per-node data pass ran as a kernel that allocated its own
buffers, uploaded its inputs, synchronized and downloaded its result.

This spec moves a fit's state onto the device for the whole fit:
- the bin matrix, labels, margins, gradient pairs and histograms;
- the row partition.

The host drives the tree's shape, and each step costs a small, bounded
transfer.

Bit-exactness does not change. Every result stays equal to the CPU builder
and to the NVIDIA reference, checked by the existing fixtures.

### 1.2 Where the time goes, measured

Phoenix measured these on the RTX 4090 (WSL2) at cajeta-xgboost 6035b9a with
cajeta v0.43.1, on 2026-10-17. All runs use 10 rounds, depth 6, max_bin 256,
eta 0.3 and lambda 1.

Upstream XGBoost 3.1.2, device=cuda, medians of 3 after a warm-up:

| data | end to end (QuantileDMatrix + train) | training alone |
|---|---|---|
| large_reg, 20k x 50 | 177 ms | 78 ms |
| large_binary, 20k x 50 | 112 ms | 70 ms |
| large_multi, 20k x 50, 4 classes | 269 ms | 205 ms |
| synthetic regression, 200k x 50 | 366 ms | 82 ms |
| synthetic 4-class, 200k x 50 | 585 ms | 263 ms |
| synthetic regression, 1M x 50 | 1437 ms | 146 ms |

cajeta `GBDT.fit` on large_reg, device gpu, took 53.4 s per fit. Its
phases, GPU path against CPU path:

| phase | GPU path | CPU path |
|---|---|---|
| Sketch.cuts | 445 ms | 445 ms |
| Binner.bin | 457 ms | 490 ms |
| Booster.train | 52,923 ms | 5,177 ms |

That is about 84 ms per node over roughly 630 nodes. featureScan is not the
cost: about 55 µs.

int64-scan-verbs Unit 7 (cajeta-xgboost 97016e0) took the first step:
- the bin matrix is uploaded once per fit;
- the gradient pairs are uploaded once per tree;
- each node's rows are a segment of a resident row index, partitioned in
  place;
- a histogram reads only its node's rows.

One depth-6 tree, round-0 gradients, median of 9:

| tree | gfx1151 | 4090 |
|---|---|---|
| large_reg | 2419 ms → 53 ms | 5442 ms → 196 ms |
| covertype | 5726 ms → 69 ms | 13212 ms → 198 ms |

On the 4090 the segment tree costs the same at 20k and 50k rows, so what
is left is per-node fixed cost, not row work.

Phoenix's xgbbench run at 0.5.0's code (97016e0) on the 4090:
- `GBDT.fit` on large_reg takes 4.6 s, against 53.5 s before;
- its boost takes 3588 ms, against 52,923 ms, and now beats cajeta's own
  CPU boost (5,177 ms);
- the boost is still about 46x upstream's training alone (78 ms), and the
  whole fit about 26x upstream end to end (177 ms).

### 1.3 What is still per node after Unit 7

Read at 97016e0:

- **Split evaluation.** `GpuSplitFinder.findBest`, per node:
  - allocates seven device buffers;
  - uploads the node's histogram (`2 * nbins` int64) and the cut table;
  - launches featureScan, synchronizes, and downloads three record arrays.
- **Histogram download.** The built histogram (`2 * nbins + 2` int64) comes
  back to the host.
- **Host-side work and syncs.**
  - Sibling subtraction runs over the host store.
  - The store itself is `cap * nbins` int64 per tree, twice.
  - Every kernel is followed by a full stream synchronization: about six per
    split.
- **Gradients.** These are computed on the host from host margins, and so are
  the quantiser's sums. The quantized pairs are uploaded once per tree.
- **Leaf updates.** Each row's leaf is read back once per tree, and the
  margins are updated on the host.

On WSL2 a launch plus synchronization costs about 50 µs (featureScan's floor
in int64-scan-verbs 6.1.2). A depth-6 tree has up to 63 splits, so six
synchronizations per split is already about 19 ms per tree before any work.

### 1.4 Systems

cajeta-xgboost:
- `gpu/GpuTrainState`, `GpuHistogram`, `GpuPartition` and `GpuSplitFinder`;
- `tree/TreeBuilder`'s device path and `booster/Booster.train`;
- `objective/*` for the device gradient forms;
- `tree/GradientQuantiser`.

cajeta:
- `cajeta.xpu` (`KernelBuffer`, `KernelStream`, `Event`, `Workgroup`, `Wave`);
- `cajeta.math.sfu.SfuMath`'s device forms (`fdividefDev`, the EX2
  table).
- for 6.1's on-device block-base scan, the device-wide scan of
  compaction-sort-blocks (draft), or a one-workgroup scan kernel until it
  lands.

Benchmarks:
- the harness at /home/julian/code/ml/xgbbench (bench.py, gen.py, run.sh,
  src/, src2/);
- `selftest/TreeGrowTiming` and `FeatureScanTiming`.

### 1.5 Non-goals

- Changing any result. A faster path that moves one bit is a regression.
- Multi-GPU, external memory, and sampling (subsample, colsample), which
  stay as the cajeta-xgboost spec defers them.
- The sketch and the binner. They take about 0.9 s of a fit on large_reg on
  both paths, and they are separate work (10.5).
- Prediction (`TreeWalker`), which is not on the training path.

## 2. Resident data

### 2.1 Requirements

A fit uploads these once:
- the bin matrix;
- the labels;
- the cut table;
- the base margins.

Margins, gradient pairs and their quantized form stay on the device for the
whole fit. The host keeps the trees and whatever per-node scalars it needs
to decide the tree's shape.

### 2.2 Use cases

- **2.2.1** When a fit runs R rounds of K classes, the bin matrix and labels
  are uploaded once, and nothing of size `n` or `n * nf` crosses the bus
  after that, in either direction. The one exception is the final margins
  when the caller asks for them.
- **2.2.2** When a fit ends, its device buffers are released. A second fit
  in the same process starts from nothing.

## 3. Gradients and the quantiser on the device

### 3.1 Requirements

Each round's gradient pairs are computed on the device from the resident
margins and labels:
- squared error, logistic and softmax;
- in the op order the CPU objectives use, with the EX2 table for `expf`
  as `SfuMath`'s host form uses it.

The quantiser's scale comes from four float64 sums of clipped float32
gradients (`GradientQuantiser.build`). Only the power of two that each sum
rounds into matters (the class doc), but a float sum in a different order
can cross that boundary. The device form must give the same exponent as the
host's ascending-row sum on every input, not just usually (10.1).

### 3.2 Use cases

- **3.2.1** When the device computes a round's gradient pairs, each is
  bit-identical to the host objective's, for every objective, on every
  fixture. The softmax ground truth captured on the 4090 is included.
- **3.2.2** When the device builds the quantiser, its four scale factors are
  bit-identical to `GradientQuantiser.build`'s, including on an input
  constructed so that the clip sum sits within one ulp of a power of two.

## 4. Histograms and sibling subtraction on the device

### 4.1 Requirements

The node histograms live in a device store. A node's histogram is built from
its segment (as in Unit 7) or derived as parent minus sibling by a device
kernel. Both are exact int64, so the store's contents do not depend on the
path taken.

The store holds only the nodes that can still be split or subtracted from.
That is the current level's nodes and their parents, so its size is bounded
by the widest level, not by the tree's capacity (10.2).

### 4.2 Use cases

- **4.2.1** When a child's histogram is derived on the device, it equals the
  CPU builder's `subtractInto` result bit for bit, totals included.
- **4.2.2** When a tree of depth 8 or more trains, the device store's peak
  size is that of its widest level, not `2^(d+1) * nbins`.

## 5. Split evaluation on the device

### 5.1 Requirements

Stage A (featureScan) reads its node's histogram from the device store, with
no upload and no allocation. Stage B, the cross-feature update, runs on the
device as well: strict `>` with the min_child_weight gate, ties to the lowest
feature. It writes one split record per node, which is the only thing that
comes back.

A level's nodes are evaluated by one launch: a workgroup per (node, feature)
pair.

### 5.2 Use cases

- **5.2.1** When a node is evaluated on the device, its split record (valid,
  feature, boundary, gain bits, default direction, child sums) equals
  `SplitFinder.findBest`'s, on every fixture and on the knife-edge mcw10
  fixture.
- **5.2.2** When a level of L nodes is evaluated, it costs one launch, and
  the transfer back is L records.

## 6. Partition and level batching

### 6.1 Requirements

Unit 7's segment partition stays. All the splits of a level are partitioned
by one pair of launches, with the block bases scanned on the device so that
no count crosses to the host per split. The left counts come back once per
level, with the split records, for the host's bookkeeping.

The histograms of a level's built children are likewise one launch over
their segments.

### 6.2 Use cases

- **6.2.1** When a level with S splits is partitioned, every child's segment
  is the CPU builder's rows in ascending order (Unit 7's tests). The level
  costs a fixed number of launches, independent of S.
- **6.2.2** When a tree of depth d trains, the number of stream
  synchronizations is at most a small constant times d, plus a constant per
  tree. It is not proportional to the number of nodes.

## 7. Leaf values and margins on the device

### 7.1 Requirements

Leaf weights are computed as today, from each leaf's sums with the CPU
builder's op order, and they come to the device as a table of at most `cap`
float32 values. A kernel adds each row's leaf value to its margin, reading
the row's leaf from the segments. The host never sees per-row leaves or
margins during a fit.

### 7.2 Use cases

- **7.2.1** When a round ends, the device margins equal the host path's
  margin history for that round, bit for bit, on every fixture
  (`ParityRunner.compareMargins`).
- **7.2.2** When the caller asks for the margin history, it is downloaded
  once at the end of the fit, or once per round only when requested.

## 8. Transfers and synchronization budget

### 8.1 Requirements

Per tree: one synchronization point per level, plus a constant for the root
and the leaf update. Per level, the device sends back:
- the split records;
- the children's counts and totals, for the stopping rules and leaf
  weights.

The host sends down the level's split decisions and, at the end, the leaf
table. Nothing proportional to `n`, `nf` or `nbins` crosses per node.

### 8.2 Use cases

- **8.2.1** When a fit runs, a test-only counter shows:
  - bin-matrix uploads: 1;
  - transfers of n-sized buffers after the start: 0;
  - synchronizations per tree: at most `c * depth + c0`.

## 9. Measurement and acceptance

### 9.1 Requirements

Every fixture's trees and margins remain bit-identical on cpu, amdgpu
(gfx1151) and nvptx (sm_89). The existing parity suites and
`DeviceParityTest` gate this.

Speed is measured with the xgbbench harness against upstream XGBoost 3.1.2
on the same 4090, end to end and training alone. The training-alone column
is the target (10.3).

### 9.2 Use cases

- **9.2.1** When `GBDT.fit` trains large_reg on the 4090 at the 1.2
  parameters, its Booster.train time is within the factor of upstream's
  training-alone time that 10.3 sets.
- **9.2.2** When the row count goes from 200k to 1M at fixed shape, the
  training time grows sublinearly, as upstream's does (82 ms to 146 ms).
- **9.2.3** When the same fit runs on gfx1151 (HIP), it gives the same
  model, and its time is recorded beside the 4090's.

## 10. Open questions

- **10.1 The quantiser's sums on the device.** These options would make the
  exponent exact and order-free:
  - accumulate the clipped float32 values in a wide fixed point (exact, but
    about 280 bits for float32's range);
  - compute the sums with the CPU's order, as a scan in row order (exact,
    but serial over n);
  - download the float32 gradients once per tree and sum on the host (n * 8
    bytes per tree, which is what 2.2.1 forbids);
  - prove that a pairwise device sum reaches the same exponent except
    within an ulp of a power of two, and fall back to the host sum only
    then.

  Recommended: the wide fixed point. Only the exponent matters, so a
  per-workgroup exact partial in two int64 words of a scaled mantissa,
  combined exactly, is enough. To confirm before the plan.
- **10.2 Store policy.**
  - Recommended: keep only the current level and its parents (4.1).
  - The alternative is the full `cap * nbins` store, which is simpler, is
    26 MB at depth 6 on large_reg, and is 420 MB at depth 10.
- **10.3 The speed target.** Upstream trains large_reg in 78 ms. It has
  hand-tuned CUDA and no bit-exactness tax beyond its own quantiser.
  Proposed: within 3x of upstream's training-alone time on the 4090 for
  large_reg, large_binary and the 200k synthetic set, and flat scaling to
  1M (9.2.2). Or name another factor.
- **10.4 Depthwise only?** Upstream batches by level for depthwise growth.
  `maxLeaves` (lossguide-like) growth needs a priority order, not levels.
  - Recommended: batch by level when `maxLeaves` is 0, and keep the per-node
    order otherwise, with the same resident data.
- **10.5 The sketch and the binner.** They take 0.9 s of the 1.3 s that
  would remain on large_reg.
  - Recommended: a separate spec once training meets 10.3, since upstream's
    QuantileDMatrix costs 100 ms there and that difference will then
    dominate end to end.
