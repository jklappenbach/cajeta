# Catalog: every cajeta-xgboost kernel

Generated from the sources of `dev.cajeta.xgboost` 0.7.0 on 2026-10-24: every `@Kernel` in the `gpu` package with its source location, the training stage it serves, whether it declares a 32-lane wave, and its role from the kernel's own documentation. The stages, decompositions and the one measured adaptation are described in [Gradient boosting on the device](xgboost.md).

33 kernels. Every one is integer-exact across backends except the gradient and leaf kernels, whose float32 results are per row and need no reduction.

| kernel | stage | source | `@Wave(32)` | role |
|---|---|---|---|---|
| `binCells` | Binning | `gpu/GpuBinner.cajeta:26` |  | One thread per (row, feature) cell binary-searches the quantile cuts on integer order keys, so the bin does not depend on the device's float compare. |
| `histTiles` | Histograms | `gpu/GpuHistogram.cajeta:168` |  | Tile x of rows rows over feature group y, summed in workgroup memory and merged once per nonzero word; a level tile's segment comes from plan, else it is sBegin, sCount at sBase. |
| `levelTiles` | Histograms | `gpu/GpuHistogram.cajeta:249` |  | Writes split j over its tiles of tileSplit when its half segment has at least minRows rows; its first tile is in slotTab, its count in tab. |
| `histScatter` | Histograms | `gpu/GpuHistogram.cajeta:422` |  | The legacy whole-dataset histogram: one thread per cell, int64 global atomics, a position filter selecting the node's rows. |
| `histSegment` | Histograms | `gpu/GpuHistogram.cajeta:449` |  | Node histogram over a row segment: one thread per (segment row, feature) cell, the same integer atomics as histScatter, into the slot at word base. |
| `zeroRange` | Histograms | `gpu/GpuHistogram.cajeta:475` |  | Zeroes words [base, base + n) of buf. |
| `subtractSlot` | Histograms | `gpu/GpuHistogram.cajeta:482` |  | Sibling subtraction over one slot: store[out + i] = store[parent + i] - store[built + i]. |
| `copyWords` | Histograms | `gpu/GpuHistogram.cajeta:490` |  | Copies the first n words of src into dst. |
| `zeroWords` | Histograms | `gpu/GpuHistogram.cajeta:497` |  | Zeroes the first n words of buf. |
| `childSlots` | Histograms | `gpu/GpuHistogram.cajeta:509` |  | Per split j of a level: the smaller child is built in the new slot of slotTab (parent base, new base, first cell, first tile) and the larger derived in the parent's; plan gets built base, begin, count and derived base, a |
| `zeroLevel` | Histograms | `gpu/GpuHistogram.cajeta:537` |  | Zeroes the built slot of every split in plan. |
| `histLevel` | Histograms | `gpu/GpuHistogram.cajeta:552` |  | histSegment over every built child of a level: cell i belongs to the last split whose first cell in slotTab is at most i, and cells past the child's rows do nothing. |
| `subtractLevel` | Histograms | `gpu/GpuHistogram.cajeta:587` |  | The derived slot of every split in plan, in place: parent minus built. |
| `addLeaves` | Leaves | `gpu/GpuLeaves.cajeta:16` |  | Position p of ridx belongs to the last leaf of tab (segment begin, float32 value bits; ascending begins) whose begin is at most p; its row's margin of group gains that leaf's value in float32. |
| `squaredError` | Gradients | `gpu/GpuObjective.cajeta:88` |  | One thread per row: gradient and hessian of the squared-error objective at the current margin. |
| `logistic` | Gradients | `gpu/GpuObjective.cajeta:99` |  | One thread per row: logistic gradient and hessian, the exponential through the captured exp2 table and the software fused multiply-add model. |
| `softmax` | Gradients | `gpu/GpuObjective.cajeta:116` |  | One thread per (row, class); each thread re-scans its row's classes for the maximum and the sum. |
| `partitionScatter` | Partition | `gpu/GpuPartition.cajeta:26` |  | The legacy partition: one thread per row, int64 atomic adds for the left and right counts. |
| `segmentCount` | Partition | `gpu/GpuPartition.cajeta:71` | yes | The partition's first pass: each workgroup stores how many of its rows go left. |
| `segmentPlace` | Partition | `gpu/GpuPartition.cajeta:90` | yes | The second pass: a left row goes to its block's base plus the left rows before it in the block, a right row after every left row in segment order. |
| `levelBlocks` | Partition | `gpu/GpuPartition.cajeta:113` |  | Workgroup j marks each block of split j's segment with j in blockSplit. |
| `levelCount` | Partition | `gpu/GpuPartition.cajeta:127` | yes | segmentCount over every split of a level: block w counts the left rows of its split's block. |
| `levelBases` | Partition | `gpu/GpuPartition.cajeta:142` | yes | Workgroup j turns split j's block counts into block bases in place and stores its left count. |
| `levelPlace` | Partition | `gpu/GpuPartition.cajeta:165` | yes | segmentPlace over every split of a level, into scratch. |
| `levelCopy` | Partition | `gpu/GpuPartition.cajeta:188` |  | Copies every split's segment of a level from scratch back into ridx. |
| `copySegment` | Partition | `gpu/GpuPartition.cajeta:223` |  | Copies the segment [begin, begin + count) of src into dst. |
| `identity` | Partition | `gpu/GpuPartition.cajeta:231` |  | ridx[r] = r for every row: the root's segment. |
| `clipWords` | Gradient quantisation | `gpu/GpuQuantiser.cajeta:56` |  | Word w of sum s (0 grad+, 1 grad-, 2 hess+, 3 hess-) over rows r*stride + offset: each value's 24-bit mantissa lands at bit exponent - 1. |
| `serialSums` | Gradient quantisation | `gpu/GpuQuantiser.cajeta:103` |  | The host's four ascending float64 clip sums, by one thread, into out[4]. |
| `quantize` | Gradient quantisation | `gpu/GpuQuantiser.cajeta:131` |  | fixedGrad and fixedHess of each row into q (row-major pairs). |
| `featureScan` | Split finding | `gpu/GpuSplitFinder.cajeta:95` | yes | Stage A, one 32-lane wave per feature and a lane per boundary of a 32-boundary tile. |
| `levelScan` | Split finding | `gpu/GpuSplitFinder.cajeta:214` | yes | #featureScan for a level: one 32-lane wave per (node, feature), reading node j's slot of the device store at word slotBase[j] (interleaved G and H per bin, then the totals). |
| `levelPick` | Split finding | `gpu/GpuSplitFinder.cajeta:340` |  | Stage B for a level, a thread per node: SplitFinder.findBest's cross-feature update over the node's records (strict > after the min_child_weight gate, so ties keep the lowest feature). |
