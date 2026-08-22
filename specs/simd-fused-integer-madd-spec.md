# Fused integer multiply-add for quantized kernels

**Filed 2026-08-22**, from cajeta-llama plan 15.1.18(b). The engine's Q4_K
mat-vec sits ~100x above its memory floor while `llama.cpp` sits at its floor,
and the difference is three SIMD primitives cajeta does not expose.

## 1. Definition

`Vector<T,N>` today offers `tableLookup`, `widenLo`/`widenHi`, `narrow`,
`toF32`/`toI32`, `bitcastF32`/`bitcastI32`, and a DP4a-shaped `dot` on
`Vector<int8,4>`. Every one of those is *element-preserving*: the lane count
halves as element width doubles, and no operation combines a multiply with a
reduction.

Quantized inference is built on operations that do exactly that. This spec
defines the fused integer multiply-add family, so a quantized kernel written in
cajeta can reach the same instruction count as a hand-written intrinsic kernel.

**Non-goal.** Auto-vectorization, or inferring these from a scalar loop. The
spelling stays explicit, matching the rest of the Unit 17 toolkit.

## 2. The measurement that motivates this

One 4096x4096 projection, one token, `--release`, on a box with avx512f.

| kernel | time | notes |
|---|---|---|
| pure f32 dot, `Vector<float32,16>` | 3.43 ms | at the ~39 GB/s memory floor, 134 MB read |
| Q4_K block-direct, current best | 23.9 ms | reads 9.4 MB — 14x LESS data, 7x MORE time |
| Q4_K memory floor | ~0.24 ms | 9.4 MB at 39 GB/s |

- **2.1** The f32 dot proves the hardware and the codegen can reach the memory
  floor. The quantized kernel, reading a fraction of the data, cannot.
- **2.2** An int8-activation rewrite — fewer arithmetic operations on paper,
  half the activation traffic — measured **26.0 ms, SLOWER than the 24.2 ms
  f32 version it replaced**. Removing work made it slower, which is the
  signature of the emulation overhead dominating.
- **2.3** So the gap is not width, not memory, and not the activation format.
  It is that every integer multiply must be preceded by a widen and followed
  by a reduction, and each of those is a separate instruction.

## 3. What `llama.cpp` does that we cannot

From `ggml/src/ggml-cpu/arch/x86/quants.c`, `ggml_vec_dot_q4_K_q8_K`:

```c
p16l = _mm256_maddubs_epi16(q4l, q8l);    // 32 int8 pairs -> 16 int16, ONE op
p16l = _mm256_madd_epi16(scale_l, p16l);  // xscale AND reduce to int32, ONE op
sumi = _mm256_add_epi32(sumi, sumj);      // integer accumulate, whole block
acc  = _mm256_fmadd_ps(vd, _mm256_cvtepi32_ps(sumi), acc);  // ONE convert per 256
```

- **3.1** When 32 unsigned bytes multiply 32 signed bytes with adjacent
  products summed pairwise into 16 int16 lanes, that is one instruction
  (`vpmaddubsw`), not a widen pair plus a multiply.
- **3.2** When 16 int16 lanes multiply another 16 and adjacent products sum
  pairwise into 8 int32 lanes, that is one instruction (`vpmaddwd`). It is also
  how the per-sub-block scale is applied **in integer space**, so no float
  appears until the block is finished.
- **3.3** When a whole 256-element block accumulates in int32, exactly one
  int32->f32 conversion and one FMA occur per block. The current cajeta kernel
  converts 8 times per block because its scales are floats.
- **3.4** When the `dmin` term reads precomputed per-16 sums of the quantized
  activations (`block_q8_K.bsums`), it is integer throughout. The cajeta kernel
  keeps f32 group sums and a separate float pass.

## 4. Required operations

- **4.1** `mulAddPairsU8(Vector<uint8,N> a, Vector<int8,N> b) -> Vector<int16,N/2>`
  — adjacent products summed pairwise. Lowers to `vpmaddubsw` (x86),
  `vmull`+`vpadd` (NEON). Saturating, matching the hardware.
- **4.2** `mulAddPairs16(Vector<int16,N> a, Vector<int16,N> b) -> Vector<int32,N/2>`
  — lowers to `vpmaddwd` (x86), `vmull`+`vpadal` (NEON).
- **4.3** Both have a scalar fallback that produces bit-identical results, on
  the `CAJETA_SIMD_SCALAR_FALLBACK=1` discipline Unit 17 established.
- **4.4** The saturation semantics of `vpmaddubsw` are observable (0..15 x
  -127..127 cannot saturate, but the operation is defined for inputs that can),
  so the fallback must saturate identically rather than widen to int32.

## 5. Use cases

- **5.1** When a Q4_K sub-block's nibbles multiply int8 activations, the
  kernel emits one `mulAddPairsU8` rather than two widens and a multiply.
- **5.2** When per-sub-block scales apply, they apply in integer space via
  `mulAddPairs16`, so the block converts to float once.
- **5.3** When a Q6_K, Q5_K or Q8_0 kernel does the same, it uses the same two
  operations — the formats differ only in how the nibbles are unpacked.
- **5.4** When `CAJETA_SIMD_SCALAR_FALLBACK=1` is set, every kernel above
  produces byte-identical output, so the fallback is the correctness floor.
- **5.5** When a device (`@Kernel`) path uses the same helpers, one IR serves
  host and device, as Unit 17's ops already do.

## 6. Acceptance

- **6.1** `mulAddPairsU8` and `mulAddPairs16` match a scalar reference across
  the input range, including the saturating cases.
- **6.2** Both lower to the named intrinsic on x86, asserted against emitted IR
  the way `VectorSimdLadderTests` asserts `pshufb`.
- **6.3** A Q4_K mat-vec rewritten on them measurably beats 23.9 ms on the
  reference shape, and still agrees with `q4kMatVecIntoScalar`.
- **6.4** The engine-level number moves: decode is 10.91 s/token today against
  `llama.cpp`'s 0.177 s (5.64 t/s single-threaded), a 61x gap.

## 7. Open question

- **7.1** Whether the existing DP4a `dot` on `Vector<int8,4>` should generalize
  to wider lane counts instead of adding a separate family. It has the right
  shape (integer multiply plus reduction) but the wrong width and the wrong
  reduction granularity — DP4a reduces 4 lanes to 1 scalar, while these reduce
  pairwise and stay in vector space.
