# int64 and scan building blocks (spec)

Status: draft, written 2026-10-09 at Julian's request. Julian answered the
open questions 2026-10-10 (7). 6.3 and 6.4, the row partitioner, were added
that day and await his review.

## 1. Definition

### 1.1 Purpose

The cross-lane building blocks (`CajetaXPU.md` §5.5.2) were sized for
cajeta-llm, whose reductions are `float32` and `int32`. `Wave` shuffles,
`Wave.prefixSum` and the integer `Wave.reduce*` forms take `uint32`.
`Group.reduce` and `Workgroup.reduce` take `float32` or `int32`. Nothing
scans across a workgroup.

cajeta-xgboost keeps every gradient sum as an `int64` fixed-point value, so
that its histograms and split search are exact integer arithmetic and
identical on every backend. Its split search is a prefix scan over a
feature's histogram bins, and XGBoost's own GPU code does it as a warp scan
over `int64` gradient pairs. In cajeta today that scan has to be spelled as
two `uint32` shuffles per step with a carry between the halves, in every
kernel that needs it.

This spec adds the `int64` forms of the wave verbs and of `Workgroup.reduce`,
and a `Workgroup.scan` over the integer types.

### 1.2 Who needs it, read 2026-10-09

cajeta-xgboost 933d334 has three kernels:

- `GpuSplitFinder.featureScan` gives one thread to each feature. That thread
  walks the feature's bins in tiles of 32, carrying an `int64` prefix sum of
  G and H and keeping the best gain. The tile is XGBoost's warp. With tens of
  features the launch is one or two blocks of mostly idle lanes, each running
  up to 256 bins of gain arithmetic in sequence. One wave per feature, a lane
  per bin, needs an `int64` wave scan and an `int64` wave reduce.
- `GpuPartition.partitionScatter` does two global `atomicAdd`s on one
  2-element counter for every routed row. A per-block `Workgroup.reduce` over
  `int32` cuts that to one atomic per block, and needs nothing new.
- `GpuHistogram.histScatter` scatters into many bins, so it is not a reduce.
  It covers every (row, feature) cell at every node and filters on the
  node's position. XGBoost keeps each node's rows contiguous (its row
  partitioner, a stable partition by scan), so a node's histogram reads only
  its rows. That partition needs a workgroup scan.

cajeta-llm has no `int64` reduction and no workgroup scan today.

### 1.3 Constraints

- Every backend lowers the new forms: nvptx, amdgpu, vulkan and cpu, and the
  reference interpreter models them.
- The results are exact. Integer addition wraps and is associative, and the
  maximum and minimum are order-free, so a result is identical on every
  backend at every wave width. A kernel needs no `@Wave` pin to agree.
- A kernel's source names no wave width and no wave count to use them.
- They are no slower than the hand-written forms they replace.

### 1.4 Non-goals

- A `float32` or `float64` scan. Its order would have to be defined, and on
  Vulkan the driver's own scan order is unmeasured. No adopter needs one yet.
- An arg-reduce (a maximum together with its lane). It is composed from
  `Wave.reduceMaxF32` and `Wave.ballotSync` where needed.
- `int64` atomics on shared memory, which a shared-memory histogram needs.
  A later spec, since Vulkan gates them on a device feature.
- A segmented integer reduce.
- `uint64`. The fixed-point values are signed.

## 2. The `int64` wave verbs

### 2.1 Requirements

`Wave` gains `int64` forms of its shuffles (`shuffleSync`, `shuffleUpSync`,
`shuffleDownSync`, `shuffleXorSync`), of its reduce (sum, signed maximum,
signed minimum), and of `prefixSum`, which stays EXCLUSIVE as the `uint32`
form is: lane 0 receives 0. Each acts on the active lanes of one wave as its
`uint32` sibling does.

### 2.2 Use cases

- **2.2.1** When every lane calls the `int64` sum, every lane receives the
  wrapped sum of all the lanes' values, carries across the 32-bit halves
  included.
- **2.2.2** When values are negative or above 2^32, the signed maximum and
  minimum and the exclusive prefix sum are exact.
- **2.2.3** When a lane shuffles an `int64`, the receiving lane gets both
  halves of the source lane's value.
- **2.2.4** When the same kernel runs on two backends or at two wave widths,
  each lane's result is the same.

## 3. `Workgroup.reduce` over `int64`

### 3.1 Requirements

`Workgroup.reduce(GroupOp op, int64 value)` joins the `float32` and `int32`
forms, for `Add`, `Max` and `Min`, with the same expansion, scratch, barrier
and refusals (workgroup-reduce spec 2, 4). The pinned launch block folds its
wave count as it does for the other forms (workgroup-reduce 6.8).

### 3.2 Use cases

- **3.2.1** When every lane of a workgroup calls it, every lane receives the
  exact result over the whole workgroup.
- **3.2.2** When the workgroup holds one wave, the result equals the `int64`
  wave reduce.

## 4. `Workgroup.scan`

### 4.1 Requirements

`Workgroup.scan(GroupOp op, T value)` returns to each lane the EXCLUSIVE
combination of `value` over every lane before it in the workgroup's linear
order (`x + y·dimX + z·dimX·dimY`). Lane 0 receives the identity: 0 for
`Add`, the type's minimum for `Max`, its maximum for `Min`. `T` is `int32` or
`int64`. It is a barrier, like `Workgroup.reduce`: each wave scans its lanes,
the wave's last lane stores the wave total in compiler-owned scratch, one
workgroup barrier, then each lane combines the totals of the waves before
its own and adds its in-wave prefix. A call site inside a loop adds a
trailing barrier.

### 4.2 Use cases

- **4.2.1** When every lane calls `Workgroup.scan(GroupOp.Add, x)`, lane `i`
  receives the sum of `x` over lanes `0..i-1`.
- **4.2.2** When each lane passes 1 for a row it keeps and 0 otherwise, the
  results are the kept rows' output slots, in lane order, with no gaps.
- **4.2.3** When the workgroup holds one wave, the result equals the wave's
  exclusive prefix.
- **4.2.4** When a kernel calls it twice in a row, or in a loop, the second
  call reads no total left by the first.
- **4.2.5** When every launch site passes one constant block, the wave count
  is a compile-time constant and no workgroup size is read at run time.

## 5. Refusals

### 5.1 Requirements

The new forms are refused as `Workgroup.reduce` is (workgroup-reduce spec 4):
under control flow that diverges between lanes, with an operator that is not
a `GroupOp` literal, and with a value type they do not take. A host call acts
as a one-lane group.

### 5.2 Use cases

- **5.2.1** When `Workgroup.scan` sits under a branch on a lane coordinate,
  the kernel is refused, naming the verb.
- **5.2.2** When `Workgroup.scan` is called with a `float32` or `uint32`
  value, the kernel is refused, naming the type.

## 6. Adoption in cajeta-xgboost

### 6.1 Requirements

Once a release carries the verbs, `featureScan` runs one 32-lane wave per
feature, and the row partitioner of 6.3 replaces `partitionScatter`. Every
split and every partition stays bit-identical to the CPU builder, which
`GpuSplitFinderTest`, `GpuPartitionTest` and the reference fixtures already
check, and each rewritten kernel is no slower at the fixtures' shapes.

### 6.2 Use cases

- **6.2.1** When `featureScan` runs a wave per feature, its per-feature
  records equal the current kernel's on every fixture, ties to the lowest
  bin included.
- **6.2.2** When the partitioner of 6.3 splits a node, the (left, right)
  counts and every row's node are unchanged.
- **6.2.3** When either kernel is timed against its current form at the
  fixtures' shapes, it is no slower.

### 6.3 The row partitioner

In scope by Julian's answer to 7.3. Read 2026-10-10 at cajeta-xgboost
0883745: every row carries its node in `position[]`, and for each node
`GpuHistogram.histScatter` and `GpuPartition.partitionScatter` scan all `n`
rows and keep those with `pos == nid`. Each call uploads the `n × nf` bin
matrix and the quantized gradients from the host again. A tree of depth `d`
therefore reads every row about `2^d` times per level.

XGBoost's GPU builder keeps instead a row-index array in which each node's
rows are one contiguous segment, and splits a segment in place. This spec
adopts that layout:

- The bin matrix and the quantized gradient pairs are uploaded once per
  training run and stay on the device.
- A row-index buffer `ridx` of `n` `int32` holds every node's rows as a
  segment `[begin, begin + count)`. The root is `0 .. n-1`.
- Splitting a node is a stable partition of its segment: the left child's
  rows first, then the right child's, each in ascending row order, which is
  the CPU builder's order. It runs in two passes over the segment. In the
  first, each workgroup counts its left rows with
  `Workgroup.reduce(GroupOp.Add, int32)`. The block counts are then scanned
  exclusively into each block's base, and their sum is the left count. In
  the second, a lane's left slot is its block's base plus
  `Workgroup.scan(GroupOp.Add, leftFlag)`, and a right row's slot follows
  from its index in the segment minus the left rows before it. The second
  pass writes to a scratch buffer, and the segment is copied back.
- The counts come from the first pass, so no atomic is needed.
- A node's histogram covers only its segment: one thread per (row, feature)
  cell of the segment, scattering with the same integer atomics as today.
- The host learns each row's node from the segments when it needs one,
  once per tree, rather than per split.

The workgroup total of 7.2 is not needed here: a block's base comes from the
scan of the block counts, not from its own total.

### 6.4 Use cases for the partitioner

- **6.4.1** When a node is split, each child's segment holds exactly the
  rows the CPU builder assigns it, in ascending row order.
- **6.4.2** When a segment spans several workgroups, the partition is still
  stable, and the left count equals the CPU builder's.
- **6.4.3** When a node's histogram is built from its segment, it equals the
  CPU builder's bit for bit.
- **6.4.4** When a tree is trained, the bin matrix and the gradient pairs are
  uploaded once, not once per node.
- **6.4.5** When a tree is trained at the fixtures' shapes, it is no slower
  than with today's per-node kernels.

## 7. Open questions

- **7.1** Spelling. Recommended: `int64` overloads on the existing `Wave`
  names (`prefixSum`, `reduceSum`, `reduceMax`, `reduceMin`, the shuffles),
  and `Workgroup.scan` beside `Workgroup.reduce`. The alternative is a
  `Group.scan(GroupOp, T)`, but `Group` verbs are an identity on cpu, where
  a group is one lane, so a workgroup scan built on them would be wrong
  there.
  **Answered 2026-10-10 (Julian): overloads on the existing `Wave` names, and
  `Workgroup.scan` beside `Workgroup.reduce`.**

- **7.2** Should `Workgroup.scan` also hand back the workgroup total? A
  compaction needs it to reserve output space. Recommended: not yet. The
  total is one `Workgroup.reduce` of the same value, which costs a second
  barrier, and an adopter can measure whether that matters.
  **Answered 2026-10-10 (Julian): not yet. A lane that needs the total takes
  the last lane's prefix plus its value, and the partitioner of 6.3 does not
  need it.**

- **7.3** Is XGBoost's row partitioner (contiguous rows per node, built with
  `Workgroup.scan`, histograms over a node's rows only) in this spec's
  adoption, or a later cajeta-xgboost spec? Recommended: later. It changes
  the training loop's data layout, not only kernels.
  **Answered 2026-10-10 (Julian): in this spec. 6.3 and 6.4.**

- **7.4** Are the `int64` shuffles in scope? Recommended: yes. The scan and
  reduce are built from them on backends with no native 64-bit form, and
  they cost little once the halves are handled.
  **Answered 2026-10-10 (Julian): yes.**

