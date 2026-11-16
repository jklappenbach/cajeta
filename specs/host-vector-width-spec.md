# host-vector-width — the vector register width host code is built for

> Status: **active**, approved by Julian 2026-11-15 ("Add a host vector-width
> query"; decision below). Plan: [host-simd](../agents/host-simd-plan.md).

## 1. Definition

`Cajeta.hostVectorBits()` answers, as an `int32`, the width in bits of the
fixed-width vector registers the running code was compiled for: 128 for SSE or
NEON, 256 for AVX2, and 512 for AVX-512. Host code that holds several
`Vector<T,N>` accumulators sizes its register tile from it. Four 16-lane float32
accumulators fill 8 of AVX2's 16 ymm registers, but only 4 of AVX-512's 32 zmm.
That is too few to cover the fused multiply-add latency on two pipes
(Proton's read of cajeta-ml's HostGemm on Zen 5, 2026-11-15).

Decision (Julian, 2026-11-15): the query reports the width the code is compiled
for, not the running CPU's. A tile's register budget depends on what its loop
was lowered to. A generic build on an AVX-512 machine lowers to 256-bit
registers, and a tile sized for 512 would spill. This is the same "reported
against built" trap the cpu backend's wave probe hit (2026-10-06).

So the answer is fixed when the final module is generated for its target, not
when a library is compiled. A `.cja` ships bitcode that its consumer compiles,
so the query stays unresolved in an archive and resolves in the build that
emits code: the executable's object, or the JIT's module, for the host.

## 2. Use cases

- **2.1** As host code, when I call `Cajeta.hostVectorBits()` in an executable
  built with `--cpu=native` on an AVX-512 machine, I get 512. Built with
  `--cpu=x86-64-v3` on the same machine, I get 256.
- **2.1a** As host code built with `--cpu=x86-64-v4`, I get 256, even though
  the target has AVX-512. LLVM's generic tuning for that level prefers 256-bit
  vectors, so 256 is the width its loops are lowered to, and the answer follows
  the build rather than the instruction set. Measured on proton's Zen 5
  (2026-11-15): native and jit-run read 512, x86-64-v4 and v3 read 256, and v2
  reads 128.
- **2.2** As host code in a library, the answer is the consuming build's, not
  the library's, because the archive keeps the query unresolved.
- **2.3** As host code under the JIT, I get the host's width, which is what the
  JIT compiles for.
- **2.4** As an optimizer, the answer is a constant after the query resolves,
  so a branch on it folds and the unused tile shape is dropped.

## 3. Acceptance

- A JIT test asserts the host answer matches LLVM's TargetTransformInfo
  fixed-width register width for the host, and that the call folds to a
  constant in the optimized IR.
- An archive test asserts the declaration survives `--emit=cja` unresolved.
- `docs/specification/03-types-values-variables.md` (the Vector section)
  documents the query beside `vload` and `vstore`.
