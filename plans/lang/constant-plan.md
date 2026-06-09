# Plan: `const` — immutability bindings, qualifiers, and the optimizations they unlock

Status: **NOT STARTED (drafted 2026-06-09).** Captured so the idea isn't lost.
The `const` keyword already lexes (`CajetaLexer.g4:44`) and parses as a
`classOrInterfaceModifier` (`CajetaParser.g4:69`), but it is **semantically a
no-op today** — there is no `CONST` in the `Modifier` enum
(`src/cajeta/type/Modifiable.h:13`) and `Modifiable::toModifier()` has no
`"const"` case, so the token is silently discarded. This plan turns the reserved
keyword into a real, enforced, *optimizing* qualifier.

Companion docs to write alongside: `docs/lang/Const.md` (the spec) and a
MemoryModel.md edit (const ↔ borrow-checker interaction). This file is the
**plan** (phased, checkbox-tracked, TDD); the spec lands in CONST-0.

---

## 1. The case — why `const` is worth building

`const` is usually pitched as a *safety* feature (catch accidental mutation).
That is the least interesting reason to build it in **this** language. Cajeta is
already Rust-style borrow-checked and monomorphizing; `const` slots into that
machinery as a first-class **aliasing and immutability contract** that the
optimizer can actually exploit. The payoff is in three buckets:

### 1.1 Semantic / correctness gains

- **Immutable bindings.** `const int32 x = f();` — reassignment is a compile
  error. Cheap, expected, documents intent.
- **A real shared-vs-exclusive borrow marker.** This is the big one. Cajeta
  already distinguishes ownership transfer (`#T`) from borrow (plain `T`), but a
  *borrow* today is undifferentiated. Rust's whole optimization story rests on
  `&T` (shared, immutable, may alias other `&T`) vs `&mut T` (exclusive,
  mutable, provably unaliased). `const T` / `T` is the natural place to draw
  that line: a `const` borrow is a **shared immutable borrow** (many may
  coexist), a plain mutable borrow is **exclusive**. The borrow checker already
  tracks live borrows; `const` refines the rule instead of adding new machinery.
- **`const` member functions** (C++-style read-only receiver) — a method
  declared `const` may not mutate `this`, and may be called on a `const`
  reference. Gives APIs an enforceable read-only surface.
- **Foundation for compile-time evaluation** (`constexpr`-like). Not in scope
  here, but a tracked `const` with a constant initializer is the prerequisite.

### 1.2 Optimization / reduced-IR gains (the part that pays for itself)

These are wins the optimizer **cannot get on its own** without whole-program
escape/alias analysis — front-end `const` hands LLVM the facts directly:

1. **`constant` LLVM globals for `const static` fields.** A `const static`
   field with a constant initializer is emitted as `@g = constant ...` instead
   of `@g = global ...`. LLVM then: places it in `.rodata`, **constant-folds
   loads of it across functions**, dedupes identical constants, and never emits
   a store. Today every static read is a `load` the optimizer must prove
   invariant; `const` makes it free. **Measurable IR reduction.**

2. **`readonly` / `readnone` parameter & function attributes.** A `const`
   reference parameter becomes an LLVM `readonly` pointer arg; a method that
   only reads through `const` params/`this` can earn `readonly`/`readnone`
   function attributes. These unlock **LICM (hoist loads out of loops), CSE of
   loads across calls, and dead-call elimination** — LLVM otherwise has to
   assume any call may clobber any memory.

3. **`!invariant.load` / `!invariant.group` metadata.** A load through a `const`
   borrow of an object whose constness is guaranteed for the borrow's lifetime
   can carry `!invariant.load`, letting LLVM hoist and CSE it freely even across
   opaque calls. This is exactly the loop-invariant-load pattern that dominates
   getters-in-loops.

4. **Richer alias analysis.** A `const` shared borrow promises *no mutation
   through this path*. Combined with the exclusive-mutable-borrow guarantee,
   this gives cajeta TBAA-grade `noalias`/immutability facts **for free from the
   type system** — the same source of speed that makes Rust's codegen
   competitive. Fewer reloads, more reordering, better vectorization.

5. **Leaner borrow-checker bookkeeping → less drop/IR overhead.** A `const`
   borrow never moves and never mutates, so it needs no move-tracking and
   registers no drop entry transitions. Fewer `__cajeta_drop_*` calls emitted,
   smaller IR, less work at scope exit. (Direct synergy with the ownership-
   transfer machinery touched in the reflection/LinkedList work.)

6. **Devirtualization opportunity (speculative, later).** A `const`-constructed
   object reached through a `const` borrow whose dynamic type is provable lets
   the optimizer fold virtual dispatch to a direct call. Tracked, not scheduled.

### 1.3 What we explicitly do NOT adopt

- **Java's "effectively final for lambda capture" rule.** Java forces captured
  locals to be (effectively) `final`/`const`. The user dislikes this and we
  reject it: cajeta lambda/closure capture will be governed by the **borrow
  checker** (capture a mutable exclusive borrow, or move ownership in), not by a
  blanket const requirement. `const` stays a *chosen* contract, never a tax the
  language imposes to make closures work. (Recorded as decision **D-CAP**.)

### 1.4 Scope discipline

`const`-correctness is famously viral (it propagates through signatures). To
avoid a months-long migration we ship in **enforced-but-opt-in** layers: the
binding/parameter/method qualifiers first (local blast radius), the
optimization attributes next (pure upside, no new errors), and the
borrow-checker integration last and behind a flag until the stdlib is annotated.

---

## 2. Design decisions to lock first (CONST-0)

- [ ] **CONST-0.1** Write `docs/lang/Const.md` — the spec. Define the four
      positions `const` can appear and what each means:
      (a) **binding** `const T x = …` (immutable local/field binding);
      (b) **reference qualifier** `const T&`-equivalent param/return (shared
      immutable borrow — note cajeta spelling, plain `T` borrow + `const`);
      (c) **member function** `const` after the signature (read-only `this`);
      (d) **static field** `const static` (compile-time-constant global).
- [ ] **CONST-0.2** Decide **deep vs shallow** const. Recommendation: **shallow
      with transitive read-through** — a `const` binding to a reference forbids
      reassigning the binding AND mutating through it (matches Rust `let` on a
      shared borrow), but does not deep-freeze owned sub-objects beyond what the
      borrow rule already gives. Document precisely; it drives every later check.
- [ ] **CONST-0.3** Decide `const` ↔ `final` relationship. `final` currently
      means class-finality / static dispatch (`FINAL = 0x20`). `const` is
      orthogonal (value immutability). Confirm they coexist on one declaration
      and never alias the same bit.
- [ ] **CONST-0.4** Decide initialization rule: a `const` binding must be
      **definitely assigned exactly once** before use (P3 definite-assignment
      already exists in `BinaryOpExpression.cpp` — reuse it). `const` without an
      initializer is legal only if assigned once on every path.
- [ ] **CONST-0.5** Ratify **D-CAP** (no Java effectively-final capture rule).

**Deliverable:** `docs/lang/Const.md` committed; decisions recorded here.
**Acceptance:** each of the four positions has a worked example + the
exact error it raises on violation; D-CAP written down.

---

## 3. Phase 1 — Modifier plumbing (CONST-1)

Make `const` a tracked modifier end-to-end. No enforcement yet — just stop
discarding it, so RTTI/reflection/printers see it.

- [ ] **CONST-1.1** Add `CONST = 0x100` to the `Modifier` enum
      (`src/cajeta/type/Modifiable.h:13`). (Next free bit after `ASYNC = 0x80`.)
- [ ] **CONST-1.2** Map it in `Modifiable::toModifier()` (`"const" → CONST`) and
      `toString()` (`CONST → "const"`).
- [ ] **CONST-1.3** Add `isConst()` helper (mirror `isStatic()`).
- [ ] **CONST-1.4** Confirm the visitor actually feeds the parsed `CONST` token
      into the modifier set (it already walks `classOrInterfaceModifier`; verify
      `const` now survives instead of being dropped). Extend the
      `variableModifier` grammar rule to also accept `CONST` if locals are to be
      const (grammar currently only allows `FINAL | annotation` there —
      **grammar edit + regenerate ANTLR**).
- [ ] **CONST-1.5** Surface the bit in RTTI modifier words so reflection's
      `getModifierFlags()` reports it (cross-ref `plans/reflection` — the
      `Modifier` bit doc in `Class.cajeta` lists the existing bits; add CONST).

**TDD:**
- `test/parser/ModifierTests.cpp` (or nearest existing) — parse a decl with
  `const`, assert the type/field/method's `getModifiers()` contains `CONST` and
  `toCanonical()` round-trips `"const"`.
- A reflection test: `Class.of(obj).getFieldModifierFlags(i) & 0x100 != 0` for a
  `const` field.

**Deliverable:** `const` is a first-class modifier visible in metadata; no
behavior change otherwise.
**Acceptance:** modifier round-trip test + reflection-bit test green; full
existing regression suite unchanged (const still does nothing observable beyond
metadata).

---

## 4. Phase 2 — `const` bindings + assignment enforcement (CONST-2)

The first *enforced* layer. Local blast radius: a `const` binding may not be
reassigned.

- [ ] **CONST-2.1** Tag the binding's symbol/`Field`/local entry as const at
      declaration (`LocalVariableDeclaration.cpp` for locals; field path for
      members).
- [ ] **CONST-2.2** In the assignment path (`BinaryOpExpression.cpp`, the `=`
      handler — same file as the P3 definite-assignment + the field-store
      ownership-transfer block) reject a store whose LHS resolves to a const
      binding that is **already definitely assigned**. The first assignment of an
      uninitialized const is allowed (CONST-0.4); subsequent ones error.
- [ ] **CONST-2.3** Clear, located diagnostic: `cannot assign to const binding
      'x' (declared const at <loc>)`.
- [ ] **CONST-2.4** Reject `const` on a binding that is also an ownership-
      transfer target if that contradicts the move semantics — define the
      interaction (a `const` owner can still be dropped; it just can't be
      reassigned). Document.

**TDD (write first, must fail before impl):**
- `test/parser/ConstBindingTests.cpp` — `const int32 x = 1; x = 2;` ⇒ compile
  error with the expected message.
- `const int32 x; if (c) x = 1; else x = 2; use(x);` ⇒ OK (single assignment per
  path).
- `const int32 x = 1; x = 1;` ⇒ error (no "assigning same value" loophole).
- Negative control: non-const reassign still compiles & runs.

**Deliverable:** reassigning a `const` binding is a compile error; everything
else compiles as before.
**Acceptance:** ConstBindingTests green; full regression green (no false
positives on existing non-const code).

---

## 5. Phase 3 — `const` parameters & `const` member functions (CONST-3)

Extend immutability to signatures.

- [ ] **CONST-3.1** `const` parameter: body may not reassign the param nor mutate
      through it (read-only borrow). Enforced via the CONST-2 assignment check +
      CONST-6 borrow rule (forward ref).
- [ ] **CONST-3.2** `const` member function (grammar: allow a trailing/leading
      `const` on `methodDeclaration` — **grammar edit + regen**). A `const`
      method gets a read-only `this`; calling a non-const method on `this` from
      within is an error; mutating a field of `this` is an error.
- [ ] **CONST-3.3** Overload/override interaction: a `const` and non-`const`
      method with the same signature — decide (recommend: const-ness is part of
      the method's contract but NOT part of the overload key v1; a `const`
      override of a non-const virtual is allowed, not vice-versa). Document.
- [ ] **CONST-3.4** A `const` reference may only call `const` methods on the
      referent.

**TDD:**
- `test/parser/ConstMethodTests.cpp` — `const` method mutating a field ⇒ error;
  `const` method calling a non-const method ⇒ error; read-only body ⇒ OK and
  runs (JIT).
- `const` param reassigned ⇒ error; passed-through read-only ⇒ OK.
- Calling a non-const method on a `const` ref ⇒ error.

**Deliverable:** `const` parameters and `const` member functions enforce
read-only semantics.
**Acceptance:** ConstMethodTests green; stdlib still compiles (it has no `const`
yet, so this must be purely additive); regression green.

---

## 6. Phase 4 — optimization: `const static` → LLVM `constant` globals (CONST-4)

First pure-upside optimization layer. No new errors; smaller/faster IR.

- [ ] **CONST-4.1** In static-field codegen (`StructureMetadata.cpp` /
      wherever statics become globals), emit a `const static` field initialized
      with a compile-time constant as `llvm::GlobalVariable` with
      `isConstant=true` (and appropriate linkage/`unnamed_addr` for dedup).
- [ ] **CONST-4.2** Loads of such a field carry no aliasing hazard — verify LLVM
      folds them (check `--emit=ir` before/after; cross-function constant
      propagation should kick in at -O1+).
- [ ] **CONST-4.3** Guard: a `const static` with a *non*-constant initializer
      (runtime-computed) must NOT be marked `constant` — fall back to a normal
      global written once (init-guard). Detect constant-foldability conservatively.

**TDD:**
- `test/codegen/ConstGlobalTests.cpp` — compile a class with
  `const static int32 K = 42;`, emit IR, assert the global is `constant` and a
  function returning `K` folds to `ret i32 42` at -O1.
- Non-constant init ⇒ assert it's a normal `global`, still correct at runtime.

**Deliverable:** const statics live in rodata and fold across functions.
**Acceptance:** IR assertions green; runtime values correct; an IR-size or
instruction-count check on a small fixture shows reduction vs the non-const
baseline (record the number in the plan).

---

## 7. Phase 5 — optimization: `readonly`/`invariant` attributes (CONST-5)

The second optimization layer — hand LLVM the aliasing facts from `const`
references and methods.

- [ ] **CONST-5.1** Mark `const` reference (pointer) parameters with the LLVM
      `readonly` parameter attribute at the call ABI boundary.
- [ ] **CONST-5.2** A method whose only memory effects are reads through `const`
      params/`const this` earns `readonly` (or `readnone` if it touches no
      memory) function attribute. Conservative analysis — only when provably
      side-effect-free at this level.
- [ ] **CONST-5.3** Loads through a `const` borrow of a value whose constness
      holds for the borrow lifetime carry `!invariant.load` metadata where sound.
- [ ] **CONST-5.4** Verify LICM/CSE actually fire: a getter-in-a-loop fixture
      should hoist the load after annotation.

**TDD:**
- `test/codegen/ConstAttrTests.cpp` — IR assertion that a `const`-ref param has
  `readonly`; that a pure const method has `readonly`/`readnone`.
- A loop fixture: `for (...) sum += obj.get();` where `get` is `const` — assert
  the load is hoisted (1 load in IR, not N) at -O2.
- Soundness negative control: a non-const path through the same memory does NOT
  get the attribute (mutation observed correctly).

**Deliverable:** `const` reference params and pure const methods carry the
attributes that enable LICM/CSE/DCE.
**Acceptance:** attribute + hoist IR assertions green; a microbench (or
instruction-count proxy) shows the loop-invariant load eliminated; regression
green (no miscompiles — the soundness control is the gate).

---

## 8. Phase 6 — borrow-checker integration: shared vs exclusive (CONST-6)

The deepest and last layer — `const` becomes the shared-immutable-borrow marker.
Behind a compiler flag (`--const-borrows` or similar) until the stdlib is
annotated, because it can introduce new borrow errors in existing code.

- [ ] **CONST-6.1** Classify each borrow as **shared/immutable** (`const`) or
      **exclusive/mutable** (plain). Many shared borrows may coexist; an
      exclusive borrow excludes all others (the Rust rule).
- [ ] **CONST-6.2** Enforce: cannot take an exclusive borrow while a shared
      borrow is live, and vice-versa; cannot mutate through a shared borrow.
- [ ] **CONST-6.3** Feed the shared-borrow guarantee into CONST-5's
      `noalias`/`invariant` emission (now provably sound, not conservative).
- [ ] **CONST-6.4** Elide move-tracking and drop-entry transitions for `const`
      shared borrows (they never move/mutate) — measure the drop-call reduction.
- [ ] **CONST-6.5** Flag-gate; annotate stdlib hot paths; flip default once green.

**TDD:**
- `test/parser/ConstBorrowTests.cpp` — two `const` borrows coexist ⇒ OK; a
  mutable borrow alongside a const borrow ⇒ error; mutate-through-const ⇒ error.
- Regression: the full suite under `--const-borrows` (expect some stdlib
  annotation churn; track failures → annotate, not suppress).
- Drop-count assertion: a fixture with a `const` borrow emits fewer
  `__cajeta_drop_*` calls than the mutable-borrow equivalent.

**Deliverable:** `const` is the shared-immutable-borrow qualifier; aliasing
facts from it are provably sound; drop overhead drops.
**Acceptance:** ConstBorrowTests green; suite green under the flag with the
stdlib annotated; recorded drop-call / IR reduction numbers; default-flip
decision recorded.

---

## 9. Phase 7 — reflection, tooling, docs (CONST-7)

- [ ] **CONST-7.1** Reflection: `Modifier.CONST` exposed; `Class`/`Field`/
      `Method` const-ness queryable (the bit from CONST-1.5).
- [ ] **CONST-7.2** cajetadoc / printers render `const` in signatures.
- [ ] **CONST-7.3** `docs/lang/Const.md` finalized with the shipped semantics;
      MemoryModel.md updated for the shared-vs-exclusive borrow rule; a tour
      `ConstDemo` (optional, mirrors `ReflectionDemo`) if it adds clarity.

**Deliverable:** const is documented, reflectable, and rendered by tooling.
**Acceptance:** docs reviewed; a reflection test reads the CONST bit; cajetadoc
output shows `const` on a fixture.

---

## 10. Sequencing & risk

- **Order:** CONST-0 → 1 → 2 → 3 → 4 → 5 → 6 → 7. Phases 1–3 are correctness
  (local blast radius). Phases 4–5 are pure-upside optimization (no new errors).
  Phase 6 is the high-value, high-churn borrow integration — flag-gated last.
- **Biggest risk:** const-correctness virality in the stdlib (CONST-6). Mitigated
  by flag-gating and annotating incrementally; never suppress, always annotate.
- **Cheapest big win:** CONST-4 (`const static` → `constant` global) — small,
  isolated, measurable IR reduction, no new errors. Good first real deliverable
  after the CONST-1 plumbing.
- **Grammar regens required:** CONST-1.4 (variableModifier), CONST-3.2
  (const member functions). Batch the ANTLR regeneration.

## 11. Open questions

- Const reference *spelling*: reuse plain-borrow `T` + `const` qualifier, or a
  distinct sigil? (Lean: `const T`, no new sigil — fits the existing `#T`/`T`
  vocabulary.) — resolve in CONST-0.1.
- Should `const` imply `final` static dispatch for methods? (Lean: no, keep
  orthogonal.) — CONST-0.3.
- `constexpr`/compile-time evaluation: separate future plan, not here.
