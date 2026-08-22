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

## 4. Required operation

Revised 2026-08-22 after checking what the ISAs actually provide: this is ONE
operation, not the two originally proposed, and it is a GENERALIZATION of the
`Vector<int8,4>.dot` that already exists.

- **4.1** `dotAccum(w, a, acc)` — multiply 4 adjacent int8 pairs, sum the four
  products, and accumulate into the corresponding int32 lane:

  ```
  Vector<int32,N> dotAccum(Vector<uint8,4N> w, Vector<int8,4N> a,
                           Vector<int32,N> acc)
  ```

  The result stays in VECTOR space. That is the whole point: 15.1.18(a)
  measured that reduce frequency, not width, is what costs — a Q4_K kernel
  reducing per sub-block ran 70.7 ms against 24.2 ms for the same kernel
  reducing once per block.

- **4.2** The existing `Vector<int8,4>.dot -> int32` is this operation at N=1
  with the accumulator dropped and the lane collapsed to a scalar. It should
  generalize rather than gain a sibling family.

- **4.3** Two sign variants, not four: `uint8 x int8` (quantized weights
  against signed activations — the case every K-quant needs) and
  `int8 x int8`. RISC-V exposes all four combinations; the other two have no
  caller here and would be surface without use.

### Lowering

Every modern ISA has this, and the RISC/CISC split does not predict who: ARM is
the textbook RISC and shipped it in ARMv8.2-A (2016). These are single-uop,
fully pipelined ops — `vpdpbusd` is ~5-cycle latency, like an FMA — not
microcoded CISC leftovers. They are domain-specific ML accelerators, and
quantized inference is why they exist.

| target | instruction | ops |
|---|---|---|
| x86 AVX512-VNNI / AVX-VNNI | `vpdpbusd` (`x86_avx512_vpdpbusd_512`) | 1 |
| x86 AVX2, pre-VNNI | `vpmaddubsw` + `vpmaddwd` + `vpaddd` | 3 |
| AArch64 ARMv8.2 dotprod | `udot` / `sdot` (`aarch64_neon_udot`) | 1 |
| AArch64 base | `umull`/`smull` + `uadalp` | 3 |
| RISC-V Zvqdotq | `vqdotsu` / `vqdot` (`riscv_vqdotsu`) | 1 |
| WASM SIMD | `wasm_dot` (2-way i16) plus a widen | 3 |
| elsewhere, or `CAJETA_SIMD_SCALAR_FALLBACK=1` | scalar loop | — |

- **4.4** All of the above are present in the LLVM the toolchain builds against
  (22.0.0git), verified in `IntrinsicsX86.h`, `IntrinsicsAArch64.h` and
  `IntrinsicsRISCV.h`. `tableLookup` is the in-repo precedent for emitting a
  target intrinsic with a NEON path and a scalar fallback
  (`src/cajeta/type/VectorOps.h`), and this follows its shape exactly.
- **4.5** Note the mixed-sign asymmetry is not an x86 quirk: RISC-V's
  `vqdotsu` is the same signed x unsigned form. Two independent ISAs converged
  on it because that is the shape quantized inference needs.
- **4.6** Six lowering paths against `tableLookup`'s three is the real cost of
  this primitive, and the scalar fallback is what makes it verifiable — every
  path must produce bit-identical results.

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

## 7. Open questions

- **7.1** CLOSED 2026-08-22 — the DP4a `dot` DOES generalize, and that is the
  design (§4.2). The original two-primitive proposal (`mulAddPairsU8` +
  `mulAddPairs16`) modelled llama.cpp's AVX2 path, which predates VNNI. On a
  VNNI/dotprod/Zvqdotq target the 4-way accumulate is one instruction and the
  pairwise pair is two, so the pair is the FALLBACK, not the interface.
- **7.2** CLOSED 2026-08-22 — EXPLICIT accumulator. Decided by measurement,
  not preference.

  The implied form (`acc + w.dot(a)`, backend fuses) was tested directly: a
  six-line C loop of exactly that shape, `-O3 -march=native` on this
  VNNI-capable box, clang 22. LLVM generated THREE inner loops from it:

  | block | width | instruction |
  |---|---|---|
  | `.LBB0_11` | zmm, 512-bit — the wide main loop | `vpmaddwd`, UNFUSED |
  | `.LBB0_15` | xmm, 128-bit | `vpdpbusd`, fused |
  | `.LBB0_8` | xmm, 128-bit | `vpmovsxbw` + `vpmaddwd`, UNFUSED |

  The hottest, widest loop did NOT get the fused instruction — LLVM preferred
  `vpmaddwd` at 512-bit over `vpdpbusd` at 512-bit, and only reached for
  `vpdpbusd` in a narrow path. Auto-fusion is not merely unreliable here; it
  declined exactly where it mattered, on the simplest possible input. A kernel
  built on it would be a coin flip per loop shape, per target, per LLVM
  version, and the failure is INVISIBLE in the source.

  `dotAccum(w, a, acc)` also maps 1:1 to the hardware: `vpdpbusd`, `udot` and
  `vqdotsu` all compute `dst += dot(a, b)` destructively, so the accumulator is
  the instruction's own shape rather than an ergonomic tax.

  That `acc = w.dotAccum(a, acc)` repeats `acc` is a FEATURE. 15.1.18(a)
  measured that reduce frequency, not width, is what costs — a kernel reducing
  per sub-block ran 70.7 ms against 24.2 ms reducing once per block. A syntax
  that hides where accumulation happens hides the one thing that has to be
  reasoned about.

- **7.3** `dot(w, a)` ships too, defined as `dotAccum(w, a, zeros)` — one extra
  entry point over the same lowering, so the non-accumulating case has a
  spelling and nobody is tempted back to `acc + dot(...)`.

- **7.4** Every lowering asserts its emitted intrinsic in test, the way
  `VectorSimdLadderTests` pins `pshufb` for `tableLookup`. That makes "did we
  actually get the instruction" a unit-test failure rather than a profiling
  discovery — which is what would have caught both of 15.1.18's silent
  regressions (a 512-bit rewrite that ran 2x slower, an int8 rewrite with fewer
  ops that also ran slower) before they were measured.
