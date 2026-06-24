# Syntax Sugar — Specification

> Status: draft for review (2026-06-23). A bundle of **cheap grammar/codegen additions** (the
> "➕ tier" of `target-experience.md`), driven by núcleo; lexer/parser/codegen features that may
> graduate to `docs/specification/lang/` when implemented. Layer-1a foundation. Companions:
> `language-foundations.md`, `target-experience.md`, `records-spec.md`.
>
> This is a **spec** — requirements and use cases (the *why/what*). Build decisions (the *how*)
> are deferred as `> **TBD (plan-time):**` markers and collected in §11, to be resolved when this
> spec is turned into a plan. Outline-numbered for addressability.

## 1. Definition

### 1.1 Purpose
This spec collects the **familiar-feel syntax sugar** that makes núcleo's typed façades read
like numpy/torch instead of Java. Each feature is a small, bounded grammar or codegen addition.
Crucially, **none of these gate capability** — every operation they sugar already works today via
ordinary method calls (`a.matmul(b)`, `a.slice(Range...)`, `a.pow(b)`). What they deliver is
**muscle memory**: the surface a Python developer expects, so an ported script reads naturally.

### 1.2 Scope
- New operators: `@` (matmul), `**` (power).
- **Lowering** of already-parseable unary operators (`-`, `!`, `~`) for user types.
- Bracket **slicing** (`a[1:5]`, `a[1:5:2]`, `a[:, 0]`) and **multi-axis indexing** (`a[i, j]`).
- Indexing **markers**: ellipsis `...` and newaxis.
- **Tuple/record destructuring** (`var (q, r) = qr(a)`).
- **Type aliases** (`alias FloatTensor = Tensor<float32>`).
- **Comparison-returns-mask** for tensor types, enabling boolean-mask indexing (`a[a > 0]`).

### 1.3 Non-goals
- **New capability.** Nothing here adds an operation that methods cannot already express; this is
  surface only. A façade that shipped with *zero* sugar would still be functionally complete.
- **General operator-overloading expansion.** Cajeta already overloads `+ - * / %`, comparisons,
  `[]`, and compound-assign; this spec adds `@` and `**` and *lowers* the unary set — it does not
  open arbitrary new operator tokens to user definition.
- **Lazy/expression-template semantics.** Whether `a @ b` materializes or fuses is an engine
  concern (`nucleo.expr`), not a sugar concern. The sugar only fixes *what method the surface
  binds to*.
- **Runtime-dynamic shapes.** Compile-time shape checking applies only where dimensions are
  const-generic (fixed-size `Matrix`/`Tensor`); dynamic-rank tensors check at runtime as today.

### 1.4 Relationship to existing constructs
- Cajeta **already supports** operator overloading (`+ - * / %`, comparisons, `[]`,
  compound-assign), named arguments + defaults, `var` inference, and array literals — these
  features lean on that machinery.
- The audit confirms Cajeta currently **lacks**: a `@` token, a `**` token, *lowering* of unary
  `- ! ~` for user types (they parse but do not lower), bracket slice/multi-index syntax (only
  `.slice(Range...)` exists), indexing markers, destructuring binds, and type aliases.
- These compose with **records** (`records-spec.md`): destructuring (§8) binds a record's fields,
  and comparison-mask indexing (§10) feeds the typed columnar surface.

### 1.5 A bundle of independent features
Every section below is **independently shippable and individually low-risk**. There is no
ordering dependency among them (the only cross-feature tie is §8 destructuring sharing its bracket
choice with `records-spec.md` F8, and §5↔§10 both touching `[]`). They may ship in one grammar
pass or be rolled out incrementally as façade work demands each — see §11.

> **TBD (plan-time):** [S0] Do all nine features ship in a single grammar/codegen pass, or
> incrementally as each façade surfaces the need? Lean: ship the indexing cluster (§4–§6, §10)
> together since they share the `[]` parser, and the rest à la carte.

## 2. The `@` matrix-multiply operator

A new infix operator binding to a matmul method; the canonical numpy/torch surface for
contraction.
```cajeta
var mvp = proj @ view;          // 4x4 @ 4x4  -> Matrix<float32,4,4>
var y   = weights @ x;          // contraction, reads like the math
```

**Desugars to:** the user-type `matmul` overload — `a @ b` → `a.matmul(b)` (exact hook name is a
plan detail). For fixed-size `Matrix<T, R, C>` the inner dimension is **compile-time
shape-checked**: `Matrix<f32,4,4> @ Matrix<f32,4,3>` type-checks; `Matrix<f32,4,4> @ Vec3` is a
compile error at zero runtime cost.

**Familiar-feel benefit:** the single most recognizable numpy/torch operator. `proj @ view` is the
line every graphics/ML developer expects; `proj.matmul(view)` reads as a Java port.

**Use cases**
- **2.1** As a graphics developer, when I write `proj @ view` on two `Matrix<f32,4,4>` values, then
  it lowers to the matmul kernel and yields a `Matrix<f32,4,4>`.
- **2.2** As a developer, when I write `a @ b` where the inner dimensions of two fixed-size
  matrices disagree, then I get a **compile error** naming the mismatch — not a runtime exception.
- **2.3** As a developer, when I write `a @ b` on dynamic-rank tensors whose shapes are not
  statically known, then the operator binds and shape-checks at runtime as the method form does.
- **2.4** As a developer, when neither operand defines a matmul overload, then `@` is a compile
  error (the token is reserved for the overload; it is not a fallback to anything else).
- **2.5** As a developer, when I read mixed arithmetic (`a @ b + c`), then `@` binds with
  multiplication-like precedence (tighter than `+`), matching the math and numpy.

> **TBD (plan-time):** [S1] Exact precedence/associativity of `@` relative to `*` (numpy gives
> `@` the same precedence as `*`, left-associative). And the overload hook name (`matmul` vs.
> an operator-method spelling).

## 3. The `**` power operator

A new infix operator for exponentiation, the numpy/Python spelling of power.
```cajeta
var sq = x ** 2;                // x.pow(2)
var r  = base ** exponent;
```

**Desugars to:** the user-type `pow` overload — `a ** b` → `a.pow(b)`. Applies to scalar numeric
types and element-wise to tensors (whichever defines the overload).

**Familiar-feel benefit:** `x ** 2` is universal numpy/Python; the Java-ish `Math.pow(x, 2)` or
`x.pow(2)` breaks the read of a ported numerical expression.

**Use cases**
- **3.1** As a developer, when I write `x ** 2` on a numeric scalar, then it lowers to the power
  operation and yields the same numeric type.
- **3.2** As a developer, when I write `t ** 2` on a tensor that defines the power overload, then
  it applies element-wise.
- **3.3** As a developer, when I write `a ** b ** c`, then it associates **right** (matching
  Python: `a ** (b ** c)`), distinct from left-associative arithmetic.
- **3.4** As a developer, when no `pow` overload exists for the operand types, then `**` is a
  compile error.

> **TBD (plan-time):** [S2] Right-associativity and precedence of `**` (Python binds it tighter
> than unary minus on the right: `-x ** 2 == -(x ** 2)`); confirm we match.

## 4. Unary operator lowering (`-`, `!`, `~`)

Cajeta **already parses** unary `-`, `!`, and `~` but does **not lower** them for user types — they
work for built-ins only. This feature makes the existing unary overloads (negate / logical-not /
bitwise-not) lower for user types, completing the operator-overloading story.
```cajeta
var neg  = -tensor;             // tensor.negate()
var flip = !mask;               // mask.not()
var inv  = ~bits;               // bits.complement()
```

**Desugars to:** the matching user-type unary overload — `-a` → `a.negate()`, `!a` → `a.not()`,
`~a` → `a.complement()` (exact hook names are a plan detail).

**Familiar-feel benefit:** `-x` and `~mask` are everywhere in numerical/mask code; today the parse
succeeds but codegen silently fails to bind, so a developer must write `x.negate()`. Lowering
closes a gap rather than adding a token — the lowest-risk item here.

**Use cases**
- **4.1** As a developer, when I write `-t` on a tensor that defines a unary-negate overload, then
  it lowers to that overload (today it parses but does not lower).
- **4.2** As a developer, when I write `!mask` on a boolean-mask type that defines logical-not,
  then it lowers element-wise.
- **4.3** As a developer, when I write `~bits` on a type defining bitwise-complement, then it
  lowers to that overload.
- **4.4** As a developer, when the operand type defines no matching unary overload, then it is a
  compile error (the diagnostic must distinguish "no overload" from the prior silent non-lowering).
- **4.5** As a developer, when I write `-x` on a built-in numeric, then existing behavior is
  unchanged (no regression).

> **TBD (plan-time):** [S3] Whether the three unary overloads are spelled as named methods
> (`negate`/`not`/`complement`) or an operator-method form, and the exact diagnostic for the
> previously-silent failure path.

## 5. Bracket slicing

Range-based slicing inside `[]`, the numpy/torch surface that currently exists only as
`.slice(Range...)`.
```cajeta
var head = a[1:5];              // a.slice(Range(1,5))
var step = a[1:5:2];            // a.slice(Range(1,5,2))
var col0 = a[:, 0];            // first axis full, second axis index 0
var tail = a[2:];              // open upper bound
var pre  = a[:3];              // open lower bound
```

**Desugars to:** a `slice` call over `Range`/index descriptors — the colon forms produce `Range`
arguments (`1:5` → `Range(1,5)`, `1:5:2` → `Range(1,5,2)`, open ends → unbounded `Range`), and a
bare index in a multi-axis position produces a scalar selector. The result follows numpy semantics
(a scalar index drops that axis; a range keeps it).

**Familiar-feel benefit:** `a[1:5:2]` and `a[:, 0]` are the daily numpy idiom; `a.slice(Range(1,5,2))`
is a faithful but alien transcription.

**Use cases**
- **5.1** As a developer, when I write `a[1:5]`, then it selects elements 1..4 (half-open, numpy
  convention) via the slice path.
- **5.2** As a developer, when I write `a[1:5:2]`, then the third value is the step.
- **5.3** As a developer, when I write `a[2:]` or `a[:3]` or `a[:]`, then the omitted bound is
  open (start, stop, or both).
- **5.4** As a developer, when I write `a[:, 0]` on a 2-D structure, then axis 0 is taken whole and
  axis 1 is indexed at 0 (the scalar drops that axis).
- **5.5** As a developer, when a slice's bounds are statically out of range for a fixed-size shape,
  then it is a compile error; for dynamic shapes it is a runtime bounds check as today.
- **5.6** As a developer, when I assign into a slice (`a[1:5] = b`), then it routes to the
  slice-assignment / compound-assign path (consistent with existing `[]` assignment).

> **TBD (plan-time):** [S4] The grammar for the colon form inside `[]` — colon is otherwise used
> for named-argument and aggregate-init syntax, so the slice colon must be unambiguous *inside
> subscript context only*. And whether assignment-into-slice (5.6) is in this pass.

## 6. Multi-axis indexing

Comma-separated selectors inside one subscript — `a[i, j]` — the numpy n-d access form. The lexer
must **disambiguate** the comma here from a comma-expression.
```cajeta
var e = a[i, j];                // a.index(i, j)  — element of a 2-D structure
var r = a[i, 1:5];             // mix scalar + slice across axes
```

**Desugars to:** a multi-argument `index`/`slice` call carrying one selector per axis
(`a[i, j]` → `a.index(i, j)`), each selector being a scalar, a `Range` (§5), or a marker (§7).

**Familiar-feel benefit:** `a[i, j]` is *the* way numpy users address an element; the method form
`a.index(i, j)` or chained `a[i][j]` (which has different semantics) breaks the muscle memory and,
for SoA tensors, the chained form is also wrong.

**Use cases**
- **6.1** As a developer, when I write `a[i, j]` on a 2-D structure, then it addresses the single
  element at (i, j) — not `a[i][j]` row-then-column chaining.
- **6.2** As a developer, when I write `a[i, 1:5]`, then I mix a scalar selector on one axis with a
  range on another in one subscript.
- **6.3** As a developer, when the number of comma-separated selectors exceeds the structure's rank
  (and no ellipsis is present), then it is a compile error for fixed-rank types.
- **6.4** As a developer, when I write a single-axis subscript `a[i]`, then existing single-index
  `[]` behavior is unchanged (no regression).
- **6.5** As a parser, when a comma appears inside `[ ]`, then it is read as an axis separator, not
  a comma-expression operator (the disambiguation is scoped to subscript context).

> **TBD (plan-time):** [S5] **The lexer/parser disambiguation strategy for `a[i, j]`** — the comma
> inside `[]` must mean "next axis," not the comma operator. Options: a subscript-scoped parse mode
> that reinterprets commas, vs. parsing the bracket body as a comma-list always and letting the
> overload decide. This is the highest-attention TBD in the spec.

## 7. Indexing markers — ellipsis and newaxis

Two numpy index markers: `...` (ellipsis — "all remaining axes") and a newaxis marker (insert a
length-1 axis).
```cajeta
var last = a[..., 0];           // index last axis, leave the rest whole
var lifted = a[:, None];        // insert a new length-1 axis (newaxis)
var b = a[..., None];
```

**Desugars to:** axis selectors in the multi-axis `index`/`slice` call (§6) — `...` expands to as
many full-axis selectors as needed to fill the rank; the newaxis marker injects a length-1 axis
into the result shape.

**Familiar-feel benefit:** `a[..., None]` and `a[..., 0]` are pervasive in tensor code
(broadcasting setup, batch-axis manipulation); there is no readable method spelling for them.

**Use cases**
- **7.1** As a developer, when I write `a[..., 0]`, then `...` stands for every axis but the last,
  each taken whole, and the last is indexed at 0.
- **7.2** As a developer, when I write `a[:, None]` (or the chosen newaxis spelling), then a new
  length-1 axis is inserted at that position, raising the rank by one.
- **7.3** As a developer, when I use more than one `...` in a single subscript, then it is a compile
  error (numpy rule: at most one ellipsis).
- **7.4** As a developer, when `...` is combined with explicit selectors that already cover the full
  rank, then `...` expands to zero axes (a no-op fill), consistent with numpy.

> **TBD (plan-time):** [S6] The **newaxis spelling** — reuse `None` (numpy's literal), introduce a
> dedicated marker (e.g. `newaxis`), or both. And the token for ellipsis if `...` collides with any
> existing varargs/spread syntax in the grammar.

## 8. Tuple / record destructuring

Bind multiple locals from a multi-field value in one statement — the numpy/scipy `q, r = qr(a)`
idiom. Works against **records** (`records-spec.md` F8), binding their fields positionally.
```cajeta
var (q, r) = qr(a);             // binds q = res.q, r = res.r  (res : QrResult { q, r })
var (mn, mx) = minmax(data);
```

**Desugars to:** evaluate the right-hand value once into a temporary, then bind each name to a
field in declared order — `var (q, r) = qr(a)` becomes `var __t = qr(a); var q = __t.q; var r = __t.r;`.
For a record, the order is the record's declared field order (`records-spec.md` §2).

**Familiar-feel benefit:** `var (q, r) = qr(a)` is the scipy/numpy multiple-return idiom; without
it the typed-return-bag (records §5) is read as `var res = qr(a); use res.q, res.r` — correct but
not the muscle-memory form.

**Use cases**
- **8.1** As a developer, when I write `var (q, r) = qr(a)` and `qr` returns a record with fields
  `q`, `r` in that order, then `q` and `r` are bound to those fields.
- **8.2** As a developer, when the count of destructured names does not match the record's field
  count, then it is a compile error naming the arity mismatch.
- **8.3** As a developer, when I destructure a record, then the right-hand expression is evaluated
  **once** (no double-eval of side effects).
- **8.4** As a developer, when a destructured field's type is inferred, then each local takes that
  field's declared type (full typing, like `var` elsewhere).
- **8.5** As a developer, when destructuring is unavailable (feature not shipped), then field access
  (`res.q`) still delivers the capability — destructuring is sugar, not a gate.

> **TBD (plan-time):** [S8] **Destructuring bracket choice — `(q, r)` positional vs. `{q, r}`
> by-name** — this is **shared with `records-spec.md` F8** and must be decided jointly. Positional
> matches numpy/scipy; by-name matches the record aggregate-init `{ field: value }` surface. Also:
> whether destructuring binds *only* records or also a future tuple type.

## 9. Type aliases

A name for an existing (possibly fully-instantiated) type, so verbose monomorphized types read
cleanly.
```cajeta
alias FloatTensor = Tensor<float32>;
alias Mat4 = Matrix<float32, 4, 4>;
alias Frame = Table<Tick>;
```

**Desugars to:** a **transparent** alias — `FloatTensor` *is* `Tensor<float32>` (same type
identity, interchangeable, no distinct nominal type). The alias is resolved at type-resolution time
and carries no runtime presence.

**Familiar-feel benefit:** numpy/torch code leans on short type names; `Tensor<float32>` repeated
across signatures is noisy, and `Matrix<float32, 4, 4>` even more so. Aliases keep the typed
surface readable without giving up the precision.

**Use cases**
- **9.1** As a developer, when I declare `alias FloatTensor = Tensor<float32>` and then use
  `FloatTensor` in a signature, then it is fully interchangeable with `Tensor<float32>`.
- **9.2** As a developer, when I pass a `FloatTensor` where a `Tensor<float32>` is expected (or
  vice versa), then it type-checks — the alias is transparent, not a new nominal type.
- **9.3** As a developer, when I alias a fixed-size shape (`alias Mat4 = Matrix<float32,4,4>`), then
  all compile-time shape checks (§2) apply through the alias unchanged.
- **9.4** As a library author, when I alias a record-parameterized type (`alias Frame = Table<Tick>`),
  then the alias carries the schema transparently.
- **9.5** As a developer, when an alias names a template that still has free parameters
  (`alias Vec<T> = Tensor<T>`, if supported), then it is itself parameterized — *(scope TBD below)*.

> **TBD (plan-time):** [S9] Whether aliases may be **parameterized** (`alias Vec<T> = ...`, 9.5) in
> v1 or only fully-applied. And whether an alias is strictly transparent or may opt into nominal
> (`newtype`) distinctness later (out of scope here, noted to keep the door open).

## 10. Comparison-returns-mask for tensors

Comparison operators (`> < >= <= == !=`) returning an element-wise **boolean mask** for tensor
types — already true for `@ValueType` SIMD vectors; this **extends it to `Tensor`** — enabling
boolean-mask indexing.
```cajeta
var mask = data > 0.0;          // element-wise boolean mask tensor
var pos  = data[data > 0.0];    // mask-indexed gather of positive elements
```

**Desugars to:** the tensor comparison overload returning a mask tensor (`data > 0.0` →
`data.gt(0.0)` yielding a boolean mask), and mask-indexed subscript routing through the `[]` path
(`data[mask]` → a gather/filter by mask). Composes with §5/§6 (the subscript carrying a mask is one
more selector kind).

**Familiar-feel benefit:** `data[data > 0]` is the canonical numpy boolean-mask filter; without it
a developer writes a method chain (`data.gt(0.0).select(...)`) that loses the idiom entirely.

**Use cases**
- **10.1** As a developer, when I write `data > 0.0` on a tensor, then I get an element-wise boolean
  mask of the same shape (not a single boolean).
- **10.2** As a developer, when I write `data[data > 0.0]`, then I get the elements where the mask
  is true (boolean-mask gather).
- **10.3** As a developer, when I write a comparison on a fixed-size `@ValueType` SIMD vector, then
  existing mask-returning behavior is unchanged (this extends, not replaces, that path).
- **10.4** As a developer, when I compare two tensors of incompatible shape, then it is a compile
  error for fixed shapes / a runtime check for dynamic, consistent with element-wise ops.
- **10.5** As a developer, when I use a mask where a scalar boolean is required (`if (data > 0.0)`),
  then it is a compile error — a mask is not a scalar condition (numpy's `any()`/`all()` is the
  explicit reduction).

> **TBD (plan-time):** [S10] Whether mask-indexed **assignment** (`data[data > 0.0] = 0.0`,
> numpy's masked-write) is in this pass or deferred. And the result-shape contract of a
> mask gather (1-D compacted, numpy-style) vs. shape-preserving with a fill.

## 11. Acceptance criteria (spec-level)
- `a @ b` and `a ** b` lower to the matmul/power overloads; fixed-size `@` mismatches are compile
  errors.
- Unary `-`/`!`/`~` lower for user types that define the overload (closing the parse-but-don't-lower
  gap); built-ins unregressed.
- `a[1:5]`, `a[1:5:2]`, `a[:, 0]`, `a[i, j]`, `a[..., 0]`, and a newaxis form parse and route to the
  slice/index path with numpy semantics.
- `var (q, r) = qr(a)` binds a record's fields, evaluating the RHS once.
- `alias Name = Type` declares a transparent, interchangeable alias.
- A tensor comparison yields an element-wise mask, and `data[data > 0.0]` gathers by it.
- Every feature is reachable-or-equivalent via methods today, confirming none gates capability.

## 12. Open questions (resolve at plan time)
- **[S0]** Single grammar pass vs. incremental rollout (§1.5).
- **[S1]** `@` precedence/associativity and overload hook name (§2).
- **[S2]** `**` right-associativity/precedence vs. unary minus (§3).
- **[S3]** Unary-overload spelling and the diagnostic for the formerly-silent path (§4).
- **[S4]** Slice-colon grammar inside `[]` (colon disambiguation) and slice-assignment scope (§5).
- **[S5]** **The lexer/parser disambiguation strategy for multi-axis `a[i, j]`** (§6) — highest
  attention.
- **[S6]** Newaxis spelling (`None` vs. dedicated marker) and the ellipsis token (§7).
- **[S8]** **Destructuring syntax `(q, r)` vs. `{q, r}`** — **shared with `records-spec.md` F8**,
  decided jointly (§8).
- **[S9]** Parameterized aliases and transparent-vs-nominal scope in v1 (§9).
- **[S10]** Mask-indexed assignment and mask-gather result-shape contract (§10).
