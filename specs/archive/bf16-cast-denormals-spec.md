# bf16-cast-denormals — `(bfloat16) x` keeps subnormals on every x86 CPU

> Status: **active**, approved by Julian 2026-11-18 ("fix now, own spec"), found by
> coop-converting-store's bfloat16 value tests on proton. Plan:
> [bf16-cast-denormals](../../agents/archive/bf16-cast-denormals-plan.md).

## 1. Definition

The scalar cast `(bfloat16) x` of a `float32` rounds to nearest even and keeps
subnormal results, as every IEEE narrowing does. The GPU backends do this. On an x86 CPU
with AVX512-BF16 (proton's Zen 5), the host and the cpu backend do not. LLVM selects
`vcvtneps2bf16` for `fptrunc float to bfloat`, and that instruction treats a subnormal
input as zero. A host compiled for such a CPU, or a cpu kernel built for it, turns every
float32 denormal into a signed zero.

Measured on proton (cajeta 5533cefa2, 2026-11-18):

| x | bits | expected `(bfloat16) x` | host and cpu backend | hip and vulkan |
|---|---|---|---|---|
| 1.0e-40 | 0x000116C2 | 0x0001 | 0x0000 | 0x0001 |
| -1.0e-40 | 0x800116C2 | 0x8001 | 0x8000 | 0x8001 |
| 1.0e-39 | 0x000AE398 | 0x000B | 0x0000 | 0x000B |

Phoenix has no AVX512-BF16, so LLVM's generic expansion runs there and is correct. The
result of the cast depends on the build machine's CPU, which the language never allows,
and the coop-converting-store bf16 value tests compute their host oracle with this cast.

Decisions (Julian, 2026-11-18, reviewed interactively):

1. Fix it now, under this spec.
2. The narrowing is integer round-to-nearest-even on every CPU, the GPUs' sequence. No
   hardware convert with a denormal fixup: one form, one test.
3. `@FastMath` code may keep the hardware convert, as the XPU contract lets it flush.
4. float64 to bf16 is measured first: Unit 1's tests carry float64 cases that a double
   rounding gets wrong, and the fix covers float64 only if they fail.
5. Performance is read and reported, not gated: cajeta-llm's cpu-leg bf16 timings
   before and after, on proton. A large regression comes back to Julian.
6. The fix is in the cajeta-llvm fork's x86 backend, released as r13. LLVM chooses
   `vcvtneps2bf16` (and AVX-NE-CONVERT's form, which flushes the same way) in two
   places, `LowerFP_ROUND` for vectors and `LowerFP_TO_BF16` for scalars, whenever the
   CPU has the feature. Upstream treats the flush as intended: llvm-project PR #90425,
   a `denormal-fp-math-bf16` setting, has been open since 2024. In the fork, both
   places use the instruction only when flushing is allowed: the conversion carries
   the approximate-functions (`afn`) flag, which `@FastMath` code sets, or the
   function's float32 denormal mode is not IEEE. Otherwise they expand to the exact
   integer form. Because LLVM's own bf16 arithmetic rounds back through the same
   lowering, this covers conversions the compiler inserts during instruction
   selection, on every path, including library bitcode at the consumer's build.
   Chosen over a late IR pass and over fixing each emission site (both hook many
   places and miss LLVM's own conversions), and over turning the CPU feature off
   (which takes the fast convert from `@FastMath`). The cost is a fork release and a
   full rebuild on every machine and CI image.
7. The fork's own tests check the assembly (llc at an AVX512-BF16 CPU); Phoenix checks
   cajeta's assembly at `-mcpu=znver5` and the values at its own CPU; proton runs the
   value tests where the instruction exists.
8. It ships in coop-converting-store's release (that plan's Unit 4).

## 2. Features

### 2.1 The cast is the same on every CPU

On x86, every float32-to-bf16 narrowing that reaches instruction selection, scalar or
vector, whether written in the source or inserted by LLVM, becomes integer
round-to-nearest-even unless flushing is allowed (decision 6). That is the sequence the
GPU backends already emit:
`(bits + 0x7FFF + ((bits >> 16) & 1)) >> 16`, and a NaN gives its top half with the
quiet bit set. It happens in LLVM's x86 backend, so every source is covered: the
language cast, `Vector.toBF16`, the stdlib, a coop tile's narrowing store, and bf16
arithmetic. A `@FastMath` kernel
may keep the hardware convert (decision 3). A `float64` source is measured first
(decision 4): if LLVM narrows it through float32 and rounds twice, it narrows once,
by the same rule.

Use cases:
1. On proton, `(bfloat16) x` on the host and in a cpu kernel gives 0x0001, 0x8001 and
   0x000B for the inputs above, and `Vector.toBF16` gives the same lanes.
2. Values that round, overflow to infinity, are NaN, or are zero keep the results they
   have today on every CPU.
3. coop-converting-store's bfloat16 value tests pass on proton.
4. A cpu kernel that narrows to bf16 in a hot loop costs a few integer instructions a
   lane. The cajeta-llm cpu leg's bf16 timings are read before and after and reported
   (decision 5).
