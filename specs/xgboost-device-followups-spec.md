# cajeta-xgboost device follow-ups (spec)

Status: draft. Opened 2026-10-19 from xgboost-device-training Unit 12, the
findings its 4090 runs turned up that were outside that plan's scope. That
plan released dev.cajeta.xgboost 0.6.0 and closes once these move here.

## 1. Definition

### 1.1 Purpose

0.6.0's device fit matches or beats upstream XGBoost 3.1.2's training time on
the 4090 at every benchmark size, once the process is warm and the SFU
capture tables are present. Four costs remain outside that steady state:

- a fit without the capture tables, which is what an installed library gets;
- the first fit in a process;
- the sketch and the binning at 1M rows, most of an end-to-end fit;
- the tile tuner's choice under WSL2.

This spec sets requirements for each.

### 1.2 Measured

On the 4090 (Phoenix), cajeta-xgboost 4f7468c, cajeta v0.43.1:

- **Without the captures.** With RCP_TABLE and EX2_TABLE unset, `GBDT.fit`
  defers split finding to the CPU, node by node, and a 20k fit takes about
  3.5 s. With them, the boost is 25 ms on large_reg.
- **The first fit.** xgbbench's one-shot phases driver, boost of a cold
  process against a warm `FitTiming` fit, WSL2: large_reg 954 / 25 ms,
  large_binary 1805 / 43, large_multi 1951 / 164. Earlier runs measured
  1.1 to 2.8 s for a cold 20k fit. Unit 13 already removed the ~0.3 s of
  clock calibration the tuner caused.
- **Data prep at 1M**, WSL2: sketch 1191 ms and bin 2885 ms. The boost is
  145 ms. Cajeta end to end is 1369 to 1387 ms per run in xgbbench's
  repeated driver, and upstream end to end is 1294 to 1370 ms with
  training at 129 to 137 ms. At 200k: sketch 246 ms, bin 591 ms.
- **The tuner under WSL2**, tuned against the global build: large_multi
  164 / 129 ms and 200k 74 / 66 ms. Native Windows and gfx1151 show no
  loss.

### 1.3 Systems

- cajeta-xgboost: `api/GBDT`, `tree/TreeBuilder`, `gpu/GpuTrainState`,
  `gpu/GpuHistogram`, `gpu/GpuSplitFinder`, `sketch/` and `gpu/GpuBinner`.
- cajeta: `cajeta.math.sfu.SfuMath`, `cajeta.xpu` (kernel compilation and
  `Autotune`).
- xgbbench on the 4090 (Phoenix, WSL2 and native Windows) and gfx1151 on
  proton.

### 1.4 Non-goals

- Changing the bit-exact contract. With the captures, every tree and margin
  stays bit-identical to the NVIDIA reference.
- Further boost speed at warm steady state. 0.6.0 meets its target there.
- Multi-GPU, external memory, or sampling.

## 2. Fits without the capture tables

### 2.1 Requirements

Without the captures, `SfuMath` falls back to correctly rounded math, and
results are deterministic but not NVIDIA's bits (docs/Determinism.md). The
device fit must not be much slower in that mode than with the captures.

### 2.2 Use cases

- **2.2.1** When a device fit runs without RCP_TABLE and EX2_TABLE, it grows
  each tree a level at a time on the device, as it does with them.
- **2.2.2** When the same no-capture fit runs on the CPU and on the device,
  the two models are bit-identical.
- **2.2.3** When a no-capture device fit trains large_reg on the 4090, its
  boost is within 2x of the same fit with the captures.
- **2.2.4** When a fit runs without the captures, the model's metadata
  records that it used correctly rounded math rather than NVIDIA's.

## 3. The first fit in a process

### 3.1 Requirements

A program that trains once pays the cold cost every time. That cost has to
be measured by phase before it is cut: kernel compilation, the tile tuner,
the state's first allocations and uploads.

### 3.2 Use cases

- **3.2.1** When a cold process fits large_reg on the 4090, a log or timing
  hook reports the first fit's time by phase.
- **3.2.2** When a process fits a shape it tuned in an earlier process, the
  tile and the shared threshold come from a persisted tuning, not a new
  one.
- **3.2.3** When a cold process fits large_reg on the 4090, the boost is
  within the target 7.2 sets.

## 4. Data prep at 1M rows

### 4.1 Requirements

At 1M rows the sketch and the binning take about 28 times the boost. Every
cut and bin stays bit-identical to `QuantileDMatrix(numpy)`.

### 4.2 Use cases

- **4.2.1** When a device fit prepares 1M rows, the time it spends in the
  sketch and in the binning is reported separately, along with which binner
  ran (host or device).
- **4.2.2** When `GBDT.fit` trains reg_1000000 on the 4090, sketch plus bin
  is within the target 7.3 sets.
- **4.2.3** When the row count goes from 200k to 1M at a fixed shape, sketch
  plus bin grows no faster than the rows.

## 5. The tile tuner under WSL2

### 5.1 Requirements

The tuner picks the shared or global build and the tile from timings of the
root's build. Under WSL2 it picked the slower build on two sets. The
choice has to hold on every platform the library is measured on.

### 5.2 Use cases

- **5.2.1** When `FitTiming` runs on the 4090 under WSL2, under native
  Windows and on gfx1151, the tuned fit is within 5% of the better of the
  shared and the global builds on every set.
- **5.2.2** When the tuner times a candidate, each sample covers enough work
  that a sync's latency does not decide the choice.

## 6. Measurement and acceptance

### 6.1 Requirements

Every suite stays green on cpu, gfx1151 and the 4090. Times are taken on an
idle box, with the sha verified, through xgbbench and `FitTiming`, against
upstream 3.1.2 on the same 4090.

### 6.2 Use cases

- **6.2.1** When the work is complete, dev.cajeta.xgboost is released with
  each target in 7 met or its miss recorded.

## 7. Open questions

- **7.1 How a fit without the captures reaches the device.**
  - (a) Device split finding in correctly rounded math, the same fallback
    `SfuMath` uses on the CPU, so the level path runs without the tables.
  - (b) Publish the two 32 MB tables as an Olla artifact the library can
    fetch, so an installed library gets NVIDIA's bits.
  - Recommended: (a) here. (b) changes what an install downloads and is a
    separate decision.
- **7.2 The cold-fit target.** Proposed: a cold large_reg boost on the 4090
  under 0.5 s, with the tuning persisted to a per-user cache by default. If
  kernel compilation dominates, its cache belongs to cajeta.xpu and gets
  its own cajeta spec.
- **7.3 The 1M data-prep target.** Upstream's whole fit at 1M is about
  1.3 s, so its QuantileDMatrix build is about 1.2 s. Proposed: sketch plus
  bin within 2x of that on the 4090 under WSL2, about 2.4 s against 4.1 s
  today.
- **7.4 The tuner's tolerance.** Proposed: 5% (5.2.1). FitTiming's medians
  vary by about that much between runs.
- **7.5 Order and release.** Proposed: 2 first, since it is what a user who
  installs the library gets; then 5, 3 and 4. One release at the end, as
  0.7.0.
