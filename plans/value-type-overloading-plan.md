# Value-type operator overloading — mechanism + framework

> **COMPLETE (2026-06-05).** Stages S0–S9 are all DONE; `@ValueType` / `VALUE_TYPE_FLAG`
> + the operator-dispatch gate + the by-value ABI are live in the compiler (latest fix:
> "@ValueType instance-method call"). Retained as the mechanism + decision record
> (incl. Decision #4 / S7 — templated operators retired).

## Context

cajeta wants the standard library (Toffee, math types) to define **fast value types whose
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
and frameworks every value type in [`docs/ValueTypeCatalog.md`](../docs/ValueTypeCatalog.md).

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

- **S0 — Golden-IR baseline (no source change). DONE 2026-06-04.** Regression oracle for the
  Vector retrofit, both sides:
  - **Device:** `XpuVectorDeviceTests.lowersToVectorIr` asserts `<4 x float>` / `fmul` /
    `extractelement` / `insertelement` in the kernel IR; `runsOnCpu`/`runsOn{Vulkan,Amd}Device`
    are the behavioral oracle.
  - **Host:** added the host-IR-string accessor (`CajetaJit::Options::captureIr` →
    `getModuleIr()`, capturing post-codegen pre-opt IR at the same point `CAJETA_DUMP_IR` prints)
    and `VectorHostGoldenIrTests` — `lowersToFlatVectorIntrinsics` mirrors the device assertions
    on the host JIT, and `vectorOperatorsAreNotDispatchedAsCalls` pins the defining invariant
    (Vector `+`/`*` fold to flat `<N x T>` SSA via intrinsic interception, NO
    `Vector<…>::operator` call). The S9 retrofit must keep both green.
- **S1 — `VALUE_TYPE_FLAG` + `@ValueType` + POD validator.** Flag bit; visitor recognition;
  POD-validity check (reject base/interface/virtual/non-POD-field; **recurse into value-type
  fields**). Negative + positive tests. Files: `CajetaType.h`, `CajetaLlvmVisitor.h`,
  `CajetaClass.cpp` (~583).
- **S1b — Inliner-pass fix (REQUIRED FIX #1).** Add `AlwaysInlinerPass` to the JIT pipeline and
  the AOT-O0 path; test that an `AlwaysInline` function is folded at O0/JIT. Files:
  `JitTestHelper.cpp` / the JIT driver, `Optimizer.cpp`. **Prerequisite for S4/S5.**
- **S2 — Value-type by-value calling convention + Copy borrow exemptions (DONE 2026-06-04).**
  **Finding (2026-06-04): the dispatch gate is NOT the blocker.** A `@ValueType` class carries
  `VALUE_TYPE_FLAG` but not `PRIMITIVE_FLAG`, so it already passes the existing
  `!(PRIMITIVE_FLAG)` gate like any non-primitive class — operators *resolve*. The real S2 work
  is the **value-type by-value ABI + treating value types as Copy** in the borrow/ownership
  checker. The flag must also be applied inside `CajetaClass::generatePrototype` (after its
  `typeFlags` reset, before method prototypes) or the operator borrow check runs before the
  flag is set. **Done so far (uncommitted):** (a) `CajetaClass.cpp:583` re-applies
  `VALUE_TYPE_FLAG` after the reset; (b) `Method.cpp` multi-param-borrow-return rule exempts
  value-type returns (Copy, not a borrow); (c) `Statement.cpp:1303` fresh-allocation-return
  (`return stack Vec2(...)`) exempts value types (no `#` needed); (d) `Method.cpp:526`
  return-signature is by-value (the aggregate) for value types, not by-pointer.

  **DONE 2026-06-04 — all 4 `ValueTypeOperatorHostTests` green** (`+`, `-`/`* scalar`, `==`/`!=`,
  `<`/`>`/`<=`/`>=`), via the genuine by-value calling convention (NOT an inlining crutch — the
  operators are real cross-function calls passing/returning `Vec2` aggregates by value). 200+
  regression tests across operator/vector/view/interface/multiclass/template/ctor/dtor/archive
  suites stay green. Landed:
  1. **`BY_VALUE_FLAG` (storage axis) + born-correct archive (flag-identity root cause).** A new
     `BY_VALUE_FLAG` bit (`CajetaType.h`) names the storage axis explicitly: set on `@ValueType`
     classes alongside `VALUE_TYPE_FLAG` (the "kind"); `hasValueSemantics()` is now a direct
     two-bit test (`PRIMITIVE_FLAG | BY_VALUE_FLAG`), no canonical-map backstop in the hot path.
     The stale-instance problem (canonical gets the flag in `generatePrototype`, parse-time
     placeholders don't) is fixed AT THE SOURCE by `markArchiveValueType`/`isArchiveValueType`
     (`CajetaType.cpp`) — the prescan (`Compiler.cpp`, `classHasValueTypeAnnotation`) detects
     `@ValueType` and the placeholder-synthesis path sets `VALUE_TYPE_FLAG | BY_VALUE_FLAG` from
     birth, exactly mirroring the proven `markArchiveEnum`/`isArchiveEnum` mechanism. `isValueType()`
     keeps a cheap canonical-map fallback as belt-and-suspenders. The flag is also re-applied in
     `CajetaClass::generatePrototype` after its `typeFlags` reset.
  2. **By-value parameter ABI.** `Method.cpp` (both signature paths) and `ParameterField.cpp`
     exempt `@ValueType` params from pass-by-pointer: the LLVM signature carries the aggregate,
     the call site loads the inline slot, and the callee spills the by-value arg to an
     aggregate-sized slot (was a `ptr`-sized slot — a by-value store overflowed it, corrupting
     the adjacent operand → the original "garbage value" symptom). Return signature was already
     by-value.
  3. **Field-access on inline value-type slots.** `DotExpression.cpp` GEPs a value-type local's
     alloca DIRECTLY (the slot holds the aggregate inline) instead of loading through it — gated
     on `isValueType() && !allocatedType->isPointerTy()`, so a value-type METHOD's `this` (a
     pointer spilled to a `ptr` slot — receiver is still by-reference) correctly loads through.
  4. **Drop-chain bypass.** `LocalVariableDeclaration.cpp` skips drop-entry registration for
     `@ValueType` locals — they are Copy PODs, never heap-backed, no destructor.

  **Also landed 2026-06-04:**
  - **Vtable-free POD layout.** `%test.Vec2` is now `{ float x, float y }` (8 bytes, fields at
    index 0/1) — true register-friendly POD, no vtable slot. `hasVtablePointerAtSlotZero()` returns
    `!isValueType()` (mirrors the `CajetaTask` precedent); the layout builder (`embedSubObject`),
    `getFieldLlvmIndex`, and the construction-time vtable store (`CreatorRest.cpp`) all route through
    it. Construction memsets 8 bytes with no vtable write; the (now unused) `#VTable`/RTTI globals are
    still emitted but never stored into an instance — harmless, left for reflection.
  - **`AlwaysInline` on value-type operators (S4 core).** `Method::generatePrototype` marks every
    operator overload declared on an `@ValueType` class `alwaysinline`, so the O0 `AlwaysInlinerPass`
    folds a dispatched value-type operator to the same flat, register-resident IR an intrinsic would —
    host and device. (Size guard for an oversized operator body remains a future S4 refinement.)
- **S3 — `operator[]` read gate + mutating-operator policy (DONE 2026-06-05).** Read-`operator[]`
  already dispatched through the operator mechanism; locked Decision #3 enforced at DECLARATION
  time — a `@ValueType` class declaring `operator++`/`--`/`[]=`/compound-assign is a compile error
  (`CAJETA_ERROR_VALUE_TYPE_MUTATING_OPERATOR`, `CajetaLlvmVisitor.h`). Closing the read path on a
  value-type receiver surfaced two vtable-free-POD fallouts the static operators never hit, both
  fixed: the receiver passes by ADDRESS not loaded aggregate (`Expression.cpp`), and instance
  methods on vtable-free types dispatch DIRECTLY (`useVtable` now requires
  `hasVtablePointerAtSlotZero()`, `CajetaClass.cpp` — else a slot-0 vtable load reads a field and
  segfaults); plus scalar-return-from-lvalue coercion (`Statement.cpp`). Tests:
  `ValueTypeIndexOperatorTests` (read + two rejections). 133-test dispatch/inheritance regression green.
- **S4 — Guaranteed inlining (DONE 2026-06-05).** `AlwaysInline` marking landed earlier;
  `Method::generateCode` now drops it from an operator whose emitted body exceeds ~100 IR
  instructions (size guard) so a large operator keeps a real call. `captureIr` snapshots
  post-AlwaysInline IR so tests can see the fold. Tests `ValueTypeInlineSizeTests` (small folds,
  large keeps a call).
- **S5 — POD marshalling incl. nested (DONE 2026-06-05).** `isPodStruct` recurses into
  `VALUE_TYPE_FLAG` fields (`KernelArgTrait.cpp`), so a `@ValueType` POD and a struct containing
  value-type fields are kernel-arg admissible by value. `deviceStructInfo` builds nested device
  struct types for value-type fields + records their sub-field map; `structFieldRead` resolves a
  nested `param.vfield.subfield` read to a multi-index `extractvalue` (`KernelLowering.cpp`). Tests
  `XpuValueTypeArgDeviceTests` (flat + nested field read + admissibility); 20 XPU tests green.
- **S6 — Shared dispatch/derivation helper (DONE 2026-06-05).** New
  `src/cajeta/asn/expression/OperatorDispatch.h`: the op→symbol map, the `!=/>/>=/<=` derivation
  table, and `dispatchBinaryOperator()` (tries the direct operator then its derivation via two
  caller-supplied callbacks — resolve+invoke, negate). `BinaryOpExpression` re-pointed at it;
  behavior unchanged (53 operator/comparison/value-type tests green). This is the seam S8 reuses.
- **S7 — RETIRED (owner decision #4, 2026-06-04). NOT a workstream.** Method-templated operators
  are rejected: operator overloads are concrete (a specific LHS type × a specific RHS type), so
  there is nothing to templatize. `*` **is matrix multiply**, lowered as a **compiler intrinsic** —
  the compiler-known value type lowers `M1 * M2` to matmul by reading `R/K/C` straight off the
  (already concrete) operand types in codegen, exactly like `Vector`'s operators; it does NOT go
  through the user overloading mechanism. No templates, inference, or monomorphization. Element-wise
  multiply is the method `a.hadamard(b)`. See Decision #4 below and the retired sub-plan
  `plans/method-templated-operators-plan.md` (kept only as the record of why this was rejected).
- **S8 — Device path (DONE 2026-06-05; aggregate-returning + spirv-val closed same day).**
  `lowerBinaryOp` routes a `@ValueType` LHS through the S6 helper
  (`opdispatch::dispatchBinaryOperator`) with device callbacks: resolve the operator, require
  `@Device`, lower it via `lowerDeviceFn` (aggregate-param ABI already works — S5's
  `deviceStructInfo` + the existing `alwaysinline` helper path), emit the call the backend inliner
  folds; comparison derivations reuse the shared table. Kernel bodies aren't host-type-resolved, so
  operand value types come from a `valueTypeNames` (param/local → type) map; a whole value-type
  param reads as its materialized aggregate SSA (extractvalue-only, SPIR-V-safe). A value-type LHS
  with no `@Device` operator now errors cleanly (was an ICmp-on-aggregate crash). Tests
  `XpuValueTypeOperatorDeviceTests` (device `==` dispatch + derived `!=`).
  **Aggregate-returning (DONE 2026-06-05):** a value-type-RETURNING `@Device` operator (`Vec2
  operator+`) now lowers — `new Vec2(...)` builds the result as an SSA aggregate (`insertvalue`
  chain into `deviceStructInfo`'s struct, `lowerNewValueType`), `lowerDeviceFn` returns the device
  struct by value, and a value-type kernel local holds the aggregate SSA (no alloca, registered in
  `structValues`/`structFields`/`valueTypeNames`; its own class registered in `valueTypeCtors` so a
  kernel can build value types directly). **spirv-val (DONE 2026-06-05):** a kernel that constructs
  Vec2s, dispatches `operator+`, and reads the result emits a Vulkan module `spirv-val` accepts
  (`deviceOperatorKernelValidatesAsSpirv`) — value types stay register-resident OpCompositeInsert/
  Extract aggregates, no pointer-to-aggregate in Function storage. **Still deferred:** scalar-RHS
  operators (`Vec2 * float`) need RHS-type tracking in the device lowerer.
- **S9 — Vector signature surface + docs (DONE 2026-06-05).** `ValueTypeCatalog.md` already
  carried the Vector entry + operator-signature conventions; reconciled the Vector entry with the
  implemented mechanism — its operators are compiler intrinsics (signatures documentary), Vector is
  the register-residency reference pinned by the S0 oracle, its `operator[]`/`[]=` are exempt from
  the S3 mutating ban (not a `@ValueType` CajetaClass), and declared-signature synthesis stays
  deferred (documentary-first, Decision #5). S0 golden re-run green.

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
