# Spec: ThinLTO backend must codegen with the host CPU features

## 1. Definition

### 1.1 Purpose
A `--lto=thin` release build must produce native-ISA machine code (AVX-512 / AVX2 / FMA
on a capable host) for SIMD-heavy code, matching the non-LTO `-O3 -march=native` build —
so ThinLTO's cross-module inlining is gained without losing per-function vectorization.

### 1.2 Problem
`--cpu=native` is applied only to this compiler's `TargetMachine` (used directly for
non-LTO codegen). The ThinLTO backend runs inside `ld.lld`, which builds its **own**
TargetMachine and reads the target ISA from each function's `target-cpu` /
`target-features` **attributes** (the way clang emits them). cajeta never stamped those
attributes, so the ThinLTO backend fell back to the generic x86-64 baseline (SSE2) and
codegened the SIMD hot loops (e.g. `XXHash3.hashLong`) without wide vectors — a ~3×
throughput regression (xxhash3 23µs vs 7µs). The non-LTO path was unaffected (it
codegens directly through the native TargetMachine).

### 1.3 Scope
- `src/cajeta/compile/Compiler.cpp` — the ThinLTO bitcode-emit path: stamp
  `target-cpu`/`target-features` on every defined function before writing bitcode; pass
  `--lto-O3` so the backend optimizes at O3 (lld defaults to O2).

### 1.4 Non-goals
- 1.4.1 Non-LTO / JIT codegen (already correct).
- 1.4.2 Cross-compilation / non-native `--cpu` values (the same mechanism carries
  whatever cpu/features the TargetMachine was built with).

## 2. Feature: native-ISA ThinLTO codegen

### 2.1 Use cases
- 2.1.1 As a developer, a `--lto=thin` build on an AVX-512 host vectorizes `XXHash3.hashLong`
  with AVX-512 — xxhash3 throughput matches the non-LTO build (~7µs / ≥120 GB/s on 1 MiB).
- 2.1.2 As a developer, other SIMD/native-sensitive benchmarks (matmul, sort, md5) are at
  least as fast under ThinLTO as non-LTO.
- 2.1.3 As a developer, ThinLTO's cross-module inlining wins are retained (arraylist-append
  stays ~the ThinLTO number, not the slower non-LTO one).
- 2.1.4 As an existing build, non-LTO output is unchanged.

## 3. Acceptance themes
- 3.1 Correctness/perf: 2.1.1-2.1.3 hold (verified via the profile suite: xxhash3 back to
  ~7µs, arraylist-append still ~187µs).
- 3.2 Non-regression: every benchmark is ≥ its prior speed; no output/correctness change.
