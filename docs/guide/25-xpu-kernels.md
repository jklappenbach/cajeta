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

### 25.2.1 Example — an integer decode wave

The IQ4_NL mat-vec on the integer route, as shipped. One wave per
row; each lane owns one 32-element block; the nibble reaches int8
through a 16-entry codebook in a single `lut4`; two `dotSum`s against
the q8_K-packed activation; one wave reduction; lane 0 stores.

`cajeta-llm/src/main/cajeta/dev/cajeta/llm/io/QuantKernel.cajeta:14962-15009`

```cajeta
/**
 * WAVE-COOPERATIVE IQ4_NL x q8_K. Q4_0's twin — one LANE per
 * 32-element block, the same 16-byte payload and f16 scale — with
 * the nibble through a codebook instead of `q - 8`.
 *
 * `dotSum` takes a SIGNED receiver, so the codebook feeds it
 * directly — no bias term, where Q4_0 rides its -8 on the
 * activation sum to keep its quants unsigned.
 */
@Kernel
public static void iq4nlQ8WaveMatVecKernel(KernelBuffer<float32> y,
        KernelBuffer<int8> packed, KernelBuffer<int8> xp,
        uint32 rows, int64 blocksPerRow, int64 yOff) {
    uint32 lane = KernelThread.x();
    uint32 row = KernelThread.globalIdX() / 32;
    int64 prefix = ((blocksPerRow * 2L + 3L) / 4L) * 4L;
    int64 rowBytes = prefix + blocksPerRow * 16L;
    Vector<int8,16> kv = QuantKernel.iq4Kv();
    float32 acc = 0.0f;
    if (row < rows) {
        int64 i = (int64) row;
        int64 wb = (int64) lane;
        while (wb < blocksPerRow) {
            int64 ro = i * rowBytes + prefix + wb * 16L;
            float32 d = QuantKernel.scaleAtDev(packed,
                i * rowBytes / 2L + wb);
            Vector<int8,16> qs = packed.vload<16>(ro);
            Vector<int8,16> dl = (qs & 15).lut4(kv);
            Vector<int8,16> dh = ((qs >> 4) & 15).lut4(kv);
            int64 ab = wb / 8L;
            int64 xb = ab * 320L + (wb % 8L) * 32L;
            Vector<int8,16> a0 = xp.vload<16>(xb);
            Vector<int8,16> a1 = xp.vload<16>(xb + 16L);
            int32 dot = dl.dotSum(a0, 0) + dh.dotSum(a1, 0);
            Vector<int8,4> dv = xp.vload<4>(ab * 320L + 288L);
            float32 xs = Cajeta.bitsToF32(((int32) dv[0] & 255)
                | (((int32) dv[1] & 255) << 8)
                | (((int32) dv[2] & 255) << 16)
                | (((int32) dv[3] & 255) << 24));
            acc = acc + d * xs * (float32) dot;
            wb = wb + 32L;
        }
    }
    float32 tot = Wave.reduceSumF32(acc);
    if (lane == 0 && row < rows) {
        y[yOff + (int64) row] = tot;
    }
}
```

What to read in it:

- `int64 i = (int64) row;` then `i * rowBytes + prefix + wb * 16L` —
  the resident layout: a row is its f16 scale prefix, padded to a
  dword, then dword-clean 16-byte payloads. `scaleAtDev(packed,
  i * rowBytes / 2L + wb)` reads the block's scale from that prefix.
- `int64 wb = (int64) lane; ... wb = wb + 32L;` — lane `l` takes
  blocks `l, l+32, ...`: consecutive lanes read consecutive 16-byte
  payloads, which is the coalesced mapping (208 against 162 GB/s).
- `(qs & 15).lut4(kv)` and `((qs >> 4) & 15).lut4(kv)` — the codebook
  maps a nibble to **int8**, never to f16; that is what lets the dot
  stay integer. `dl.dotSum(a0, 0)` is signed × signed: the receiver is
  the int8 weight vector, the argument the int8 activation.
- `ab = wb / 8L; xb = ab * 320L + (wb % 8L) * 32L;` — a q8_K block is
  320 bytes: 256 int8 values, eight int32 sub-block sums at +256, one
  f32 scale at +288. Eight weight blocks of 32 share one q8_K block.
- `acc = acc + d * xs * (float32) dot;` — one float multiply per
  block, after the integer dot. Reduce first, scale once.
- `Wave.reduceSumF32(acc)` then `if (lane == 0 ...)` — the reduction is
  the hardware's; no LDS tree.

This kernel decodes the IQ4_NL 8B at 211 GB/s; the Q4_0 twin it was
modelled on reads 213. Its first draft folded a +128 bias into the
table for a mixed unsigned × signed dot and was wrong by 140%: read
the family's proven idiom (`iqDot16` in the same file) before
inventing one.

### 25.2.2 Example — the same kernel, grouped over experts

The IQ3_XXS id twin. Everything in the dot loop is the dense wave
kernel's; three things change, and they are the whole of "grouped
dispatch": which slab row the wave reads, where its activation starts,
and where its output lands.

`cajeta-llm/src/main/cajeta/dev/cajeta/llm/io/QuantKernel.cajeta:13146-13186`

```cajeta
/**
 * 6.4.3 cause 3 — the IQ3_XXS id twin of
 * {@link #iq3xxsQ8WaveMatVecKernel}: one wave per (selection slot,
 * row) against the bank's whole slab, the dot loop unchanged. The
 * codebook experts dispatched one launch per expert per projection
 * because this kernel did not exist.
 */
@Kernel
public static void iq3xxsQ8IdMatVecKernel(KernelBuffer<float32> y,
        KernelBuffer<int8> slab, KernelBuffer<int8> xp,
        KernelBuffer<int32> grid, KernelBuffer<int32> sel,
        uint32 rowsPerExpert, int64 blocksPerRow, int64 xRowBlocks,
        int64 yOff) {
    uint32 lane = KernelThread.x();
    uint32 wave = KernelThread.globalIdX() / 32;
    uint32 kk = wave / rowsPerExpert;
    uint32 row = wave - kk * rowsPerExpert;
    int64 prefix = ((blocksPerRow * 2L + 3L) / 4L) * 4L;
    int64 rowBytes = prefix + blocksPerRow * 96L;
    float32 acc = 0.0f;
    if (row < rowsPerExpert) {
        int64 i = (int64) sel[(int64) kk] * (int64) rowsPerExpert
            + (int64) row;
        int64 xbase = (int64) kk * xRowBlocks;
        int64 sub = (int64) (lane & 7);
        int64 b = (int64) (lane >> 3);
        while (b < blocksPerRow) {
            int64 xb = (xbase + b) * 320L + sub * 32L;
            float32 v = QuantKernel.iq3xxsSub(slab, xp, grid,
                i * rowBytes + prefix + b * 96L, sub, xb);
            acc = acc + QuantKernel.scaleAtDev(slab,
                i * rowBytes / 2L + b)
                * QuantKernel.q8kScaleAtDev(xp, xbase + b) * 0.25f * v;
            b = b + 4L;
        }
    }
    float32 tot = Wave.reduceSumF32(acc);
    if (lane == 0 && row < rowsPerExpert) {
        y[yOff + (int64) kk * (int64) rowsPerExpert + (int64) row] = tot;
    }
}
```

- `kk = wave / rowsPerExpert; row = wave - kk * rowsPerExpert;` — the
  grid is `rowsPerExpert × used` waves, flattened. `kk` is the
  selection slot, `row` the output row within the expert.
- `i = sel[kk] * rowsPerExpert + row` — the row's offset in the bank's
  contiguous slab, through the device-side selection buffer. One
  launch covers every selected expert; nothing is read back to pick.
- `xbase = kk * xRowBlocks` — 0 in decode (every expert reads the one
  staged token), `width/256` when each slot has its own row.
- `y[yOff + kk * rowsPerExpert + row]` — output slot-major, so the
  combine reads `used` contiguous rows.

Bit-identical to `used` per-expert launches of the dense kernel over
the same slab (§25.7's gate), and the difference between 39.9 and
122.1 t/s on a 60-expert model: the launch count, not the arithmetic.

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

### 25.3.1 Example — a grid that cannot fill the device

The per-expert coop launch for IQ4_NL, first sixteen lines. The kernel
body is not the lesson; the grid is.

`cajeta-llm/src/main/cajeta/dev/cajeta/llm/io/QuantKernel.cajeta:4971-4986`

```cajeta
public static void iq4nlF16CoopAutoLaunchNoSync(KernelBuffer<float32> y,
        KernelBuffer<int32> packed, KernelBuffer<float16> xh,
        int64 rows, int64 outDim, int64 cols) {
    int64 wgL = (outDim / 128L) * (rows / 128L);
    KernelStream s = QuantKernel.stream();
    // 6.4.3: a ragged MoE expert batch takes the halved tile rather
    // than paying for 128 rows it does not have.
    if (QuantKernel.coopN64 && rows % 128L != 0L && rows % 64L == 0L) {
        iq4nlF16CoopN64Kernel.launch(s,
            grid: [(uint32) ((outDim / 128L) * (rows / 64L))],
            block: [256])
            (y, packed, xh, (uint32) rows, (uint32) outDim,
             (uint32) (outDim / 128L), cols / 32L);
        return;
    }
    if (cols > 8192L) {
```

`grid: [(outDim / 128L) * (rows / 64L)]` — for one expert's down
projection, `outDim` 2048 and ~34 routed tokens padded to 64, that is
sixteen workgroups of 256 threads. The kernel has 89 VGPRs, no spill
and three resident groups a CU in its manifest; it moved 1.62 MB in
182 µs, 8.9 GB/s, because sixteen workgroups leave most of the device
idle and 3810 such launches ran in series. The `rows % 128L != 0L`
test chose the halved tile correctly — the tile is right, the launch
shape is wrong. The fix is the id-GEMM shape of §25.2.2 applied to a
GEMM: one launch over an expert map.

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

### 25.4.1 Example — the split, one item per dword

The payload half of the resident split, and the launcher that gates it.

`cajeta-llm/src/main/cajeta/dev/cajeta/llm/io/QuantKernel.cajeta:6353-6386`

```cajeta
/**
 * The DWORD form of {@link #splitKernel}: one work item per payload
 * dword rather than one wave per block.
 *
 * The scale sits at one END of the block by construction, so a
 * block's payload is a single contiguous run in the source, and its
 * destination is dword-aligned because the row prefix is padded to
 * one. The byte form gave every block a 64-lane wave and walked it
 * a byte at a time: an 18-byte IQ4_NL block left 46 lanes idle and
 * issued 18 byte-writes where this issues 4 dword-writes, coalesced
 * across the wave.
 */
@Kernel
public static void splitPayloadKernel(KernelBuffer<int32> outW,
        KernelBuffer<int8> src, uint32 total, int64 blockBase,
        uint32 blockBytes, uint32 payloadWords, uint32 srcPayloadOff,
        int64 bpr, int64 prefixWords, int64 rowWords) {
    uint32 t = KernelThread.globalIdX();
    if (t < total) {
        int64 b = (int64) (t / payloadWords);
        int64 k = (int64) (t % payloadWords);
        int64 gb = blockBase + b;
        int64 row = gb / bpr;
        int64 j = gb - row * bpr;
        int64 so = b * (int64) blockBytes + (int64) srcPayloadOff
            + k * 4L;
        int32 w = ((int32) src[so] & 255)
            | (((int32) src[so + 1L] & 255) << 8)
            | (((int32) src[so + 2L] & 255) << 16)
            | (((int32) src[so + 3L] & 255) << 24);
        outW[row * rowWords + prefixWords
            + j * (int64) payloadWords + k] = w;
    }
}
```

- One work item per payload **dword**, `t / payloadWords` the block and
  `t % payloadWords` the word within it — not one 64-lane wave per
  block walking bytes. On an 18-byte block the wave form left 46 lanes
  idle; this form dispatches 16× fewer threads and cut load 10.6%.
- `outW` is `KernelBuffer<int32>`, the byte buffer's word view: the
  store is a dword because the row prefix is padded to one and every
  payload is dword-clean by construction.
- The source read is four byte loads assembled with shifts — the file
  block is not aligned, the resident row is.

`cajeta-llm/src/main/cajeta/dev/cajeta/llm/io/QuantKernel.cajeta:6491-6539`

```cajeta
public static void splitLaunch(KernelBuffer<int8> out,
        KernelBuffer<int8> src, int64 nBlocks, int64 blockBase,
        int32 ty, int64 bpr) {
    KernelStream s = QuantKernel.stream();
    int64 prefix = Quant.rowPrefixBytes(ty, bpr);
    int64 rowBytes = Quant.rowResidentBytes(ty, bpr);
    int64 pay = (int64) Quant.payloadBytes(ty);
    int64 scaleB = (int64) Quant.scaleBytes(ty);
    int64 scaleOff = (int64) Quant.scaleOffset(ty);
    if (pay % 4L == 0L && prefix % 4L == 0L && rowBytes % 4L == 0L) {
        KernelBuffer<int32> outW #= out.wordView();
        int64 pw = pay / 4L;
        int64 total = nBlocks * pw;
        int64 srcPayOff = 0L;
        if (scaleOff == 0L) { srcPayOff = scaleB; }
        splitPayloadKernel.launch(s,
            grid: [(uint32) ((total + 255L) / 256L)], block: [256])
            (outW, src, (uint32) total, blockBase,
             (uint32) Quant.blockBytes(ty), (uint32) pw,
             (uint32) srcPayOff, bpr, prefix / 4L, rowBytes / 4L);
        if (scaleB == 2L && blockBase % bpr == 0L
                && nBlocks % bpr == 0L) {
            int64 pfw = prefix / 4L;
            int64 st = (nBlocks / bpr) * pfw;
            QuantKernel.scaleWordLaunches =
                QuantKernel.scaleWordLaunches + 1L;
            splitScaleWordKernel.launch(s,
                grid: [(uint32) ((st + 255L) / 256L)], block: [256])
                (outW, src, (uint32) st, blockBase / bpr,
                 (uint32) Quant.blockBytes(ty), (uint32) scaleOff,
                 bpr, pfw, rowBytes / 4L);
        } else {
            splitScaleKernel.launch(s,
                grid: [(uint32) ((nBlocks + 255L) / 256L)], block: [256])
                (out, src, (uint32) nBlocks, blockBase,
                 (uint32) Quant.blockBytes(ty), (uint32) scaleB,
                 (uint32) scaleOff, bpr, rowBytes);
        }
        s.sync();
        return;
    }
    splitKernel.launch(s, grid: [(uint32) nBlocks], block: [64])
        (out, src, (uint32) nBlocks, blockBase,
         (uint32) Quant.blockBytes(ty), (uint32) Quant.payloadBytes(ty),
         (uint32) Quant.scaleBytes(ty), (uint32) Quant.scaleOffset(ty),
         bpr, prefix, Quant.rowResidentBytes(ty, bpr));
    s.sync();
    return;
}
```

- The dword form is taken only when `pay % 4 == 0 && prefix % 4 == 0
  && rowBytes % 4 == 0`; the byte kernel stays for anything else. A
  route with a shape constraint keeps its fallback and names it.
- The scale half takes its own dword kernel only when the chunk starts
  and ends on a row (`blockBase % bpr == 0 && nBlocks % bpr == 0`),
  because a prefix dword spans two blocks of the same row; and
  `scaleWordLaunches` counts it, so a test can assert the branch fired
  (§25.7.2). That scale kernel is read-amplification bound at the
  ceiling — 49 lines pulled per 2 useful bytes — which no mapping
  fixes; folding it into the payload pass is the open item.
- `s.sync()` at the end: bind-time work pays one round trip per chunk
  and is allowed to. Decode is not (§25.5).

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
fed one format's bytes through another's decoder.

Three rules carry it. The rest of this section is what each looks like
in code you can read today.

- **One row is one kernel variant**, not a family with arms. Q4_K's
  wave variant and its packed variant are two rows, and `priority`
  orders them. A family row keeps its arms, an arm-set keeps its bare
  `else`, and that `else` is the defect.
- **A static constraint and a runtime one go in different halves.**
  `shapeRefusal(query)` is pure and the audit walks it with nothing
  bound; `readyRefusal(call)` reads the receiver and no audit can check
  it. The types enforce the split — a query carries no receiver to
  reach through, so a static gate cannot read a slab even by accident.
- **A half answers with the gate, not with `false`.** Both return the
  row's own name for what refused, or null to admit. A string literal
  is a static instance, so naming costs nothing, and there is one
  statement of each gate instead of a predicate plus a list of reasons
  beside it.

A row's `needs` is a `Capability`, never a backend name. Every row owes
a test that it fires and a test that it does not (§25.7.2).

### 25.6.1 Example — a predicate and the dispatcher it guards

`Linear.packedWaveReady` and `matvecPackedKeep`, as they are now. Until
2026-09-17 the predicate ended `return (this.q8 && this.wave) ||
this.wave6;` and the dispatcher was `if (q8 && wave) { q4k } else {
q6k }`. Widening the predicate alone would have sent codebook bytes
through the Q6_K decoder — wrong logits, no crash — which is why a
route's admission and its arms must change together, and why the
audit checks that an admitted format has an arm.

`cajeta-llm/src/main/cajeta/dev/cajeta/llm/model/Linear.cajeta:3118-3126`

```cajeta
boolean packedWaveReady() {
    if (this.packedTy < 0 || !QuantKernel.routingEnabled()) {
        return false;
    }
    this.ensureDevice();
    if (this.hasEpilogue() || !this.packedAct) { return false; }
    return (this.q8 && this.wave) || this.wave6 || this.waveIq
        || this.wave4nl;
}
```

`cajeta-llm/src/main/cajeta/dev/cajeta/llm/model/Linear.cajeta:3135-3156`

```cajeta
boolean matvecPackedKeep(KernelBuffer<int8> xp) {
    if (!this.packedWaveReady()) { return false; }
    if (this.q8 && this.wave) {
        QuantKernel.q4kQ8WaveMatVecLaunchNoSync(this.yDev, this.payloadDev,
            xp, (int64) this.outDim, (int64) this.inDim, 0L);
    } else if (this.wave6) {
        QuantKernel.q6kQ8WaveMatVecLaunchNoSync(this.yDev, this.payloadDev,
            xp, (int64) this.outDim, (int64) this.inDim, 0L);
    } else if (this.waveIq) {
        QuantKernel.iqQ8WaveMatVecLaunchNoSync(this.packedTy, this.yDev,
            this.payloadDev, xp, (int64) this.outDim,
            (int64) this.inDim, 0L);
    } else if (this.wave4nl) {
        QuantKernel.iq4nlQ8WaveMatVecLaunchNoSync(this.yDev,
            this.payloadDev, xp, (int64) this.outDim,
            (int64) this.inDim, 0L);
    } else {
        return false;
    }
    Linear.nLaunches = Linear.nLaunches + 1L;
    return true;
}
```

- `this.waveIq || this.wave4nl` — the predicate now asks "on an integer
  wave route", which is what it always meant. Each of those booleans is
  one row: the predicate and its matching arm become a `format()` and a
  `dispatch()` that has nothing left to choose.
- Every branch is explicit and the last is `return false`. A bare
  `else` is a dispatcher with an arm for a format it was never asked
  to serve. One row per variant is how that `else` stops existing —
  there is no arm to omit and nothing to fall through to.
- `Linear.nLaunches + 1` — the launch count is instrumented at the
  dispatch, so a census can be checked against it.

### 25.6.2 Example — which half a constraint belongs in

`ExpertBank.idReady`, as it is now. Four lines answering two different
questions at once: which formats this row is for, and whether this
bank's widen actually happened. Widening it for a new format also
claims that format's slab is ready — that conflation is the defect, not
an incidental of it, and it is why one predicate cannot be both halves.

`cajeta-llm/src/main/cajeta/dev/cajeta/llm/model/ExpertBank.cajeta:929-936`

<!-- snippet: skip -->

```cajeta
public boolean idReady() {
    if (this.packedTy == 12 || this.packedTy == 14) { return true; }
    if (ExpertBank.codebookId(this.packedTy)) { return true; }
    return ExpertBank.widenSlabOn && !this.widenRefused
        && (this.packedTy == Quant.GG_Q8_0
            || this.packedTy == Quant.GG_Q5_0
            || this.packedTy == Quant.GG_Q4_0);
}
```

- `packedTy == 12 || packedTy == 14`, and `codebookId`'s twin list, are
  the row's `format()`. They are not a constraint at all — they are the
  key the table resolves on, and a list of them in a predicate is the
  duplicated knowledge the table removes.
- `this.widenRefused` is written by a bind that failed: the receiver's
  state, so `readyRefusal`.
- `ExpertBank.widenSlabOn` is a mutable static — an A/B arm someone can
  flip at run time. It goes in `readyRefusal` too, not in the static
  half, and the test is not "is this value known at compile time" but
  "can the audit's answer change depending on when it ran". A global
  switch fails that; a query field does not.

`MoeFfn.zeroSyncReady` (`MoeFfn.cajeta:1248-1288`) is the same reading
with every case present: nine gates with nine sentences in the
`moe-row-route` record. Three are per-bank format tests, so they become
the `format()` of three rows. Four are shape — `gating != 1`,
`hidden % 256`, `widthN % 32`, the shared expert's width — and go in
`shapeRefusal`, where the audit can walk them with nothing bound. Two
read a slab that did or did not bind, and go in `readyRefusal`. Each
keeps its own sentence, which is the whole reason a half answers with
the gate rather than with `false`: collapse them and you have
reproduced the thing the table was built to remove.

### 25.6.3 Example — the row those predicates become

The shape of one row. The registrant declares its static facts once, on
its own `RouteQuery` subclass, and both halves read them; the receiver
and the operands ride on `RouteCall`. The llm rows land with
codebook-quants 9.2.9.

```cajeta
import cajeta.lang.String;
import cajeta.xpu.Capability;
import cajeta.xpu.Regime;
import cajeta.xpu.Route;
import cajeta.xpu.RouteCall;
import cajeta.xpu.RouteQuery;

public final class WeightQuery extends RouteQuery {
    public int32 inDim;
    public int32 outDim;
    public WeightQuery(Regime g, int32 ty, int32 inDim, int32 outDim) {
        this.regime = g;
        this.ty = ty;
        this.inDim = inDim;
        this.outDim = outDim;
    }
}

public final class WeightCall extends RouteCall {
    public boolean slabBound;
    public boolean widened;
    public WeightCall(WeightQuery q, boolean slabBound, boolean widened) {
        this.query #= q;
        this.slabBound = slabBound;
        this.widened = widened;
    }
}

public final class IntWaveRow implements Route {
    String nm;
    int32 fmt;
    Capability[] caps;

    public IntWaveRow(String nm, int32 fmt, Capability[] caps) {
        this.nm #= nm;
        this.fmt = fmt;
        this.caps #= caps;
    }

    public String name() { return this.nm; }
    public Regime regime() { return Regime.DecodeRow; }
    public int32 format() { return this.fmt; }
    public int32 priority() { return 20; }
    public Capability[] needs() { return this.caps; }

    public String shapeRefusal(RouteQuery q) {
        if (q instanceof WeightQuery w) {
            if (w.inDim % 256 != 0) { return "in width not 256-aligned"; }
            if (w.outDim % 32 != 0) { return "out width not 32-aligned"; }
            return null;
        }
        return "not a weight query";
    }

    public String readyRefusal(RouteCall c) {
        if (c instanceof WeightCall w) {
            if (!w.slabBound) { return "slab is not bound"; }
            if (!w.widened) { return "slab is not widened"; }
            return null;
        }
        return "not a weight call";
    }

    public void dispatch(RouteCall c) {
        return;
    }
}
```

- `format()` is one format. A second variant for the same format is a
  second row with a different `priority()`; the audit reports the pair
  as shadowed rather than letting the choice be implicit.
- `shapeRefusal` names which width refused. A row with one gate could
  have returned a boolean; a row with four could not, and every real
  one has four.
- `readyRefusal` reads `WeightCall`. It could not read it from
  `shapeRefusal` — that parameter is a `RouteQuery` and has no receiver
  on it.
- `dispatch` is where the launch goes, and it is the only arm: one
  `k.launch(...)`, no `if`, nothing to fall through. Compare
  `matvecPackedKeep` above, which is five arms and a bare `else`.
- `needs()` returns a stored list, so the selector asking it costs no
  allocation. `null` means the row needs nothing.

Registration is once, at start-up; resolution is once, **at bind**. The
weight stores the row it got and the launch is one virtual call — this
is the same trade the sixteen booleans on `Linear` make today, with the
knowledge in one place instead of sixteen.

<!-- snippet: skip -->

```cajeta
table.register(heap IntWaveRow("q4k-int-wave", Quant.GG_Q4_K, null));

WeightCall call = heap WeightCall(this.query, this.slabBound,
    this.widened);
this.row = table.pick(call);
if (this.row == null) {
    RouteRefusal why #= table.whyNotPicked(call);
    Diag.emit("weight-route", 0 - 1, why.text(), 0L, 0L, 0L, 0L, 0L, 0L);
}
```

- `pick` returns the row or nothing, and allocates nothing either way.
  The refusal is built by `whyNotPicked`, on the branch that is already
  about to print — so a route record costs the path that took the row
  nothing at all.
- `why.text()` names the row and the gate in one line. The failure this
  replaces is a census showing the old kernels and no way to tell which
  of a dozen predicates said no.
- Nothing here tests a format. If you find yourself writing
  `ty == GG_IQ3_XXS || ty == GG_IQ4_NL` outside a row's `format()`, that
  is the list the table exists to delete.

### 25.6.4 Example — the audit, and what it will not check

`audit` walks the **declared** half over the registrant's own queries —
format, regime, shape. Nothing is bound, and no device is asked, which
is what lets it run in the same commit as the route it checks and on a
machine with none of the hardware.

<!-- snippet: skip -->

```cajeta
RouteQuery[] qs = heap RouteQuery[3];
qs[0] = heap WeightQuery(Regime.DecodeRow, Quant.GG_Q4_K, 2048, 4096);
qs[1] = heap WeightQuery(Regime.DecodeRow, Quant.GG_IQ3_XXS, 1408, 4096);
qs[2] = heap WeightQuery(Regime.DecodeRow, Quant.GG_IQ4_NL, 2048, 4096);
RouteAudit a #= table.audit(qs);
Assert.equals(a.unservedCount(), 0);
Assert.equals(a.neverFiringCount(), 0);
```

- `unservedCount()` is the check that a new format has a row. Run it
  over the package's whole format set and it names every row that lacks
  the format, the day the format lands.
- `neverFiringCount()` is the other direction: a row that serves
  nothing in the set has a format that left it, or a gate that refuses
  everything. A predicate that silently disabled a whole check once read
  as a clean run for an hour.
- `a.shadowed(qi)` says two rows admit one query. Priority still
  decides; the point is that the choice is visible.
- `table.auditRow(row, qs)` is the per-row fire / no-fire count — hand
  it the row's own format at shapes that should and should not take it,
  and assert both halves. §25.7.2 is that pair written against a real
  kernel.

- `a.needsCount(ri)` / `a.needAt(ri, k)` list what a row DECLARES it
  needs. The audit reports that and does not rule on it: a capability is
  a fact about the box, and a row the local machine cannot run still
  serves its format. `needs` is `pick`'s question, on the live device.

What the audit will not do is ask `readyRefusal`, or ask the device. A
slab that did not bind and a capability this box lacks are both
invisible to it — those refusals are `whyNotPicked`'s, at bind, in the
route record. This is the same question the two halves answer: if the
answer could change with where or when the walk ran, it is not the
audit's to give.

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

### 25.7.1 Example — the bit gate for a grouped kernel

The test that admitted §25.2.2's kernel. It builds a slab of six
experts from a real fixture, gives each expert a different block
rotation so a kernel that ignored `sel` cannot agree by accident,
packs one activation, and compares the grouped launch against the
per-expert wave launches it replaces — `==`, never a tolerance.

`cajeta-llm/src/test/cajeta/dev/cajeta/llm/selftest/MoeCodebookIdMatVecTest.cajeta:84-167`

```cajeta
static void check(int32 ty, String fixture) {
    String be #= Device.activeBackend();
    if (be.equals("cpu") || be.equals("none")) { return; }
    if (!Linear.residentEnabled()) { return; }
    int64 E = 6L;
    int64 rows = 64L;
    int64 cols = 2048L;
    int32 used = 3;
    KernelBuffer<int8> slab #= MoeCodebookIdMatVecTest.slabOf(ty,
        fixture, E, rows, cols);
    float32[] xv #= heap float32[cols];
    int64 j = 0;
    while (j < cols) {
        xv[j] = (float32) ((j % 25L) - 12L) / 64.0f;
        j = j + 1;
    }
    KernelBuffer<float32> xf #= heap KernelBuffer<float32>((uint64) cols);
    xf.upload(xv, cols);
    KernelBuffer<int8> xp #= heap KernelBuffer<int8>(
        (uint64) ((cols / 256L) * 320L));
    QuantKernel.q8kPackLaunchNoSync(xp, xf, cols);

    int32[] ids #= heap int32[used];
    ids[0] = 4;
    ids[1] = 0;
    ids[2] = 3;
    KernelBuffer<int32> sel #= heap KernelBuffer<int32>((uint64) used);
    sel.upload(ids, (int64) used);
    int64 n = (int64) used * rows;
    KernelBuffer<float32> yId #= heap KernelBuffer<float32>((uint64) n);
    KernelBuffer<float32> yWv #= heap KernelBuffer<float32>((uint64) n);

    boolean ok = QuantKernel.idMatVecLaunchNoSync(ty, yId, slab, xp,
        sel, used, rows, cols, 0L);
    if (!ok) {
        Assert.fail("ty=" + ty + ": no id mat-vec kernel");
    }
    int64 resRow = Quant.rowResidentBytes(ty,
        cols / (int64) Quant.blockElems(ty));
    int32 k = 0;
    while (k < used) {
        KernelBuffer<int8> ex #= slab.slice(
            (uint64) ((int64) ids[k] * rows * resRow),
            (uint64) (rows * resRow));
        if (ty == Quant.GG_IQ4_NL) {
            QuantKernel.iq4nlQ8WaveMatVecLaunchNoSync(yWv, ex, xp,
                rows, cols, (int64) k * rows);
        } else {
            QuantKernel.iqQ8WaveMatVecLaunchNoSync(ty, yWv, ex, xp,
                rows, cols, (int64) k * rows);
        }
        k = k + 1;
    }
    KernelStream s = QuantKernel.stream();
    s.sync();
    float32[] a #= heap float32[n];
    float32[] b #= heap float32[n];
    yId.download(a, n);
    yWv.download(b, n);
    int64 diff = 0L;
    int64 nan = 0L;
    boolean nz = false;
    int64 i = 0;
    while (i < n) {
        if (a[i] != b[i]) {
            if (diff < 4L) {
                System.stdout.println("   ty=" + ty + " row " + i
                    + ": id " + a[i] + " wave " + b[i]);
            }
            diff = diff + 1L;
        }
        if (a[i] != 0.0f && a[i] == a[i]) { nz = true; }
        if (a[i] != a[i]) { nan = nan + 1L; }
        i = i + 1;
    }
    Assert.equals(0L, diff);
    // A NaN output compares unequal to itself, so it would read as a
    // kernel disagreement AND satisfy a bare non-zero check.
    Assert.equals(0L, nan);
    Assert.isTrue(nz);
    System.stdout.println("   ty=" + ty + " id == wave over " + n
        + " rows");
    return;
}
```

- `a[i] != a[i]` counts NaN, and the non-zero check demands
  `a[i] == a[i]` as well. The first version of this fixture rotated
  *resident* bytes, slid the f16 scale field onto payload bytes, and
  produced NaN on both arms — which compares unequal to itself and
  would have satisfied a bare "not all zero." Validate the instrument.
- `QuantKernel.stream().sync()` once, after every launch on both arms.

### 25.7.2 Example — a test that the route fires, and one that it does not

For the split's dword scale kernel (§25.4.1). The existing byte-exact
tests would have gone green without executing a line of the new
kernel — their chunks end mid-row, so both took the fallback. Hence a
counter on the branch and a pair of tests against it.

`cajeta-llm/src/test/cajeta/dev/cajeta/llm/selftest/ResidentLayoutTest.cajeta:190-205`

```cajeta
@Test
public void theScaleSplitTakesTheDwordKernelOnARowAlignedChunk() {
    String be #= Device.activeBackend();
    if (be.equals("cpu") || be.equals("none")) { return; }
    int64 before = QuantKernel.scaleWordLaunchCount();
    ResidentLayoutTest.checkDeviceSplit(Quant.GG_IQ4_NL, "iq4_nl.bin",
        13L, 317L, 0L);
    ResidentLayoutTest.checkDeviceSplit(Quant.GG_Q6_K, "q6_k.bin",
        7L, 53L, 0L);
    int64 fired = QuantKernel.scaleWordLaunchCount() - before;
    if (fired != 2L) {
        Assert.fail("a row-aligned split took the dword scale kernel "
            + fired + " times, wanted 2");
    }
    return;
}
```

The twin, `theScaleSplitKeepsTheBlockKernelOnAChunkThatEndsMidRow`,
splits at `first = 2000` with `bpr = 317` and asserts the counter did
not move. Red first at "0 times, wanted 2"; the byte-exactness half of
the same test passed while red. That is the pair every route row gets.
