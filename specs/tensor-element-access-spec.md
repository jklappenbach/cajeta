# Tensor element access cost (spec)

Status: draft, 2026-10-17. Opened by xgboost-data-prep 1.2.3 on its
measurement. This is a stub to be scoped with Julian.

## 1. Definition

`Tensor<T>.flatGet(i)` reads one element: `this.store.get(this.offsetVal +
i)`. `Storage<T>.get` then checks `onDevice`, which throws if the data is
device-resident, and reads `host[offset + i]`.

xgboost-data-prep's PrepTiming measured it on proton (Strix Halo cpu) with
cajeta v0.43.1, on 2026-10-17, in a binner-shaped loop of 20M reads that
sums float32-narrowed float64 values:
- through `flatGet`: 16.4 ns per read;
- from a plain `float64[]`: 2.3 ns per read.

In cajeta-xgboost's old binner that difference was about 180 ns of the 226
ns each cell took. Any library that loops over a Tensor element by element
pays the same: ml, timeseries, recsys and xgboost all do.

The stdlib pays it too. `cajeta.math.npio.Npy.decodeI32` (and its siblings)
fills its result with one `flatSet` per element. Loading the 32 MB SFU
capture (`SfuMath`'s 2^23-entry table) takes about 160 ms on proton, and it
is most of what is left of cajeta-xgboost's cold first fit with the
captures (xgboost-device-followups 3.2.3, 2026-10-19).

## 2. Use cases

- **2.1** When a host loop reads a contiguous Tensor with `flatGet`, it costs
  within a small factor of a plain array read.
- **2.2** When the data is device-resident, `flatGet` still refuses with
  `PlacementException`.
- **2.3** When `Npy` decodes a numeric `.npy` image, it copies the data
  in bulk, not one element at a time.

## 3. Open questions

- **3.1** Where do the 14 ns go? Candidates:
  - the two calls are not inlined across the generic boundary;
  - the `onDevice` branch;
  - a bounds check per level;
  - a per-call borrow or drop.

  Measure with `--emit=ir` on the probe before designing anything.
- **3.2** Fix the access path, or rely on the bulk view for hot loops, or
  both. The bulk view already exists: `hostBuffer()` borrows the host
  array, indexed from `hostBase()` (since v0.22). cajeta-xgboost's sketch
  and binner read through it from xgboost-data-prep Unit 3 on. What
  remains is whether `flatGet` itself should cost this much.
