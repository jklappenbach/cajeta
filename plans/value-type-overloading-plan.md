# Value-type operator overloading — mechanism + framework

## Context

cajeta wants the standard library (Prism, math types) to define **fast value types whose
operators go through real operator overloading** — Pythonic sugar with GPU-native codegen.
Today that is impossible: the operator-dispatch gate
(`BinaryOpExpression.cpp:658-659`, mirrored at the `operator[]` gate `Expression.cpp:532`
and `operator[]=` gate `BinaryOpExpression.cpp:548`) runs **only** when the LHS is a
`CajetaClass` **and** lacks `PRIMITIVE_FLAG`. `Vector<T,N>` is excluded on both counts (it
is a bare `CajetaVector`, and it is `PRIMITIVE_FLAG`), so its operators are compiler
intrinsics — not overloads. The same gate is why a hybrid `Matrix` would have to intercept
rather than dispatch.

This effort changes the mechanism **once** so a value-type category can dispatch through
overloading while preserving the 5 GPU gains (by-value marshalling, register residency,
SIMD/WMMA selection, guaranteed inlining, device-lowerer simplicity). It fixes `Vector`
(real operator surface, zero IR change), unblocks `Matrix` as a genuine dispatch consumer,
and frameworks every value type in [`cajeta-docs/ValueTypeCatalog.md`](../cajeta-docs/ValueTypeCatalog.md).

Built from a workflow audit + design + **adversarial review** (verdict: *sound-with-fixes*).
The review's required fixes are folded into the stages below.

## The model — **host dispatches, device intercepts** (stated honestly)

- A **value type** is a `CajetaClass` carrying a new additive **`VALUE_TYPE_FLAG`** (NOT
  `PRIMITIVE_FLAG`, which is an exclusive scalar/vector/array/pointer invariant wired into
  width/marshalling math). It is declared `@ValueType`, validated POD (no base class, no
  interfaces, no virtuals; fields only scalar / `Vector` / other value types). It keeps an
  ordinary vtable-stripped **struct layout** — **no `FixedVectorType` override** (that is
  `Vector`'s special case); the existing POD kernel-arg path marshals it by value unchanged.
- **Host:** relax the gate so value types dispatch through the *existing*
  `resolveMethod`/`invokeMethod` path (which the audit confirmed is already
  representation-agnostic). Operator methods are **force-inlined** so the resolved call
  becomes the same flat IR an intrinsic would.
- **Device:** the `DeviceLowerer` **intercepts** value-type operators in `lowerBinaryOp`
  and force-inlines the (`@Device`, pure) operator body via the backend `AlwaysInlinerPass`.
  The device path does **not** reuse host dispatch verbatim — it shares only the
  resolution/derivation helper. This split is real; do not pretend parity.
- **Vector retrofit:** keep `CajetaVector`'s intrinsic codegen as the implementation; layer
  a *declared operator-signature surface* on top for docs/tooling/extensibility. The gate
  still **misses** `CajetaVector` (not a `CajetaClass`), so emitted IR is **byte-identical** —
  proven by a golden-IR/SPIR-V diff (Stage 0).

## Design

- **`VALUE_TYPE_FLAG`** — new bit `0b10000000000000000000` in `CajetaType.h` (the next free
  bit above `VECTOR_FLAG`; confirm `BIT_SIZE_MASK` and `*_TYPE_ID` composites don't alias it).
- **`@ValueType`** annotation (not a marker interface — no stdlib dep, can't be implemented
  accidentally). Visitor ORs the flag into the class `typeFlags` and runs the POD validator.
- **Gate change** (3 sites): `!(PRIMITIVE_FLAG)` → `(!(PRIMITIVE_FLAG) || (VALUE_TYPE_FLAG))`.
  This admits value types while keeping `CajetaArray` (which carries `PRIMITIVE_FLAG` but is
  not a value type) **out**.
- **Force-inline:** mark operator overloads on value-type classes `AlwaysInline` at prototype
  generation (`Method.cpp:678`), gated by an instruction-count size guard (oversized → real
  call, loses guaranteed-inline but stays correct).
- **Marshalling:** value types ride the existing `isPodStruct` by-value path
  (`KernelArgTrait.cpp:86-99`) — **extended to recurse into value-type fields** (they lack
  `PRIMITIVE_FLAG`, so the current field check rejects them).
- **Method-templated operators:** **IN SCOPE (owner decision 2026-06-04 — full relaxation).**
  K-generic `matmul` `a * b` over a shared dim needs a real templated `operator*<uint32 K>`.
  This is a large sub-effort (monomorphization, non-type-param inference from operands,
  code-size) being designed by its own workflow; sequenced as a workstream after the core
  mechanism (S0–S6), which is independent of it.

## Required correctness fixes (from the adversarial review — MUST land)

1. **`AlwaysInline` is a no-op without an inliner pass — CRITICAL.** `optimizeModule`
   (`Optimizer.cpp:45`) early-returns at `O0`, and the JIT (`JitTestHelper.cpp`) builds a bare
   `LLJIT` with no inliner. So `AlwaysInline` operators stay **real calls** on the JIT and
   AOT-O0 paths — defeating gain #4 *and*, via aggregate `byval`/`sret` spills, gain #2
   (register residency). **Fix: add an explicit `AlwaysInlinerPass` to the JIT pipeline and to
   the AOT-O0 path** (device backends already run it — `SpirvBackend.cpp:96`). This is the
   single most important fix; gate everything else behind it.
2. **Instance mutating operators on a by-value Copy receiver mutate a copy and lose the write.**
   `operator[]=`/`++`/`--` need an addressable mutable `this`, but value types are Copy.
   **Fix: forbid instance mutating operators on `@ValueType` receivers** (compile error
   steering to a static returns-new form), *or* pass the receiver by mutable address — pick in
   the open decisions. (Read `operator[]` returning a value is fine; only *writes* are the hole.)
3. **Method-templated static operators are IN SCOPE (owner decision).** Rather than rejecting
   them, this effort will **support** them — `operator*<uint32 K>` monomorphized per concrete
   shape, with non-type-param inference from the operand types. Designed by its own workflow
   (see the method-templated-operators plan); the visitor `final-or-static` check
   (`CajetaLlvmVisitor.h:1139-1158`) is extended, not tightened.
4. **Device aggregate operators are net-new lowering, not reuse.** `lowerDeviceFn`
   (`KernelLowering.cpp:1718-1759`) emits scalar-param/scalar-return functions only; a
   `operator+(Vec2,Vec2)->Vec2` has aggregate params + return. **Fix: re-scope Stage 7 as
   real aggregate-param/return device-function support** (or constrain device value-type
   operators to a lowerable form), and extend `resolveDeviceMethod` to canonical-arg static
   resolution.
5. **Recursive POD validation is a hard prerequisite, not a footnote.** `isPodStruct`
   (`KernelArgTrait.cpp:96`) rejects any non-`PRIMITIVE_FLAG` field; value types carry none, so
   a value-type-of-value-types is rejected today. **Fix: its own stage — accept-and-recurse
   `VALUE_TYPE_FLAG` fields in both `isPodStruct` and `deviceStructInfo`.**

## Stages (each builds `./build.sh` + tests independently; TDD)

- **S0 — Golden-IR baseline (no source change).** Capture post-codegen LLVM IR + SPIR-V for a
  representative `Vector` workload (add/sub/mul/div, dot, length, normalize, index, broadcast)
  as the regression oracle for the retrofit. Files: `test/codegen/`, `test/xpu/`.
- **S1 — `VALUE_TYPE_FLAG` + `@ValueType` + POD validator.** Flag bit; visitor recognition;
  POD-validity check (reject base/interface/virtual/non-POD-field; **recurse into value-type
  fields**). Negative + positive tests. Files: `CajetaType.h`, `CajetaLlvmVisitor.h`,
  `CajetaClass.cpp` (~583).
- **S1b — Inliner-pass fix (REQUIRED FIX #1).** Add `AlwaysInlinerPass` to the JIT pipeline and
  the AOT-O0 path; test that an `AlwaysInline` function is folded at O0/JIT. Files:
  `JitTestHelper.cpp` / the JIT driver, `Optimizer.cpp`. **Prerequisite for S4/S5.**
- **S2 — Host gate (binary + comparison derivations).** Gate clause edit at
  `BinaryOpExpression.cpp:658-659`. `Vec2 @ValueType` with `operator+/-/* (scalar)/==/<`;
  verify dispatch + `!=/>/<=/>=` derivation on JIT; confirm plain classes unaffected and
  `CajetaArray` still excluded (negative test).
- **S3 — `operator[]` read gate + mutating-operator policy (REQUIRED FIX #2).** Open the read
  gate (`Expression.cpp:532`). Decide+enforce instance mutating-operator policy: forbid on
  value types, or by-address receiver. Tests for indexed read + the chosen write policy.
- **S4 — Guaranteed inlining.** `AlwaysInline` on value-type operator prototypes
  (`Method.cpp:678`) + size guard; IR-assert the call folds at O0 (depends on S1b) and that
  an oversized operator keeps a real call.
- **S5 — POD marshalling incl. nested (REQUIRED FIX #5).** Extend `isPodStruct`
  (`KernelArgTrait.cpp:86-99`) + `deviceStructInfo` (`KernelLowering.cpp:145-165`) to
  accept/recurse `VALUE_TYPE_FLAG` fields; marshal a value type (and a value-type-containing
  struct) by value to a field-reading kernel (no operators yet).
- **S6 — Shared dispatch/derivation helper.** Extract operator lookup + comparison derivation
  (`BinaryOpExpression.cpp:696-754`) into a representation-agnostic free function; re-point
  host at it; behavior unchanged. Prerequisite for device.
- **S7 — Method-templated static operators (workstream; owner decision #4).** Support
  templated static operators (`operator*<uint32 K>`) with non-type-param inference + per-shape
  monomorphization, so `a * b` matmul works for arbitrary conforming shapes. Large; designed
  by its own workflow + plan (`plans/method-templated-operators-plan.md`). Independent of
  S0–S6.
- **S8 — Device path (RISK; REQUIRED FIX #4).** In `lowerBinaryOp` (`KernelLowering.cpp:1761`):
  on `VALUE_TYPE_FLAG` LHS, resolve via the S6 helper, lower the `@Device` (pure) operator
  body — **with new aggregate-param/return support in `lowerDeviceFn`** — and emit a call the
  backend `AlwaysInlinerPass` folds; else fall through to native IR. Purity check (no
  alloca/GEP/heap) so SPIR-V logical addressing validates. Re-run S0 golden tests (existing
  kernels byte-identical) + `spirv-val`.
- **S9 — Vector signature surface + docs.** Land the `Vector` catalog entry + the
  operator-signature conventions; optionally synthesize declared `Vector` operator signatures
  backed by the intrinsic interception (gate still bypasses `CajetaVector`). Re-run S0 diff.

## Decisions (locked 2026-06-04)
1. **Device operator policy:** **`@Device`-only, pure** — a kernel-usable value-type operator
   must be `@Device` with a body restricted to primitive scalar/vector ops (no alloca/GEP/heap)
   so it force-inlines to flat SSA and passes SPIR-V validation.
2. **Declaration surface:** **`@ValueType` annotation** (no stdlib dep; can't be applied
   accidentally).
3. **Instance mutating operators on value types:** **forbidden** — value types are by-value
   Copy; mutating-instance operators would mutate a copy. `m[r][c]=v` (whole-value flat-lane
   insert) and `m += n` (desugars to `m = m + n`) still work; only in-place `operator[]=`/
   `++`/`--` on a value receiver are rejected with a clear error.
4. **Method-templated operators:** **REJECTED (owner decision 2026-06-04, reversed).** Operator
   overloads are concrete (a specific LHS type × a specific RHS type); templatizing an operator is
   the wrong shape. So there are NO templated operators anywhere. Consequence: shape-generic
   `Matrix`/`Tensor` **matmul `*` is compiler-intrinsic** — the compiler-known value type lowers
   `M1 * M2` to matmul with `R/K/C` inferred in codegen (the Hybrid interception path), exactly like
   `Vector`'s operators. It does NOT go through the user overloading mechanism. Element-wise
   multiply is the method `a.hadamard(b)`. The `plans/method-templated-operators-plan.md` workstream
   is **retired**. The overloading mechanism still serves user value types' concrete operators and
   `Matrix`'s concrete ones (`+ - ==` `*scalar` `[]`).
5. Defaults (override on review): value types are implicitly **Copy** (required by by-value
   marshalling); `AlwaysInline` size-guard threshold TBD; Vector operator synthesis
   documentary-first; swapped `(scalar, V)` overload must be explicit for now.

## Risks
- Device inlining failure on SPIR-V if an operator body emits non-flat IR → purity check +
  post-opt IR shape assertion + `spirv-val` gate.
- Host/device drift in comparison derivation → the S6 shared helper is the single source.
- Marshalling silently degrading to a struct pointer if any seam misses the recursion → keep
  value types on the existing POD path; nested-field test.
- Code bloat from blanket `AlwaysInline` → size guard, tested.

## Verification
- Build `./build.sh`; tests `CAJETA_SOURCE_ROOT="$PWD" ./build/test/cajeta_test --gtest_filter=...`.
- Golden-IR/SPIR-V diff (S0) is the zero-regression oracle for `Vector`.
- Full regression: existing Vector / template / operator-overload / XPU suites unchanged.
- Device backends gated on hardware (VK/AMD), `spirv-val`-clean.
- **Constraints:** commit when verified (standing permission), brief messages, **no
  attribution trailer**, stage files explicitly; push ask-first.
