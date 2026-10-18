# cajeta-xgboost data preparation: sketch and binning at speed (spec)

Status: active, approved by Julian 2026-10-17, with the open questions (6)
answered interactively the same day. This is the parallel spec that
xgboost-device-training 10.5 called for.

## 1. Definition

### 1.1 Purpose

Before a fit trains, `GBDT.fit` does two things:
- it computes each feature's quantile cuts (`Sketch.cuts`);
- it maps every value to its bin (`Binner.bin`).

Both are bit-exact to XGBoost 3.1.2's `QuantileDMatrix(numpy)` and are
pinned by the sketch and binner fixtures (cajeta-xgboost spec 2, 7.2). Both
run serially on the host.

At 20k rows they take 0.9 s. At 200k rows they take 9.5 s of a 13.8 s fit.
Upstream builds its whole QuantileDMatrix and trains in 358 ms.

This spec makes the two fast without changing a bit of their output.

### 1.2 Measured

Phoenix measured these on the 4090 at dev.cajeta.xgboost 0.5.0, cajeta
v0.43.1, on 2026-10-17. Each cell is sketch / bin, in ms.

| data | WSL2 | native Windows |
|---|---|---|
| large_reg, 20k x 50 | 438 / 476 | 401 / 830 |
| large_binary | 438 / 548 | 413 / 817 |
| large_multi | 444 / 546 | 385 / 790 |
| reg 200k x 50 | 3445 / 6103 | 3593 / 7851 |
| 4-class 200k x 50 | 3458 / 5475 | 3699 / 8016 |

Upstream end to end, QuantileDMatrix plus training: 111 ms on large_reg and
358 ms on the 200k regression set.

### 1.3 What the code does, read at 0.5.0

- **`Sketch.cuts`** reproduces the CPU `HostSketchContainer`: XGBoost's GK
  `WQuantileSketch<float, float>`, per column, values streamed in row order.
  It uses:
  - a `2 * limit_size` queue folding equal values;
  - a heapsort on overflow;
  - a binary-counter level tree;
  - two final prunes.

  Its order of operations is the reference's, so each column is
  inherently sequential. The columns are independent of one another, and
  the code runs them one after another.
- **`Binner.bin`** binary-searches each (row, feature) cell's float32 value
  in its feature's cuts (`upper_bound`, clamped). Every probe reads the cut
  through `Tensor<float64>.flatGet` and narrows it. At about 450 ns per cell
  for 1M cells, the search is far slower than its roughly eight comparisons
  should be.
- **The bin matrix** is built on the host, then uploaded to the device by
  `GpuTrainState` at the start of a device fit.

### 1.4 Systems

cajeta-xgboost:
- `sketch/Sketch` and `sketch/Binner`;
- `api/GBDT` (`fitMode`'s preparation);
- `gpu/GpuTrainState`, which accepts device-resident bins;
- the sketch and binner tests (`SketchTest`, `SketchPruneTest`,
  `BinnerTest`, `LargeSketchTest`).

cajeta:
- `cajeta.nucleo` or the stdlib's thread pool, for per-column parallelism
  on the host;
- `cajeta.xpu`, for a device binner.

Benchmarks: the xgbbench phase driver (`Phases.cajeta`).

### 1.5 Non-goals

- Changing the sketch algorithm. The reference's CPU GK sketch is the
  contract (gpu-numeric-fidelity U3), and a device-side GPU sketch, as
  upstream's `device=cuda` sketch, would produce different cuts.
- Sparse or external-memory input.

## 2. The sketch across columns

### 2.1 Requirements

Columns are sketched in parallel, each column's operations unchanged and in
row order, so each column's cuts are the same bits as today's. The result is
independent of the thread count and of scheduling.

### 2.2 Use cases

- **2.2.1** When cuts are computed with 1, 4 and all available threads, they
  are identical to each other and to the fixtures' reference cuts on every
  sketch fixture.
- **2.2.2** When the 200k x 50 set is sketched on a 16-thread host, the
  sketch takes at most a stated fraction of its serial time (6.3).

## 3. The sketch's per-column cost

### 3.1 Requirements

Within a column, the cost of each step is reduced without changing its
result:
- reading values without a per-element generic call;
- a sort for the queue's summary that matches the heapsort's output for
  this use (equal values collapse right after, as the code notes);
- no allocation per summary.

### 3.2 Use cases

- **3.2.1** When a column is sketched by the optimized step, its summary at
  every level and its final cuts equal today's, on every fixture and on an
  adversarial column (many ties, a constant column, NaNs throughout, a
  sorted column, a reverse-sorted column).

## 4. Binning

### 4.1 Requirements

Bin indices are computed as today:
- `min(upper_bound(cuts, value), nbins - 1)` in float32;
- -1 for NaN.

Either path below gives the same bin matrix.
- **The host path** reads the cuts from a float32 array, searches without
  generic calls, and runs across rows in parallel.
- **The device path**, for a device fit, uploads the raw float32 values
  once and bins them with one thread per cell. The bins stay on the device
  for training, so the separate bin upload of a device fit disappears.

### 4.2 Use cases

- **4.2.1** When the host and device binners run on every fixture, their bin
  matrices equal `Binner.bin`'s today, cell for cell, NaN included.
- **4.2.2** When a device fit starts from device-binned data, nothing of size
  `n * nf` crosses the bus after the raw values go up once
  (xgboost-device-training 2.2.1).

## 5. Measurement and acceptance

### 5.1 Requirements

Every cut and bin stays bit-identical (the sketch, prune and binner
fixtures, and every parity suite downstream). The speed is measured with the
xgbbench phase driver on the 4090 under WSL2 and native Windows, and on
gfx1151.

### 5.2 Use cases

- **5.2.1** When the 200k sets are prepared, the sketch plus the binning
  meets the target in 6.3.
- **5.2.2** When the 20k fixtures are prepared, the sketch plus the binning
  takes less time than the boost after xgboost-device-training.

## 6. Open questions

All four were decided by Julian on 2026-10-17, each as recommended:
- **6.1:** host threads, one task per column.
- **6.2:** both a fast host binner and a device binner, with resident bins
  for device fits.
- **6.3:** within 3x of upstream's QuantileDMatrix build on the 4090, about
  130 ms on large_reg and 810 ms on the 200k regression set.
- **6.4:** measure `flatGet`'s share first, and open a cajeta item if it
  holds.

The questions as they were put:

- **6.1 Host threads or the device for the sketch?** A device thread per
  column would run the same sequential GK code. It only has as many threads
  as columns (50 here), and the queue sort and level tree are poor work for
  one GPU thread.
  - Recommended: host threads, one task per column.
- **6.2 Device binning?**
  - Recommended: both. The host binner made fast for the CPU path and for
    `GBDT.predict`, and the device binner for device fits, where it also
    removes the bin upload.
- **6.3 The target.** Proposed: on the 4090, the sketch plus the binning
  within 3x of upstream's whole QuantileDMatrix build, measured as its end
  to end minus training alone (about 43 ms on large_reg and 271 ms on the
  200k regression set). Or name another figure.
- **6.4 `Tensor.flatGet`'s cost.**
  - If the binner's 450 ns per cell is mostly the generic call, that is a
    stdlib cost every library pays, and worth measuring and fixing in cajeta
    as well.
  - Recommended: measure it in Unit 1 of this work, and open a cajeta item
    if it holds.
