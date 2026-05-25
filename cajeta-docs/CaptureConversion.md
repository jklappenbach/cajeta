# Capture Conversion — capture#N Types

This document tracks the design and implementation plan for promoting cajeta's wildcard support from a syntactic receiver-identity heuristic to first-class capture types — Java's `capture#N` model — staged across multiple sessions.

## Why

The current capture-identity story (shipped through `aee10d4`) recognizes the read-back pattern `b.set(b.get())` by **identifier-text equality** on the receiver. That works for the common case but doesn't generalize:

- Chained receivers don't match: `holder.box.set(holder.box.get())` has two `DotExpression` receivers, not `IdentifierExpression`s, so the syntactic check skips.
- `this`-typed receivers don't match: `this.set(this.get())` inside a wildcard-typed class.
- Two distinct receivers of the same wildcard instantiation can't be distinguished beyond their names: there's no type-level evidence that `b1` and `b2` are different captures.
- Captures can't flow through method-template type-parameter inference: `swap<T>(b.get(), b.get())` can't unify T to the capture because the capture has no type-system presence.

A real capture type — a synthetic class allocated per binding site, with bounds and identity — replaces the syntactic check with a **type-system** check and unlocks the cases above.

## Model

### Capture types

`CajetaCapture` (a subclass of `CajetaClass`) represents a per-binding-site synthetic type:

- **Unique ID per declaration site.** Two `Box<? extends Animal>` locals get distinct captures even though they share the same source-level wildcard.
- **Upper bound** from `? extends B` (or `Object` when unbounded).
- **Lower bound** from `? super B` (or null when unbounded / extends-only).
- **Pointer-shaped at runtime.** Erased to ptr like every other class. No new LLVM type. The substitution-stable vtable hashes shipped in `1656022` already make dispatch work uniformly through any erased instantiation.

### Subtype rules

- `capture#N <: capture#N` — identity.
- `capture#N <: upperBound(N)` — bound projection (covariant read direction).
- `lowerBound(N) <: capture#N` — contravariant write direction.
- No other relation. `Animal <: capture#N(? extends Animal)` is **false** — the only way to satisfy capture#N is to be capture#N itself or its lower-bound subtype.

### Where captures get created

At each binding site that introduces a wildcard-typed slot:

1. **Local variable declarations** with wildcard type arguments — `LocalVariableDeclaration` creates a fresh capture per `?` in the declared type and threads the captures into the local's stored type.
2. **Method parameters** typed with wildcards — `Method::generatePrototype` creates captures when materializing parameter types.
3. **Method return types** with wildcards — receivers expose their captures in returns so chained calls see them.
4. **Field reads** through wildcard receivers — `DotExpression` projects through captures.

Fields declared on generic classes are NOT capture-bearing — they carry the class's own type-parameter (e.g., `T value` on `Box<T>`). The capture appears only when the class is *instantiated* with a wildcard, at the use site.

### Method dispatch

Method-resolution substitution maps `T → capture#N` instead of `T → wildcardSentinelExtends(B)`. The resolved method's return / parameter types carry the capture identity. Two calls on the same receiver produce values of the same capture type → unify at outer call sites.

Runtime dispatch uses the substitution-stable hashes from `1656022`. Captures contribute nothing to the runtime vtable — they're a compile-time type.

### Erasure

At codegen, captures behave like any pointer-typed value. There's no per-capture vtable, no special dispatch path, no metadata. The capture identity exists only in the type checker.

## Implementation plan

Staged so each session leaves the tree green and lands a self-contained slice.

### Phase 1 — Type machinery (no behavior change)

- [ ] **1.1** Add `CajetaCapture` class extending `CajetaClass`. Carries `captureId`, `upperBound`, optional `lowerBound`. `isCapture()` predicate. Static factory with monotonic ID counter.
- [ ] **1.2** Unit tests on the type itself: two captures with the same bound have distinct IDs; capture's bound is what was passed; `captureProject(capture#N)` returns the upper bound.

### Phase 2 — Local-variable capture creation

- [ ] **2.1** `LocalVariableDeclaration` walks the declared type for wildcard type-arguments. For each wildcard at the top level of the instantiation's type args, allocate a fresh capture and substitute it. The local's stored type becomes `Box<capture#N>` instead of `Box<? extends Animal>`.
- [ ] **2.2** Tests: two `Box<? extends Animal>` locals get distinct captures; chained reads through the captured local resolve members on the bound.
- [ ] **2.3** Remove the syntactic receiver-identity heuristic in MCE — replaced by capture-type equality (the resolved-types match because both calls produce values of the same capture).

### Phase 3 — Parameter / return capture flow

- [ ] **3.1** Method parameters typed with wildcards at user-declared sites: `static void f(Box<? extends B> b)` — `b`'s parameter type carries a capture per call. (Alternative: each invocation of `f` is a fresh binding, so the capture is per call-site. Decide and commit to one model.)
- [ ] **3.2** Method-template inference picks up captures: `static <T> void take(T x, T y); take(b.get(), b.get())` unifies T to the capture.

### Phase 4 — Field reads / write soundness via captures

- [ ] **4.1** Migrate the PECS write-soundness check from receiver-identity to capture-type comparison. Cleaner, generalizes to chained / `this` receivers.
- [ ] **4.2** Field reads through a wildcard-typed receiver project through the receiver's capture.

### Phase 5 — Cleanup

- [ ] **5.1** Lift the method-template lint suppression (no longer needs the wildcard sentinel as a T-placeholder — captures replace that role).
- [ ] **5.2** Stdlib PECS signature migration — swap producer-position `Stream<T>` for `Stream<? extends T>` where it expresses real variance.

## Out of scope

- **Variance through type constructors at declaration**: cajeta doesn't have site-variance annotations (`out T` / `in T`). Variance comes from wildcards at use sites only.
- **Capture *narrowing***: Java's "capture conversion" includes intersection types in some cases (`capture#N extends B & I`). v1 sticks with single-bound captures.
- **First-class capture types in user-written code**: you can't *name* a capture in source. They exist only at the type-checker level.

## Status

Phase 1 in progress.
