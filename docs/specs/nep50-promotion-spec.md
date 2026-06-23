# NEP-50 dtype promotion for `cajeta.math` — Spec

## 1. Definition

### 1.1 Purpose
Define how the `cajeta.math` `Tensor` ufunc surface (numpy-porting-plan Phase 3+)
determines the **element dtype of an operation's result** when operands differ in
dtype, following NumPy's **NEP-50** promotion semantics, realized with cajeta's own
type-system features rather than a numpy-style dynamic runtime.

### 1.2 Scope
- The **promotion rules** (which output dtype a binary/unary ufunc produces).
- The **API surface** that exposes those rules to users: a static typed kernel, a
  same-dtype shorthand, and an auto-promoting convenience.
- The **per-ufunc result/domain rules** (true-division → float, comparisons → bool,
  bitwise → integral, transcendental → floating).
- **Weak scalar** operands (a tensor combined with a bare numeric literal).

### 1.3 The problem
NEP-50 promotion picks a **result dtype `R = promote(A, B)`** from the operand dtypes
(value-independent). In numpy this is resolved dynamically. cajeta's `Tensor<T>` is
reified/monomorphized and statically typed, so the result dtype should be expressed in
the type system. The obstacle: cajeta has **no compile-time `value→type` / template
specialization** (a documented v1 gap), so `R` cannot be *derived as a type* from the
constant `promote(A,B)`. This spec resolves that by making `R` an **explicit, bounded
type parameter** of the kernel (caller- or convenience-supplied) and using the
**numeric markers** (`Number`/`Floating`/`Integral`) to bound inputs and to carry the
result **kind** statically when the exact width must stay dynamic.

### 1.4 Constraints (cajeta realities this spec must respect)
1. **No compile-time value→type.** Template specialization is a v1 gap; the exact
   promoted width cannot become a static type from a computed constant.
2. **Generic arithmetic must be on a method type-param.** Arithmetic on a bare class
   type-param `T` inside `Tensor<T>` mis-emits integer ops; ufunc arithmetic lives in
   **static method-type-param** ops with the dtype concrete at the call
   (see `cajeta-generic-arithmetic-class-param`).
3. **Borrowed-`Storage` discipline.** Operands are read directly (broadcast index),
   never wrapped in an intermediate owning-store view that could free a shared buffer
   early (the `rebuildShared`/`materializeFrom` lesson).
4. **`DType.promote(a, b)` already implements the NEP-50 array⊕array lattice** (runtime,
   over `DType` values) and is the single source of truth for promotion.

### 1.5 Non-goals
- **Fully-static exact-width auto-promotion** — auto-promote yields a *bounded wildcard*
  (`Tensor<? extends Number/Floating/Integral>`, kind static, width runtime), not a
  static `Tensor<float64>`. (Future enhancement once the compiler gains value→type.)
- **Complex-dtype ops** — no concrete complex element type exists yet; complex promotion
  rules are specified but not exercised.
- **GPU lowering** of promotion — the seam/lowering is numpy-plan Phase 3c/3f; this spec
  is dtype-correctness on the CPU floor (the GPU path reuses the same `R`).
- **Value-based casting** — explicitly excluded (NEP-50's whole point; promotion never
  inspects element values).

---

## 2. Promotion rules (the NEP-50 lattice)

### 2.1 Requirements
Promotion is **value-independent** and defined entirely by operand dtypes via
`DType.promote`. The result kind follows the category lattice
`bool < integral < floating < complex`; within a kind, width follows NEP-50.

### 2.2 Use cases
- **2.2.1** As a caller, when I combine two **same-dtype** tensors with an
  arithmetic ufunc, then the result dtype equals that dtype (e.g.
  `float32 ⊕ float32 → float32`).
- **2.2.2** As a caller, when I combine **same-kind, different-width** integers/floats,
  then the result is the **wider** (`int32 ⊕ int64 → int64`,
  `float32 ⊕ float64 → float64`).
- **2.2.3** As a caller, when I combine a **bool** with any dtype, then the result is
  the other dtype.
- **2.2.4** As a caller, when I combine **signed and unsigned** integers, then the
  result is the smallest signed type holding both (`int8 ⊕ uint8 → int16`, …), and
  `int* ⊕ uint64 → float64`.
- **2.2.5** As a caller, when I combine an **integer with a float**, then the result is
  a float of sufficient width (`int16 ⊕ float32 → float32`, `int32/int64 ⊕ float32 →
  float64`, `any int ⊕ float64 → float64`).
- **2.2.6** As a caller, when I combine a **float with a complex** (once complex
  exists), then the result is a complex of matching width.
- **2.2.7** As a developer, when promotion is requested, then it is computed by exactly
  one function (`DType.promote`) so behavior is consistent across every ufunc.

---

## 3. The typed kernel surface (`op<A,B,R>`)

### 3.1 Requirements
The workhorse is a **static, fully-typed** binary kernel parameterized by bounded input
types `A`,`B` and an explicit bounded result type `R`. Inputs are read by broadcast
index (no intermediate views), each element is converted to `R`, the op is performed in
`R`, and a fresh **C-contiguous** `Tensor<R>` is produced. One generic kernel covers all
numeric input pairs; only the `(A,B,R)` actually used are monomorphized.

### 3.2 Use cases
- **3.2.1** As a caller who knows the result dtype, when I call
  `Tensor.add<int32, float32, float64>(ai, bf)`, then I get a **statically-typed**
  `Tensor<float64>` with each element `= (float64)ai ⊕ (float64)bf`, no runtime dtype
  dispatch.
- **3.2.2** As a caller, when input shapes differ but are broadcast-compatible, then the
  kernel broadcasts right-aligned (numpy rules) and the result has the broadcast shape;
  incompatible shapes raise `BroadcastException`.
- **3.2.3** As a caller, when I pass `R = DType.promote(A,B)` (the NEP-50 result), then
  the kernel yields exactly numpy's promoted dtype and values — fully static.
- **3.2.4** As a caller, when I pass an `R` that loses information (e.g. narrowing),
  then the conversion follows cajeta's defined numeric cast for that pair (the kernel
  does not second-guess an explicit `R`).
- **3.2.5** As a developer, when the kernel runs, then operand `Storage` is never freed
  early (operands read directly; only the result owns fresh storage).

---

## 4. Same-dtype shorthand (`op<E>`)

### 4.1 Requirements
The overwhelmingly common case — both operands and the result share dtype `E` — has a
concise shorthand `op<E>(Tensor<E>, Tensor<E>) → Tensor<E>` (the `A=B=R=E`
specialization of §3). Already landed for `add`/`sub`/`mul` (commit `e59b5f05`); this
spec brings it under the unified surface and extends it across the ufunc families.

### 4.2 Use cases
- **4.2.1** As a caller, when both operands are `Tensor<float32>`, then
  `Tensor.add<float32>(a, b) → Tensor<float32>` (NEP-50 identity), with broadcasting.
- **4.2.2** As a caller, when I use the shorthand, then I get the same result as the
  general kernel with `A=B=R=E`, at the same cost.

---

## 5. Auto-promote convenience (bounded-wildcard result)

### 5.1 Requirements
For numpy-style ergonomics, an `op` convenience accepts marker-bounded inputs, computes
`R = DType.promote(...)` (plus the per-ufunc result rule, §6) **at runtime**, dispatches
to the §3 kernel for that `R`, and returns a **bounded wildcard** whose **kind** is
statically known from the markers (`Tensor<? extends Floating/Integral/Number>`). The
caller operates on it generically via bounded capture or narrows with a checked cast.

### 5.2 Use cases
- **5.2.1** As a caller, when I call the auto-promote `add(ai, bf)` on mixed
  `int32`/`float32` tensors, then I get a `Tensor<? extends Number>` whose runtime dtype
  is `float64` (= `promote(int32,float32)`), capturable as `(Tensor<float64>) r`.
- **5.2.2** As a caller, when the operation's result kind is fixed (e.g. true-division
  is always floating), then the convenience's static return type reflects it
  (`Tensor<? extends Floating>`).
- **5.2.3** As a caller writing dtype-generic code, when I receive the bounded-wildcard
  result, then I can pass it to routines bounded on the same marker without pinning the
  exact width.
- **5.2.4** As a caller, when I want the exact static type, then I capture the
  bounded-wildcard result to the concrete dtype I expect; a mismatch fails cleanly
  (no UB), consistent with the Phase-7 airlock.

---

## 6. Per-ufunc domains and result rules

### 6.1 Requirements
Each ufunc family declares its **input domain** (via marker bounds) and its **result
rule** (how `R` relates to the promotion):
- **arithmetic** (`add`/`sub`/`mul`): inputs `Number`; `R = promote(A,B)`.
- **true-division** (`div`): result is **floating** — `R = promote(A,B)` then bumped to
  floating, so `int32 / int32 → float64` (NEP-50 true division). The typed form may be
  offered as `div<…, R extends Floating>`.
- **floor-division / mod** (`floorDiv`/`mod`): inputs `Number`; `R = promote(A,B)`
  (integer stays integer).
- **comparison** (`eq`/`ne`/`lt`/`le`/`gt`/`ge`): inputs `Number`; **result `boolean`**
  (`Tensor<boolean>`), independent of `A,B`.
- **logical** (`and`/`or`/`xor`/`not`): inputs `boolean`; result `boolean`.
- **bitwise** (`bitAnd`/`bitOr`/`bitXor`/`shift`): inputs `Integral`; `R = promote(A,B)`.
- **transcendental/rounding** (`sin`/`exp`/`log`/`sqrt`/`floor`/`round`/…): inputs
  `Floating`; result `Floating` (typically same dtype).
- **`clip`/`where`**: `where(cond: Tensor<boolean>, a, b)` → `promote(A,B)`; `clip` keeps
  the operand dtype.

### 6.2 Use cases
- **6.2.1** As a caller, when I `div` two `int32` tensors, then the result is
  `float64` (true division), matching numpy.
- **6.2.2** As a caller, when I `floorDiv` two `int32` tensors, then the result is
  `int32`.
- **6.2.3** As a caller, when I compare two numeric tensors, then I get a
  `Tensor<boolean>` regardless of operand dtypes.
- **6.2.4** As a caller, when I call a bitwise op on floating tensors, then it is a
  **compile-time error** (domain bound `Integral`).
- **6.2.5** As a caller, when I call `sin` on an integer tensor, then it is a
  compile-time error (domain bound `Floating`) — convert first.

---

## 7. Weak scalar operands

### 7.1 Requirements
Combining a tensor with a **bare numeric literal** (a cajeta value, not a `Tensor`)
follows NEP-50 weak-scalar rules **without** promotion machinery: the literal adopts the
tensor's dtype when its category is ≤ the tensor's; when the literal's category is
strictly higher, the result bumps to that category's default width.

### 7.2 Use cases
- **7.2.1** As a caller, when I compute `tensor<int8> + 5` (a weak int), then the result
  is `Tensor<int8>` — the value is irrelevant (NEP-50; may overflow), no widening.
- **7.2.2** As a caller, when I compute `tensor<float32> + 1.0` (a weak float), then the
  result is `Tensor<float32>` (not `float64`).
- **7.2.3** As a caller, when I compute `tensor<int32> + 2.0` (weak float over an int
  tensor), then the result is `Tensor<float64>` (category bump to default float).
- **7.2.4** As a developer, when a scalar-operand op is monomorphized, then no
  `DType.promote` runtime call or dispatch is needed for the same-or-lower-category case
  — the scalar is converted to the tensor's element type inline.

---

## 8. Constraints & non-goals (summary)

### 8.1 Constraints
- **8.1.1** Promotion is value-independent and computed only by `DType.promote`.
- **8.1.2** ufunc arithmetic is performed in a **static method-type-param** op with the
  result dtype concrete at the call; never on a bare class type-param.
- **8.1.3** Operands are read by broadcast index; their `Storage` is never freed early.
- **8.1.4** Bounded markers (`Number`/`Floating`/`Integral`) gate input domains and carry
  result kind; they do **not** encode width.

### 8.2 Non-goals
- **8.2.1** No fully-static exact-width auto-promotion (bounded wildcard until the
  compiler gains value→type).
- **8.2.2** No complex-dtype execution (rules specified, not exercised).
- **8.2.3** No GPU lowering here (the promoted `R` is reused by the seam in a later unit).
- **8.2.4** No value-based casting.
