# The Q4_K kernel's cost is scalar block-header decode, not arithmetic

**Filed 2026-08-22**, from `simd-fused-integer-madd` §2.4. That spec predicted
the missing fused multiply-add was why Q4_K sits ~100x above its memory floor.
The primitive was built, measured, and moved the kernel **5%**. This spec is
what the measurement found instead.

## 1. Definition

A Q4_K mat-vec spends 78% of its time decoding two block-header fields and 9%
on the arithmetic those fields scale. This spec defines the work to remove that
overhead. It is scoped to the packed K-quant mat-vecs in `cajeta-llama`
(`Quant.q4k/q5k/q6k/q8MatVecInto*`) and to the one compiler defect that forces
the larger half.

**Non-goals.** Changing quantization formats, changing the engine's numerics,
or further work on the SIMD primitive — `dotAccum` is done and is not the
lever.

## 2. The measurement

4096x4096 projection, one token, `--release`, `--cpu=native`, medians of
three. Cumulative ablation: each variant strips one piece, so the delta between
neighbours attributes that piece. The variants compute WRONG answers by
construction; only their timings are meaningful.

**Measurement hygiene, and it is not boilerplate.** Every timing here must be
taken on a VERIFIED IDLE machine — `pgrep -c cajeta_test` at zero and a load
average near zero, checked immediately before the run, not assumed from the
last thing anyone said. Measured 2026-08-22: the same binary and the same code
timed **23.03 ms idle and 38.49 ms against a running sweep**, a 67% swing that
looks exactly like a regression. A single contaminated control nearly reversed
this spec's conclusion about the f16 rewrite. Ratios taken under identical load
are somewhat better than absolutes but still drift, because load varies second
to second. Re-confirm the table above on a verified-idle box before anyone
optimizes against it.

| variant | time | piece removed | cost | share |
|---|---|---|---|---|
| v0 full | 21.30 ms | — | — | — |
| v1 | 19.44 ms | `dmins` loop | 1.86 ms | 9% |
| v2 | 10.91 ms | `scaleMinK4` | **8.53 ms** | **40%** |
| v3 | 9.90 ms | lane-by-lane reduce | 1.01 ms | 5% |
| v4 | 1.91 ms | `f16At` x2 | **7.99 ms** | **38%** |

- **2.1** v4 still performs every weight load, every nibble extraction and
  every `dotAccum` — the whole SIMD body — and runs at **1.91 ms**. Against the
  ~0.24 ms memory floor that is 8x, not the 88x the full kernel sits at.
- **2.2** So the arithmetic is not the problem and never was. Two scalar
  helpers, `scaleMinK4` and `f16At`, are **78%** of the kernel between them.
- **2.3** Both are called twice or once per 256-element block, 65,536 blocks
  per projection. Neither touches the SIMD path.

## 3. `f16At` — 38%, and it is a compiler defect, not a kernel defect

- **3.1** `f16At` reads two bytes and calls `GgufFile.halfBitsToF32`, which is
  an ARITHMETIC IEEE decode: it calls `pow2(exp - 15)`, and `pow2` is a
  **repeated-squaring loop in float64**. Two per block is 131,072 loop-driven
  float64 conversions per projection, to produce what is a bit-shuffle and a
  bitcast.
- **3.2** This is a deliberate workaround, and its own comment says so:
  `Cajeta.bitsToF32` **aborts AOT emission**
  (`specs/aot-bitcast-f32-isel-abort-spec.md`, filed 2026-08-20). The JIT path
  is green; only `--emit=exe/obj` dies with `LLVM ERROR: Cannot select: f32 =
  bitcast`.
- **3.3** That spec has **no INDEX row and no plan** — filed and never
  scheduled. It reads as a niche codegen defect. It is 38% of the engine's hot
  kernel.
- **3.3.1** ROOT CAUSE FOUND 2026-08-22, and it is much smaller than it
  looked. The emitted IR is `bitcast i64 %46 to float` — a 64-to-32 bitcast,
  which is malformed LLVM IR, so isel is right to refuse it. `h << 16` promotes
  to int64 and the `bitsToF32` lowering bitcasts its operand without coercing
  to i32. The AOT/JIT divergence is a red herring: the JIT test passes a
  genuine int32, so the promotion never happens there. The fix is a coercion in
  the lowering, not backend work — which answers §6.1: this does not need its
  own arc.
- **3.4** MEASURED 2026-08-22: `--cpu=native` does NOT fix it. The repro aborts
  identically under a host cpu advertising the full AVX-512 feature set, which
  refutes the CPU dimension of that spec's "target/feature configuration"
  hypothesis and narrows the search.
- **3.5** The same workaround appears in `GgufFile.singleBits` and
  `bench/ParityRun`, so the defect's blast radius is wider than one helper.

### Use cases

- **3.6** When the AOT bitcast defect is fixed, `halfBitsToF32` becomes a
  bit-shuffle plus `Cajeta.bitsToF32`, with no loop and no float64.
- **3.7** When a checkpoint's f16 fields are decoded, the result is bit-exact
  with the arithmetic decode for every finite value — the fixtures already pin
  this, so the change is verifiable rather than trusted.
- **3.8** When the defect cannot be fixed promptly, a loop-free integer
  fallback still removes `pow2`: the exponent rebase is a shift and an add, and
  only the subnormal and infinity cases need branches.

## 4. `scaleMinK4` — 40%

- **4.1** It decodes 12 packed 6-bit fields into two `int32[8]` heap arrays,
  per block, via 12 `Quant.u8` calls and bit twiddling — then the kernel reads
  those arrays back one element at a time inside the chunk loop.
- **4.2** The arrays are the suspect, not the arithmetic: 16 heap-array stores
  and 12 reads per block, each through an array header, none of which the
  vector path needs in memory at all.

### Use cases

- **4.3** When a block's scales are decoded, they land in registers or a
  vector, not in two heap `int32[8]`s that are immediately read back.
- **4.4** When the kernel scales a sub-block, it reads the scale from wherever
  the decode left it, without an intervening array round-trip.
- **4.5** When the same decode serves Q5_K and Q6_K, they share it — the
  ablation was measured on Q4_K, but Q5_K (39.5 ms) and Q6_K (28.3 ms) run the
  same helper and are slower still.
- **4.6** When the decode is rewritten, it stays bit-exact against the existing
  fixtures, which already pin every K-quant format against gguf-py.

## 5. Acceptance

- **5.1** PROGRESS 2026-08-22: Q4_K f32 **14.23 ms** and q8_K **11.19 ms**,
  from 22.90 / 20.98 at the start of the day. End to end, decode is
  **7.83 s/token** from 10.91, i.e. 61.6x -> 44.2x against `llama.cpp -t 1`.
  The 10 ms bar is not met yet; `f16At`'s residual ~4 ms is the next item.
- **5.1.0** The Q4_K mat-vec at 4096x4096 beats **10 ms**, against 21.30 when
  filed —
  removing `f16At` and `scaleMinK4` alone accounts for 16.5 ms of it, so this
  is the conservative half of what the ablation predicts.
- **5.2** Every K-quant kernel stays bit-exact against its existing fixture
  tests. This spec changes no numerics; that is what separates it from the
  q8_K activation work, which does.
- **5.3** The ablation is re-run after the change and published in the same
  form, so the next person sees where the time went rather than being told —
  on a verified-idle machine, per §2's hygiene note, with the idle check shown
  alongside the numbers.
- **5.4** No claim in this spec is repeated without its measurement. §2.3 of
  the previous spec named a cause it had not measured and cost a unit; the
  numbers above are ablations, they are labelled as ablations, and their
  method is stated so they can be checked.

## 6. Open questions

- **6.1** CLOSED 2026-08-22 — it rides this plan. The root cause turned out to
  be a missing operand coercion in the `bitsToF32` lowering (§3.3.1), not
  backend or pipeline work, so it is a small unit rather than its own arc. It
  still blocks `--emit=exe` for any code passing a promoted operand, so it goes
  FIRST: fixing it is what lets `halfBitsToF32` drop `pow2` at all.
- **6.2** CLOSED 2026-08-22 — NEITHER. The cost was the per-byte ACCESS, not
  the arithmetic and not the storage. Twelve `Quant.u8` calls are twelve
  separately bounds-checked byte reads through an array header, ~600 cycles a
  call; one `vload<16>` with constant-index extracts replaced them for a 26%
  (f32) / 34% (q8_K) kernel win.

  Both readings in the original question were tried first and both measured
  WORSE than the code they replaced:

  | attempt | Q4_K f32 |
  |---|---|
  | decode each field on demand, no arrays | 27.96 ms |
  | decode once into 16 named locals, `h` unrolled | 27.07 ms |
  | unchanged | 19.29 ms |
  | **one vector load, arrays kept** | **14.23 ms** |

  Removing the heap `int32[8]` made it worse both ways: register pressure
  exceeds what the round-trip costs, so the array is effectively a cheap spill
  slot. The lesson generalises past this helper — a hot loop reading bytes one
  at a time out of an `int8[]` pays a bounds check and a header indirection per
  byte, and that is the thing to remove, not the buffer it writes into.
- **6.2.1** The 40% attribution was re-confirmed by a cleaner ablation than §2's:
  HOISTING `scaleMinK4` to once per row rather than substituting constants,
  which changes only call frequency and cannot unlock constant folding.
  11.87 ms hoisted against 20.01 normal → ~8.68 ms full cost, against the
  original table's 8.53. Two independent methods agreeing, so the
  constant-substitution worry was unfounded — but the hoist is the better
  instrument and should be the pattern for the rest of this spec.
- **6.3** `--minimal` SIGSEGVs on the matvec probe (stack-looking fault
  address). Unrelated to this spec, found beside it, and filed here so it is
  not lost.
