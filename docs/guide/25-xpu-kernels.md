# 25 — Writing XPU Kernels

How to write a kernel against `cajeta.xpu` so that it runs at the
device's ceiling in the regime it serves — decode, prefill, bind, or a
fused chain — and how to know that it does. Every rule here was
measured, not reasoned; where a number is quoted it was taken on
gfx1151 (Radeon 8060S, wave32, shared LPDDR5X, ~206 GB/s practical
streaming ceiling) and the record that holds it is named. The
substrate itself is in [cajeta.xpu — Accelerator Substrate](../specification/xpu/CajetaXPU.md);
how the layer picks among kernels is in [Kernel Routing](../specification/xpu/CajetaXPU-Routing.md).

## 25.1 What the layer gives you

Ask the device, never a backend name.

| you need | ask |
|---|---|
| lanes in a wave | `Device.waveSize()`; in-kernel `Wave.width()` |
| how many waves fill the device | `Device.simdCount()`, `Device.dispatchBlocks(wavesPerBlock)` |
| LDS you may take | `Device.sharedBytesPerBlock()` |
| L2 size, integrated or discrete | `Device.l2CacheBytes()`, `Device.integrated()` |
| memory you may resident | `Device.memoryBytes()`, `Device.freeMemoryBytes()` — the live figure |
| can I use a feature | `Device.supports(Capability)` |
| is this kernel on this build | `Device.kernelAvailable(name)` |
| a measured per-device setting | `Autotune.recall / remember`, `rememberFor / recallFor`, `reportUnderperforming` |
| what the compiler measured about your kernel | the kernel manifest: `vgpr`, `spillBytes`, `ldsStaticBytes`, `residentGroupsPerCu` |

In a kernel body: `KernelThread.x()` is the lane, `KernelThread.globalIdX()`
the flat item; `Wave.laneId()`, `Wave.shuffleSync`, `Wave.reduceSumF32`
and the segmented reductions; `Shared<T> t = shared T[n]` for LDS;
`Vector<T,N>` with `vload<N>`, `asWords` / `asBytes`, `lut4` (a
16-entry byte table in one `v_perm_b32`) and the integer dot products
`dotSum` (signed × signed) and `dotAccum` (unsigned × signed) — see
[IntegerDotProduct](../specification/gpu/IntegerDotProduct.md). Launch
with `k.launch(stream, grid: [workgroups], block: [threads])(args)`;
`grid` counts **workgroups**, not threads.

Bandwidth is not on the geometry surface yet. Measure it once per
device (a streaming copy at a size well past L2) and keep it in
`Autotune`; every rule below is stated against it.

## 25.2 Decode: one row, one wave, the bus is the ceiling

A decode mat-vec reads every weight byte once and does almost nothing
with it. Its only figure of merit is bytes per second against the
ceiling, and everything that matters is how the wave touches memory.

- **One wave per row, consecutive lanes reading consecutive bytes.**
  One item per row reads strided; one wave per row reads coalesced.
  Measured 208 vs 162 GB/s on the same kernel body — the mapping *is*
  the ceiling. Short rows pack several per wave rather than idle lanes.
- **Take the integer route.** Quantize the activation once to q8_K
  (256 int8 values, eight sub-block sums, one f32 scale — 320 bytes)
  and dot int8 against int8 with `dotSum`; codebooks and nibbles reach
  int8 through `lut4`. A nonlinear 4-bit codebook decoded this way ran
  at 43 µs against llama.cpp's 58.4. The codebook maps to **int8**, not
  to the f16 scale — that separation is what makes the whole dot
  integer.
- **`dotSum` takes a signed receiver.** Read the proven idiom in the
  same kernel family before inventing a bias: a first draft that folded
  +128 into a table and paid it back off the sub-block sum was wrong by
  140% and slower than the plain form.
- **Per-byte reads: it is where they are, not how many.** The same
  `vload` fix bought 22% in one format and 0% in another that issued
  four times more byte reads — the second was already in the line the
  wave had fetched. Count lines touched, not loads issued.
- **Small tables live in L1, not LDS, for a 32-lane decode wave.** An
  8 KB stage is never amortized by one wave's worth of lookups:
  46.8 t/s reading the table from L1 against 37.1 staging it. Count
  lanes per stage before staging anything.
- **Halve the accumulators before anything else if the manifest shows
  spill.** Spilling is invisible to wall-clock A/Bs and reads as "flat";
  halving accumulators won 55–70% on kernels whose A/Bs had been flat
  for a week. Read `spillBytes` first, always.
- **An unpinned launch block caps VGPRs at 192.** A non-literal `block`
  is budgeted for 1024 threads. `@Occupancy(maxThreads)` on the kernel
  is the fix, not a despill.
- **Integer multiply is quarter-rate on RDNA.** `vector * scalar`
  inside a kernel costs eight `v_mul_lo`; reduce first, then scale once
  — integer-exact, and the scale is one multiply.
- **A weight-GB/s figure hides a fixed cost.** Fit `t = bytes / rate + b`
  across formats before chasing a low-bit kernel's "slack": at 2 bits
  the constant term is most of the time.
- **Dispatch once per bank, not once per expert.** A mixture layer that
  launches one mat-vec per selected expert spends three quarters of its
  decode outside any kernel: 231 ms of device time in 944 ms of wall.
  One launch per projection over an expert-id buffer — the
  `mul_mat_vec_id` shape, the row's slab offset through `sel[kk]` —
  took the same model from 39.9 to 122.1 t/s and the device to 80%
  busy. The kernels were already bit-identical; the launch count was
  the cost.

## 25.3 Prefill: fill the device, then feed the tile

A prefill GEMM is compute- or L2-bound only once every SIMD has work.
Before tuning a tile, check that the grid can fill the device.

- **A launch that issues sixteen workgroups cannot be fast.** One
  expert's GEMM at 64 padded tokens over 2048 outputs is `(2048/128) ×
  (64/64)` = 16 workgroups; it moved 1.62 MB in 182 µs — 8.9 GB/s, 4%
  of the ceiling — with 89 VGPRs, no spill and healthy occupancy in the
  manifest. The kernel was not the problem; the grid was. Group the
  work: one launch over an expert map (`mul_mat_id`), never one per
  expert.
- **Pad to the finest tile the format has.** Padding a ragged expert
  batch of ~34 tokens to 128 rows paid 3.7× dead work; a 64-row tile
  variant with four accumulators instead of eight was lighter (86–148
  VGPRs, no spill) and 31% faster on the same batches.
- **Pick the tile by K.** The cooperative X3 tile (32×64 warp tile)
  below K = 8192, X1 (64×32) above — a measured partition, kept in
  `Autotune`, not a literal.
- **Reuse is a kernel property.** A GEMM that reads the same weight
  tile for every M-block is at the ceiling before its inner loop is;
  a prefill gap that looked like a kernel's rate was weight *reuse*
  across the batch (recorded as 6.4.2 in the codebook-quants plan).
- **Attention costs what the context costs.** An 8-token prompt made
  attention look like 3 ms; at 512 tokens of context it is 97 ms of a
  125 ms token. Bench at the context you ship.
- **The tile gates on the head dimension.** The flash decode / prefill
  tile pair requires `hd == 128`; a 96-wide head silently takes the
  scalar pair. That is a routing row (§25.6), not a kernel bug.

## 25.4 Bind: one layout, and the copy that reads the lines

Weights are converted at upload, once. The rules are the decode rules
applied to a copy.

- **One resident layout for every format.** A row is its scale prefix
  (padded to a dword) then its dword-clean payloads, the scale at one
  end of the file block by construction so the payload is a single
  contiguous run. Every kernel reads that; nothing repacks at first
  touch. Retiring the runtime repack moved 177 ms out of a mixture
  model's first prefill.
- **One work item per payload dword, not one wave per block.** A
  64-lane wave copying an 18-byte block a byte at a time left 46 lanes
  idle: 28% occupancy. One item per dword: 16× fewer threads, −10.6%
  load on that file, flat on a 210-byte block that already filled the
  wave — the control that proved it was the mapping.
- **A separate pass over scattered bytes is read-amplification bound.**
  Extracting two scale bytes per block pulls one cache line per block:
  49× amplification, and the kernel ran at ~221 GB/s of *lines* — the
  ceiling — for 1% useful bytes. No lane mapping improves it. Fold the
  extraction into the pass that already reads those lines.
- **Budget device memory against the driver's pool, live.** On a UMA
  part the GTT limit is reached before `MemAvailable`; a device request
  one byte over 2 MB costs two 2 MB blocks. `Device.freeMemoryBytes()`
  and the allocation trace, not RSS.

## 25.5 Execution: launches, syncs, and what the compiler will not tell you

- **A sync per launch is a round trip.** Chain launches on one stream
  with `NoSync` launchers and wait once at the read side; a fused tail
  (down-projection + combine + the next layer's norm and pack in one
  launch) removes the gaps between them as well as the launches.
- **Keep the launch a top-level statement.** A kernel launched inside a
  lambda faults: captured buffers are unreachable to launch codegen.
- **No `?:` in a kernel body.** It lowers to wrong device code; use the
  `if` form.
- **Never reuse a name in a kernel body.** A differently-typed shadow
  in a `@Kernel` ships silent wrong device code.
- **Index vector lanes with constants.** A runtime lane index allocas
  per extract and never restores in a loop; constant lanes unroll.
- **`@FastMath` folds `x - (f32)(f16) x` to zero.** A device-computed
  f16 residual under `@FastMath` is dead; take it from LDS after a
  barrier, or leave `@FastMath` off that kernel.
- **Initialize accumulators by writing, not by multiplying.** `0 *`
  an uninitialized NaN churns and looks like allocator corruption.
- **`int64 *` traps on overflow.** Multiplicative hashing in a kernel
  is impossible; use the library's hash.
- **Trust the manifest over the wall clock.** `spillBytes`, `vgpr` and
  `residentGroupsPerCu` are read from the code object. A kernel that
  spills can A/B flat against one that does not.

## 25.6 Routing: declare it, then let the test find the gaps

Which kernel serves which format in which regime is one table with one
audit — [Kernel Routing](../specification/xpu/CajetaXPU-Routing.md).
The failure it prevents was measured four times in one day: a predicate
that named two formats where it meant "on an integer route", each
costing a whole route, one of them with a bare `else` that would have
fed one format's bytes through another's decoder. A route is a row; a
row's `needs` is a `Capability`; every row has a test that it fires and
a test that it does not.

## 25.7 Measuring: the order that does not lie

1. **Bit gate first.** A kernel that is faster and not the same kernel
   is not a fix. Compare against the host or the kernel it replaces —
   `==`, not a tolerance, where the arithmetic is the same.
2. **Validate the instrument.** Compare only bytes the layout defines;
   a fixture whose f16 scales are rotated bytes reads as NaN, and NaN
   compares unequal to itself and passes a bare non-zero check.
3. **Read the manifest before editing.** Spill is invisible to timing.
4. **Presence before rate.** The profiler's kernel table says whether
   the new kernel ran at all; a route that refused shows the old
   kernels at the old counts, and no A/B is worth taking until it does.
5. **A/B on the same box, arms alternating, gated on idle.** ABBA, three
   reps a pass; a fixed arm order let a decaying load read as a speedup.
   Load time is the box's witness: a shift there says the box moved,
   not the code.
6. **A flat A/B under the noise floor is not "no change."** A 45 ms
   kernel inside a 2.2 s load is 2% against ±3% spread; the kernel
   table resolves what the wall clock cannot.
7. **Controls must vary the mechanism.** A control that could not have
   refuted the explanation is decoration; the 210-byte block that
   already filled the wave was the control for the lane-mapping claim.
8. **Window the profiler on kernel populations, not on host-frame
   spans.** The device and host tiers have different origins; the load
   frame's span read as a device offset swept every prefill GEMM into
   "load". The window is right when its wall matches the harness's
   phase timer.
9. **Name the engine.** `llama.cpp (Vulkan)`, never `vulkan`: cajeta has
   its own Vulkan backend and the bare name misreads.
10. **The first model in a loop runs cold.** Re-measure a surprise on
    its own.
