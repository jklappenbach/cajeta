# quaternion-vector-stdlib — Vector and Quaternion as cajeta.math stdlib classes

## 1. Definition

### 1.1 Purpose
Move `Vector<T,N>` and `Quaternion<T>` from compiler-synthesized C++ types to
declared `cajeta.math` stdlib classes, with implementation bodies written in
cajeta wherever the language can express them. The goal is twofold: the math
surface becomes language-hosted and self-documenting (cajetadoc from source,
go-to-definition lands on `.cajeta`), and the migration proves the pattern —
a register-resident SIMD value type whose operator bodies are real cajeta
code, compiled once for the host and inlined `@Device` into kernels.

**Acceptance is benchmark-gated by developer review.** The current
intrinsic path is measured before any porting work, the stdlib path after;
both result sets are committed and presented side by side. The developer
reviews the numbers and decides. If the stdlib path measurably degrades
performance, the work does not ship: the C++ implementation stays
authoritative and this spec archives with the measured results as the
record. No pass/fail threshold is encoded — the review is the gate.

### 1.2 Background
Current state:
- `CajetaVector` + `VectorOps.h` (~490 lines C++): type synthesis to LLVM
  `<N x T>` plus IR emission for lane arithmetic, extract/insert, splat,
  masks, `compressStore`, `dot`/`idotWiden`, `min`/`max`, `swizzle`, and the
  geometry helpers (`length`, `normalize`, `clamp`, `lerp`, `cross`,
  `reflect`, `distance`, `refract`).
- `CajetaQuaternion` + `QuaternionOps.h` (~270 lines C++): `<4 x T>`
  synthesis plus `conjugate`, `multiply` (Hamilton), `rotate`, `nlerp`,
  `slerp`, with `dot`/`length`/`normalize` reused from vecops.
- Both resolve by bare name with no import (interception in
  `CajetaType.cpp`, `NewExpression.cpp`) and lower in host expression codegen
  and `KernelLowering.cpp`.

Shipped precedents this work composes:
- `Matrix.cajeta` — stdlib-declared operator surface, intrinsic codegen (the
  hybrid pattern; bodies are resolution placeholders).
- `SoftwareRayQuery.cajeta` — real cajeta bodies compiled `@Device` and
  inlined into kernels on all four backends.
- `v[i]` lane read/write, `Vector` elementwise operators, and `Math.*`
  transcendentals all work in cajeta source, host and device.

### 1.3 The core/composite split
Vector's core ops are single LLVM instructions or intrinsics (lane
arithmetic, extract/insert, splat, shuffle, masks, reductions). There is no
lower-level cajeta substrate to express them in; they remain intrinsic
codegen behind the declared surface (Matrix-style hybrid). Everything that
is a *composition* gets a real cajeta body:
- Vector composite ops: `length`, `normalize`, `clamp`, `lerp`, `cross`,
  `reflect`, `distance`, `refract`.
- All Quaternion ops: they are compositions over `Vector<T,4>`.

### 1.4 Constraints
1. **Zero source breakage.** Bare-name `Vector` / `Quaternion` keep
   resolving without import; every existing test, tour demo, and benchmark
   compiles unchanged.
2. **One body, both worlds.** Cajeta bodies are `@Host @Device`; the same
   source serves host codegen and all four XPU backends.
3. **Parity is proven, not assumed.** Golden-IR tests where the emitted IR
   should be identical; the benchmark gate (§5) where it need only be
   equivalent.
4. **The intrinsic path stays intact until the gate passes.** Lowering is
   selected by a compiler flag (`--math-lowering=intrinsic|stdlib`),
   default `intrinsic`, so A/B benchmarking compares the same compiler
   build. C++ deletion happens only after the gate passes and the default
   flips.

### 1.5 Non-goals
1. Real cajeta bodies for `Matrix<T,R,C>` — blocked on method-templated
   operators (shape-generic `matmul`); separate follow-on.
2. New math surface. The op set is exactly what ships today.
3. `Tensor`, dynamic `cajeta.math.Matrix`, or any heap-backed math type.
4. Changing numeric semantics (lane order, rounding, fast-math flags).

## 2. Declared stdlib surfaces

`Vector.cajeta` and `Quaternion.cajeta` in `cajeta.math`, following the
`Matrix.cajeta` declaration pattern: the full documented op surface with
template bounds (`T` satisfies `Floating` for Quaternion; non-bool numeric
for Vector), operator declarations per `OperatorOverloading.md`.

Use cases:
- 2.1 As a developer, when I run cajetadoc over `cajeta.math`, then `Vector`
  and `Quaternion` appear with their full op surfaces documented from source.
- 2.2 As a developer, when I compile existing source using bare
  `Vector<float32,16>` / `Quaternion<float32>` with no import, then it
  compiles and behaves exactly as before.
- 2.3 As a developer, when I write `import cajeta.math.Vector;` explicitly,
  then it resolves to the same type.
- 2.4 As an IDE user, when I go-to-definition on `Quaternion`, then I land in
  `runtime/src/cajeta/math/Quaternion.cajeta`, not a diagnostic.

## 3. Flat value-class layout

The enabling compiler capability: a value class whose only field is a
`Vector<T,N>` lowers to the flat `<N x T>` — an SSA register value, no
aggregate wrapper, no memory round-trip — in host codegen and kernel
lowering alike.

Use cases:
- 3.1 As the compiler, when a `Quaternion<T>` local is declared, then its
  LLVM type is `<4 x T>` (verified by golden IR), identical to today.
- 3.2 As a kernel author, when a `Quaternion<T>` is a kernel local or
  `@Device` helper parameter, then it is the same flat `<4 x T>` on every
  backend.
- 3.3 As a developer, when I hold an array of `Quaternion<T>`, then element
  layout and stride are unchanged from today.

## 4. Real cajeta bodies

### 4.1 Vector composite ops
`length`, `normalize`, `clamp`, `lerp`, `cross`, `reflect`, `distance`,
`refract` implemented in cajeta over the intrinsic core (lane ops, `dot`,
`min`/`max`, `Math.sqrt`), marked `@Host @Device`.

### 4.2 Quaternion ops
`conjugate`, `multiply`, `rotate`, `dot`, `length`, `normalize`, `nlerp`,
`slerp` implemented in cajeta over `Vector<T,4>` (`slerp` via `Math.acos` /
`Math.sin`), marked `@Host @Device`.

Use cases:
- 4.3 As a developer, when I run the existing Quaternion and Vector test
  suites under `--math-lowering=stdlib`, then every test passes with
  numerically identical results.
- 4.4 As a kernel author, when a kernel uses `q.rotate(v)` under the stdlib
  lowering, then the body inlines on CPU, Vulkan, AMD, and NVPTX backends
  (existing device tests pass under the flag).
- 4.5 As the compiler, when host golden-IR tests run for `conjugate` and
  `multiply` under both lowerings, then the stdlib IR is equivalent
  post-optimization (identical where the test asserts exact form).

## 5. Benchmark gate

The acceptance authority. The gate produces **numbers for developer
review**; it does not encode a pass/fail threshold.

### 5.1 Method
- Benchmark programs in `bench/src/bench/math/`, compiled native `--release`,
  once per lowering (`intrinsic` vs `stdlib`) with the same compiler build.
- Each benchmark reports ns/op; score = median of ≥10 runs on the same
  machine, no other load.
- The intrinsic baseline — the libraries as they perform today — is captured
  and committed **before any porting work lands** (JSON alongside the bench
  sources). The stdlib measurement is taken after the port, same machine,
  same method.

### 5.2 Workloads
1. Quaternion composition chain (dependent Hamilton products).
2. Batch rotate: apply a quaternion to 1M vec3s.
3. `slerp` sweep (transcendental-heavy).
4. Vector geometry loop: `normalize` / `cross` / `reflect` / `distance`
   over 1M elements.
5. One XPU dispatch per available backend exercising quaternion + vector
   composite ops in-kernel (wall-clock per dispatch).

### 5.3 Review
Both result sets are presented side by side (per-benchmark delta and
geomean computed for convenience, not judgment). The developer reviews and
decides the outcome. "Measurably degrade" is the developer's call on these
numbers.

### 5.4 Outcomes
- 5.4.1 **Accepted** → default lowering flips to `stdlib`; the C++ composite
  emitters (QuaternionOps entirely; the composite half of VectorOps) are
  deleted; the flag is removed after one release; docs updated (§6).
- 5.4.2 **Rejected (perf degraded)** → we stick with C++: default stays
  `intrinsic`, and the measured results are recorded in this spec before it
  archives. Disposition of the stdlib bodies, the flag, and the declared
  surfaces (§2, which carry no codegen cost) is decided in the same review.

## 6. Documentation

- `docs/specification/README.md`: collapse the "compiler-defined" note for
  Vector/Quaternion to the Matrix-style wording (declared surface), or to
  plain stdlib wording on a §5 pass.
- `docs/specification/math/Quaternions.md` and `Simd.md`: status
  lines updated to reflect where the implementation lives.
- cajetadoc output covers `cajeta.math.Vector` / `Quaternion` (use case 2.1).
