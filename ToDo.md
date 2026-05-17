# ToDo

Working tracker for the next chunks of compiler work. Replaces the per-rollout status files now that the struct/view + memory-model rollouts are both complete (archived in `cajeta-docs/history/`).

Convention: each entry is a brief description, why it matters, where it bites today, and any pointer at where the design discussion lives. Mark `[x]` when complete; promote items into a session/rollout doc if the work warrants one.

---

## Active design discussion — Unified class model + Optional/Stream

**Status:** in design. No code yet. Pivoted mid-design from "land Optional first" to "unify class + struct first, then Optional rides on top." Conversation traced through Java's Optional, the Java/Rust/Scala/Swift/Kotlin/Haskell/C++ surveys, then the realization that most S6–S11 contortions trace back to the struct/class split. New target: drop the split, use stack/heap allocation per use site, generalize the S6–S11 machinery to live under `CajetaClass`.

### Committed decisions (sealed)

- **One unified `class` keyword.** `struct` retires (deprecation alias during migration).
- **`heap MyClass(args)` / `stack MyClass(args)` as mandatory expression prefixes** for allocation. Bare `MyClass(args)` is a compile error. Relax later only if real ergonomic demand appears.
- **`new` keyword retires** (deprecation alias during migration, then deleted).
- **`MyClass x;` declares a null reference.** Definite-assignment analysis (see open Q below) prevents reading before assignment.
- **Storage-agnostic type system.** `MyClass` is one type whether stack- or heap-allocated. Borrow checker tracks storage as metadata for escape analysis; the type system stays uniform.
- **Class instances always pass and return by pointer, never by value.** Sidesteps C++'s object-slicing problem entirely. Same vtable, same dispatch, polymorphism works for stack and heap uniformly.
- **Vtable slot at offset 0 on every class instance** (stack or heap). 8-byte cost worth uniform dispatch.
- **`extends` works for both stack- and heap-allocated classes.** No difference in dispatch, inheritance, or interface conformance — only in lifetime (which the borrow checker handles).
- **`return this` returns a pointer** — no copy under pointer-only return semantics. Builder pattern works identically across storage modes.
- **`view` keyword stays separate** — views are typed overlays onto byte buffers, conceptually different from value aggregates.
- **`T[]` stays as primitive** — ultra-fast, vtable-free, length-prefixed.
- **Optional, Iterator, Stream live in `runtime/src/cajeta/lang/`** — fundamental types, not collection machinery.
- **Combinator DRY via abstract base classes + multi-inheritance** — cajeta already supports `class C extends A, B` (verified in grammar, visitor, type resolver, and `DynamicDispatchTests.multipleInheritanceDispatchesCorrectly`). Abstract base classes hold concrete combinator bodies; implementers inherit via `extends`. No need for default methods on interfaces.
- **Single Stream<T> abstraction — no separate Iterable / Iterator layer.** Abstract `next()` returning `Optional<T>`. Concrete combinator implementations (map/filter/flatMap/take/skip/fold/count/forEach/collect/findFirst/anyMatch/allMatch/noneMatch/...) all built on top of `next()`. Everything implementing the pull protocol — leaf nodes (ArrayStream, HashMapEntryStream), wrapper nodes (MapStream, FilterStream, TakeStream), single-shot streams (Optional) — extends `Stream<T>`.
- **Collections produce streams via `stream()`, not by extending Stream directly.** Avoids the cursor-in-collection problem (concurrent iterators trampling each other, forgot-to-reset bugs). Each collection's `stream()` returns a fresh stream walking its own storage:
  - `ArrayList<T>.stream()` → `Stream<T>` (over `ArrayStream<T>`)
  - `HashMap<K,V>.entries() / .keys() / .values()` → `Stream<Pair<K,V>>` / `Stream<K>` / `Stream<V>`
  - Optional itself IS a Stream (single-shot); no separate `stream()` method needed.
- **Call sites go through `.stream()` for collections:** `list.stream().map(fn).filter(p).count()`. The `list.map(fn)` shorthand from the prior two-level design is sacrificed for conceptual simplicity (one type, one role, no abstract-class delegation chain).
- **Optional<T> extends Stream<T>** with single-shot `next()`. Inherits the full combinator vocabulary. Overrides `map`/`filter`/`flatMap` with covariant `Optional<U>` returns for cardinality-1 preservation. Carries cardinality-1-aware methods (isPresent, get, orElse, expect, contains, exists, forall, fold, inspect, ifPresent, ifPresentOrElse, or, orElseGet, orElseThrow) that aren't on Stream.
- **Children may override any inherited combinator** where the implementation can be smarter than the default (e.g., a sorted-stream wrapper's findFirst returns the head without walking, Optional's covariant map). Standard subclass behavior.
- **Stream<T> as both concrete-impl class and protocol type.** Consumers can type against `Stream<T>` directly; implementers extend it. No separate interface layer needed (no analog to Java's `Iterable<T>` + `Iterator<T>` + `Stream<T>` triple split — that exists only for backward compatibility we don't have).
- **Iterator<T> as a separate concept retires.** Naming convention: every implementer is `*Stream` (ArrayStream, HashMapEntryStream, MapStream, FilterStream, etc.). No `*Iter` suffix.
- **For-loop desugaring** lowers `for (T x in s)` to `Stream<T>.next()` in a loop until `Optional<T>.None()`.
- **Lambda parameter types are bare structural function types** in stdlib signatures — no `Predicate<T>` / `Consumer<T>` / `Function<T,U>` named wrappers.
- **Java method names** for Optional/Stream combinators (map, flatMap, filter, orElse, ifPresent, etc.).
- **`get()` / `expect(msg)` on Optional None throws a catchable exception** (CAJETA_ERROR_NONE_UNWRAP); matches the existing error model.
- **Stream is sync only for v1.** Async stream is a separate type when the fiber runtime lands; parallel stream is a separate entry (`coll.parIter()`) integrated with the fiber pool. No `.parallel()` flag (universally regretted in Java).
- **No backpressure / error channel / reactive ops in v1 Stream** — those are async-stream concerns.

### Open questions (pinned)

#### Q1 — Default methods on interfaces — CLOSED, not landing

**Context:** Stream<T>'s combinator vocabulary needs to be inherited by every implementer (Optional, ArrayIter, ...) to avoid copy-paste duplication. Earlier draft proposed default methods on interfaces as the solution. Closed once it was confirmed cajeta already supports multi-inheritance (grammar + visitor + type resolver + `DynamicDispatchTests`) — abstract base classes + multi-inheritance accomplish the same DRY win without new compiler work.

**Resolution:** use `AbstractStream<T>` abstract class for the concrete combinator bodies; implementers extend it (`class Optional<T> extends AbstractStream<T>, AbstractHashable<T>`). Stream<T> stays as a thin interface (protocol marker). No new compiler feature.

#### Q2 — Covariant return types on overrides — DECIDED: land as a feature

Schedule the half-session work to support covariant returns. Probe cajeta's current state first (hash-based vtable may already permit it), but commit to landing it explicitly with test coverage so Optional's `Optional<U> map() override` reliably works.

**Implementation:** relax the signature compatibility check from "exact return type match" to "implementer's return type must be assignment-compatible with the base's return type." Vtable still types as wider; concrete-receiver call sites see the narrower override.

#### Q3 — Definite-assignment analysis rules — DECIDED

**Q3a — Keep definite-assignment analysis.** `MyClass x;` declares a null reference; reading before assignment is a **compile error**. Forward-flow analysis tracks each local as DA (definitely-assigned) or NYA (not-yet-assigned).

**Q3b — Constructor field init: Java semantics.** All fields zero-initialized (null for class refs, 0 for primitives) when the instance is allocated. Constructors may leave fields untouched — they stay zero/null. No static check on field init in constructors.

**What counts as assignment:**
- `x = expr;` (direct assignment, in any form: stack/heap construction, borrow, `#` move)
- Anything that re-binds `x`

**What does NOT count:**
- Passing `x` to a method as an argument (no out-params in cajeta; methods get a pointer but can't rebind the caller's local)
- Reading a field of `x` (`x.field`) — this REQUIRES `x` to be DA already

**Loop rule (Java JLS §16):**
- A variable assigned only inside a loop body is **NOT** DA after the loop (loop may not execute, may exit via break before assignment).
- Provably-infinite loops with DA at every exit are technically DA-after, but v1 doesn't bother with the analysis. Workaround: assign before the loop.

**Conditional rule (if/else, switch):**
- DA after `if/else` iff DA in BOTH branches. Missing else → NOT DA.
- DA after `switch` iff DA in every case AND (default case assigns OR switch is exhaustive over an enum).

**Special forms:**
- `return` / `throw` / `break` / `continue` — code after is unreachable; trivially DA for everything.
- `try/catch` — variables assigned in `try` are NYA in `catch` (assignment may have thrown); DA after the try/catch iff DA in both `try` block AND every `catch` block.
- `while(true)` with `break` — DA after the loop equals intersection of DA-at-every-break.

**Implementation:** new forward-flow analysis pass on scope-tracked locals. Existing `Scope` infrastructure (path-borrow tracker, move-marked-names set) is the shape to mirror — add a third set for "definitely-assigned." Sized comparable to one of the S6-S11 sessions.

#### Q4 — Factory return semantics for stack-allocated values — DECIDED: RVO / sret slot

Caller pre-allocates the return body; factory writes into the caller-provided slot. No heap, no escape error. The S6.7 / S9.5.5 struct-return-by-value machinery carries forward unchanged into the unified-class model.

#### Q5 — `struct` keyword fate — DECIDED: drop entirely

One-cycle deprecation alias during migration (`struct` keyword treated as `class`), then retire. Cleanest end state.

#### Q6 — `stack` keyword optional vs required for local stack allocation

**Context:** committed to "every allocation says where" — no bare `MyClass(args)`. Question is whether `stack` is mandatory at every allocation site or whether some shorthand emerges later. Currently committed as mandatory.

**Status:** committed as required for v1; revisit if ergonomic pressure appears.

#### Q7 — `override` marker — DECIDED: `@Override` annotation, optional, lint-checked

Java's exact pattern. Annotation rather than keyword; optional but recommended; lint warning when overriding a concrete inherited method without it (catches typo'd signatures that silently create new methods instead of overriding). No grammar change beyond the annotation declaration.

#### Q8 — `default` keyword on interface methods — CLOSED, retires

**Context:** was needed if default methods on interfaces landed (Java-style explicit keyword). Closed because Q1 closed — no default methods on interfaces, no need for the keyword.

#### Q9 — Sequencing of the rollout — DECIDED

Phased order:

1. **Unified-class rollout** — drop struct/class split, `heap`/`stack` syntax, generalize S6–S11 machinery, retire `new` keyword, drop `struct` keyword (deprecation alias during migration). Several sessions; the big foundational pivot.
2. **Compiler features for stdlib enablement** — definite-assignment analysis (Q3) and covariant return types (Q2). Each its own focused session.
3. **Live-borrow tracker** (Q10) — extends path-borrow tracker to live read-borrows; catches iterator-invalidation at compile time before any iteration code ships.
4. **Stdlib rollout** — Pair, Optional, Stream, AbstractStream + the per-stream wrappers (MapStream/FilterStream/...), ArrayStream (intrinsic), ArrayList, HashMap, HashSet, Collector<T,R> + built-in Collectors, for-loop desugaring through Stream.

#### Q10 — Live-borrow tracker for iterator invalidation — DECIDED: land before stdlib

Iterator invalidation is a compile-time error from day one. The live-borrow tracker extends the existing path-borrow machinery from "moved paths" to "paths currently borrowed by some still-in-scope owner." When a stream borrows from a collection (`list.stream()` registers list's path as live-borrowed by the resulting ArrayStream), any write through that path while the stream is alive is rejected. Lands in the sequencing right after the compiler features (Q9 phase 3), before any stdlib iteration code ships.

**Implementation estimate:** comparable to one of the S6-S11 sessions. Adds a live-borrow registry on `Scope`; registration at struct-field-binding-from-borrow sites; release at the borrower's scope exit / move-out; check at any write site (assignment, index-store, mutating method call).

#### Q11 — Optional method additions — DECIDED

Landing in v1:
- **`inspect((T) -> void fn)`** (Rust) — apply side effect, return same Optional for chaining
- **`fold(() -> R ifNone, (T) -> R ifSome)`** (Scala) — expression-form extraction

Not landing:
- `expect()` — Java's `orElseThrow(() -> Exception)` already covers the "logging or other error handling on unwrap-of-None" case; no need for a separate name.
- `contains(v)` / `exists(pred)` / `forall(pred)` — not selected.

#### Q12 — Stream<T> operator surface for v1 — DECIDED: richer set

Landing in v1:
- **Intermediates (lazy):** `map`, `flatMap`, `filter`, `take`, `skip`, `peek`, `distinct`, `enumerate`, `takeWhile`, `dropWhile`, `zip`, `chain`, `concat`, `sorted`, `windowed`
- **Terminals (eager):** `forEach`, `fold`, `reduce`, `count`, `sum`, `findFirst`, `anyMatch`, `allMatch`, `noneMatch`, `toArray`, **`collect(<target collection type>)`** — polymorphic, supporting arrays, lists, sets, maps, and user-defined collections (see Q17 for the API shape)
- **`groupBy`** (Scala / Kotlin-style) — group elements by key function into a Map<K, List<T>>

Real consumers later may add things like cursor windows, distinctBy, etc.; the v1 surface is rich enough to be useful out of the box.

#### Q13 — How `T[].stream()` attaches to array types — DECIDED: compiler intrinsic

Visitor recognizes `arr.stream()` as a special form and lowers it to a freshly-allocated `ArrayStream<T>` walking the array. `T[]` stays a primitive type; no Array class needed in the stdlib. Cleanest separation — array primitive in compiler, iteration class in stdlib.

#### Q16 — `expect()` lambda signature — DECIDED: drop `expect()` from Optional

Java's `orElseThrow(() -> Exception fn)` already covers the "side-effect-y unwrap that throws a custom exception" case. Drop `expect()` from the v1 surface; users wanting that pattern call `orElseThrow` with a lambda.

#### Q17 — Polymorphic `collect()` API — DECIDED: Collector<T,R> pattern

Java's mature `Collector` pattern. Adds one stdlib abstraction.

```cajeta
public abstract class Collector<T, R> {
    public abstract R supply();                        // create empty container
    public abstract void accumulate(R container, T element);
    public R finish(R container) { return container; } // optional final transform
}

// Stream<T>:
public <R> R collect(Collector<T, R> c) {
    R container = c.supply();
    for (T x in this) { c.accumulate(container, x); }
    return c.finish(container);
}

// Built-in collectors in cajeta.lang.Collectors:
public class Collectors {
    public static <T> Collector<T, ArrayList<T>>     toArrayList() { ... }
    public static <T> Collector<T, HashSet<T>>       toHashSet()   { ... }
    public static <K,V> Collector<Pair<K,V>, HashMap<K,V>> toHashMap() { ... }
    public static <T> Collector<T, T[]>              toArray()     { ... }
    public static Collector<String, String>          joining(String sep) { ... }
    // ... groupingBy, partitioningBy, summing, counting, ...
}
```

User-defined collectors slot in by implementing `Collector<T,R>`. Handles both Collection targets (lists, sets, maps) and non-Collection targets (String joining, summing).

#### Q15 — Pair<K,V> + HashMap iteration shape — DECIDED

- Add `cajeta.lang.Pair<K,V>` — two-field generic class, aggregate-init friendly (`Pair { first: k, second: v }`), with `first()` / `second()` accessors.
- HashMap doesn't extend Stream directly. Provides three explicit entry points:
  - `entries()` returning `Stream<Pair<K,V>>`
  - `keys()` returning `Stream<K>`
  - `values()` returning `Stream<V>`
- Matches Java's approach; explicit reads better than guessing which iteration `for (x in map)` should pick.

#### Q14 — Bridging direction Optional ↔ Stream — DECIDED: Optional IS a Stream

Implicitly settled by the "Optional extends Stream<T>" commit during the Streamable/Stream collapse. Optional has a single-shot `next()` that mutates the `present` flag; first call yields the value, subsequent calls return None. `for (x in opt)` works directly. The "separate OptionalIter wrapper" alternative is retired — keeping Optional immutable wasn't worth the extra type.

---

## Carried-over deferreds from the struct/view rollout

Most of these will **retire** once the unified-class model lands. Marked as such; the rest remain as work items.

### S6.1 — `Foo(args)` constructor-call syntax on a struct segfaults

**Status under unified-class:** **retires**. There's no more "struct receiver"; all classes accept constructor calls via `stack ClassName(args)` / `heap ClassName(args)`.

**Lives in:** `cajeta-docs/history/StructsViewsStatus.md` § "S6.1 limitations called out".

---

### S7.4 — Move out of struct field doesn't clear runtime pointer (double-free risk)

**Status under unified-class:** **generalizes**. The same per-instance ownership-tracking gap exists for moves out of any class field. Still needs to be solved; just becomes a general class-field-move issue, not struct-specific.

**Fix shape:** runtime drop fn consults a per-instance ownership bitmap, OR move-out point writes null into the slot.

**Lives in:** `cajeta-docs/history/StructsViewsStatus.md` § "S7.4 limitations called out".

---

### S7.5 / S10.5 — Class-array element-layout ambiguity (blocks polymorphic interface arrays)

**Status under unified-class:** **active**. Still needs the design call — class/interface arrays store inline values or pointer references? Pick one consistently. Blocks polymorphic interface arrays (`Stream<T>[]`, `Greeter[]`).

**Lives in:** `cajeta-docs/history/StructsViewsStatus.md` § "S7.5 limitations called out".

---

### S8.4 — Direct chaining on a struct-returning method segfaults

**Status under unified-class:** **likely retires**. Chained method calls on class instances under pointer-return semantics are receiver pointers all the way through; the S6.7 repackaging that broke chaining was struct-specific. Confirm during rollout.

**Lives in:** `cajeta-docs/history/StructsViewsStatus.md` § "S8.4 limitations called out".

---

### S5b — `.length()` on T[] returned from a view trips an unrelated alloca path

**Status under unified-class:** **unaffected** (view-specific). Still needs a focused look at the array-length codegen path.

**Lives in:** `cajeta-docs/history/StructsViewsStatus.md` § "S5b limitations called out".

---

### S5b — Variable-size nested views not detected as variable-size

**Status under unified-class:** **unaffected** (view-specific). `CajetaAggregate::isVariableSize` needs recursive checking; construction-time validation needs to walk into nested var-size views.

**Lives in:** `cajeta-docs/history/StructsViewsStatus.md` § "S5b limitations called out".

---

## Done

(empty)
