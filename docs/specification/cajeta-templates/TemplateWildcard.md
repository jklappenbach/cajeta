# Template Wildcards (`<?>`) — Cost-Benefit Analysis

## Motivation

The chain-walk surface in `Stream<T>` (`unwrap`, `cloneChainOver`,
`isStatefulWrapper`, `splittableSize`, `trySplitRoot`) is templated on the
stream's element type `T`. This breaks down across **type-changing wrappers**
— wrappers whose source element type differs from their output element type:

- `MapStream<T, R> extends Stream<R>` — source is `Stream<T>`, base is
  `Stream<R>`.
- `FlatMapStream<U, T> extends Stream<T>` — source is `Stream<U>`, base is
  `Stream<T>`.
- `MapOrSkipStream<T, R>`, `MapOrFallbackStream<T, R>`, `MapOrLogStream<T, R>`
  — same structural shape.

Per Liskov, an override's signature must match the base class's. So
`MapStream<T, R>::unwrap()` is forced to return `Stream<R>`, but its source
field is `Stream<T>`. There is no legal override that preserves the chain
walk, so type-changing wrappers silently inherit `unwrap() = null`. The
parallel driver sees a leaf, short-circuits to sequential drain, and
`.parallel()` becomes a no-op flag with no diagnostic.

Two structural fixes are on the table:

- **Option A — AnyStream**: introduce a non-templated `AnyStream` base class
  carrying the chain-walk surface. `Stream<T> extends AnyStream`. Erasure is
  local to one hierarchy.
- **Option B — Template wildcards (`<?>`)**: introduce a language-level
  "unknown but tracked" type argument. `Stream<?>` becomes a usable type.
  Erasure is a first-class feature.

This document analyzes Option B in depth.

> **Outcome (resolved): Option B was chosen and shipped.** Cajeta now has
> first-class `?`, `? extends Bound`, and `? super Bound` wildcards, PECS
> write-soundness, capture types, and the wildcard-in-hot-loop lint this
> document recommends. The `?` sentinel registers at
> `src/cajeta/type/CajetaType.cpp:454`; wildcard kinds live in
> `CajetaType::WildcardKind` (`Unbounded` / `Extends` / `Super`); capture
> types are `CajetaCapture` (see `docs/CaptureConversion.md`). Coverage:
> `test/parser/TemplateWildcardP{1,2,5,6,7}Tests.cpp` and
> `WildcardLintTests.cpp`. The cost/benefit analysis below is retained as
> the design record; treat its forward-looking framing ("if `<?>` is
> chosen", the staging plan) as the plan that was executed, not pending
> work. The one piece still in flight is the migration from the syntactic
> receiver-identity heuristic to first-class `capture#N` identity
> (Step 6's deeper form) — tracked in `docs/CaptureConversion.md`.

---

## Costs of `<?>`

### Obvious costs

1. **Memory layout has no concrete answer.** `Stream<int32>` and
   `Stream<Foo>` differ in element size, vtable, drop function.
   `Stream<?>` has none of these. Forces either pointer-only access (breaks
   primitives) or boxing/erasure (i.e., AnyStream re-invented under the hood).

2. **Template instantiation breaks.** `TemplateInstantiator` keys on
   concrete type args. `<?>` is not a stable cache key. Either every
   wildcard site demands its own instantiation, or the runtime carries a
   capture identity — neither exists today.

3. **Generic method dispatch needs a `T`.** Methods like
   `collectParallelChain<T, R>` pick `T` from call-site argument types. If
   the argument is `Stream<?>`, `T` is unrepresentable. The compiler must
   defer instantiation per call or commit to erased bodies.

4. **The `#T` (move) marker depends on `T`.** Drop tables resolve
   destructors by static type. A `Stream<?>` field on a splittable share
   has indeterminate drop semantics. Restricting wildcards to
   locals/params loses the chain-cloning use case that motivated this.

### Hidden / non-obvious costs

5. **Capture conversion is a Pandora's box.** `Stream<?> a = …;
   Stream<?> b = a.unwrap();` needs to be sound. The compiler must
   either treat both as the same capture (and prove the constraint) or
   treat them as distinct (and reject most chain-walk code). Java
   solves this with synthetic `capture#N` types. Without that
   machinery, equality, assignability, and method-return relations
   are undefined. Building a capture system is half a type system.

6. **Variance pressure follows immediately.** Once `<?>` exists, the
   next ask is `<? extends T>` / `<? super T>` (PECS). Without them,
   wildcards are read-only or unsafe-write. The chain-walk surface
   needs both reads (`unwrap`) and writes (`cloneChainOver`), so naked
   `<?>` doesn't suffice; you're 70% of the way to full variance.

7. **JIT pessimization that won't show up in tests.** Today every
   chain is fully specialized — `next()` inlines, primitive arithmetic
   stays primitive, LLVM unrolls and vectorizes. Wildcard chains lose
   specialization. The parallel path could end up *slower* than
   sequential, masked by larger workloads. Microbenches catch this;
   integration tests don't.

8. **Drop-chain ABI leakage.** A drop entry today is
   `(addr, type-bound-destructor)`. A `Stream<?>` local needs a fat
   pointer (addr + destructor fn ptr) or a wildcard-aware drop site.
   Either changes ABI in places far from streams: aggregates, vectors,
   closures.

9. **Lambda closure types entangle.** A lambda capturing `Stream<?>`
   has a closure type carrying that wildcard. Two such lambdas aren't
   interchangeable — the capture identity matters. Today closures fold
   cleanly into typed functors; wildcards force per-capture closure
   variants.

10. **Diagnostics regression.** Java's "capture#1 of ?" errors are
    infamous. Cajeta's error infrastructure has no capture concept.
    Either cryptic messages ship or a capture renderer must be built.

11. **One-way door.** Once `<?>` ships, every future library author
    must consider variance for every public generic API. The cognitive
    cost on stdlib design is permanent and broad.

12. **Test matrix expansion.** Every generic API needs wildcard
    coverage — `HashMap<?, ?>`, `ArrayList<?>`, `Optional<?>`, every
    Stream subclass. The current generics test footprint roughly
    doubles, much of it negative-path tests.

### Performance cost in detail

The "pointer-only access invalidates primitives" cost is **not**
universal — it bites where primitives cross the wildcard boundary.

#### Today (specialized for `Stream<int32>`)

```cajeta
public static int32 sum(Stream<int32> s) {
    int32 n = 0;
    while (s.hasNext()) { n = n + s.next(); }
    return n;
}
```

```llvm
loop:
  %has = call i1 @ArrayStream_int32_hasNext(%s)
  br i1 %has, label %body, label %done
body:
  %v = call i32 @ArrayStream_int32_next(%s)   ; inlinable
  %n.next = add i32 %n, %v
  br label %loop
```

After inlining: `load i32, ptr; add; incr ptr`. Vectorizes 8-wide,
unrolls 4x. ~0.5 ns/element.

#### Naive `Stream<?>` usage (the worst case)

```cajeta
public static int32 countAny(Stream<?> s) {
    int32 n = 0;
    while (s.hasNext()) {
        ? v = s.next();    // unrepresentable result type
        n = n + 1;
    }
    return n;
}
```

```llvm
loop:
  %vt = load %vtable*, %vtable** %s
  %hn = load i1 (i8*)*, i1 (i8*)** getelementptr(%vt, 0, 0)
  %has = call i1 %hn(i8* %s)                ; INDIRECT
  br i1 %has, label %body, label %done
body:
  %nx = load i8* (i8*)*, i8* (i8*)** getelementptr(%vt, 0, 1)
  %boxed = call i8* %nx(i8* %s)             ; INDIRECT, alloc per element
  %drp = load void (i8*)*, void (i8*)** getelementptr(%vt, 0, 2)
  call void %drp(i8* %boxed)                ; INDIRECT free
  %n.next = add i32 %n, 1
  br label %loop
```

Indirect calls block inlining, unrolling, and vectorization. Per
element: 2-3 indirect calls + 1 alloc + 1 free. ~50-100 ns/element.
**~100x slower** than the specialized form.

#### Disciplined `Stream<?>` (chain-walk only, downcast at worker boundary)

```cajeta
// Driver does a few virtual calls (unwrap, splittableSize, trySplitRoot)
// ONCE per parallel dispatch — amortized over millions of elements.
// Worker loop is still specialized — same IR as the specialized form.
```

Net cost: a few extra virtual calls per chain dispatch. Trivial.

#### Triggers that slip into the worst case

The naive worst case is reachable through reasonable-looking source code:

1. **Generic utility helper.**
   ```cajeta
   public static int32 countAny(Stream<?> s) {
       int32 n = 0;
       while (s.hasNext()) { s.next(); n = n + 1; }
       return n;
   }
   ```
   Even with `v` discarded, the boxed allocation still happens.

2. **Heterogeneous collection.**
   ```cajeta
   ArrayList<Stream<?>> registered = heap ArrayList<Stream<?>>();
   registered.add(intList.stream());
   registered.add(nameList.stream());
   int32 i = 0;
   while (i < registered.count()) {
       Stream<?> s = registered.get(i);
       while (s.hasNext()) { s.next(); /* boxed */ }
       i = i + 1;
   }
   ```

3. **Conditional return type.**
   ```cajeta
   public static Stream<?> chooseSource(boolean useInts) {
       if (useInts) return intList.stream();
       return scoreList.stream();
   }
   ```
   The return type collapses to `Stream<?>` because the branches
   disagree on element type.

4. **Field-shaped storage (drop-chain cost, not loop cost).**
   ```cajeta
   public class StreamMonitor {
       Stream<?> source;   // destructor must virtual-dispatch
   }
   ```

5. **"Just for logging" / debug drain.**
   ```cajeta
   public static void debugDrain(Stream<?> s, String label) {
       int32 n = 0;
       while (s.hasNext()) { s.next(); n = n + 1; }
       System.out.println(label + ": " + n);
   }
   debugDrain(bigIntStream, "warmup");   // boxed 10M times
   ```

6. **Discipline failure inside the driver itself.**
   ```cajeta
   public static <R> R reduceParallelChain<T, R>(Stream<T> head, ...) {
       Stream<?> cur = head;
       while (cur.unwrap() != null) { cur = cur.unwrap(); }
       Stream<?> share = ...;
       if (share.hasNext()) {
           ? probe = share.next();   // ← boxed; one-line peek leaks the cost
       }
   }
   ```

**Structural problem**: `next()` on a wildcard-typed receiver is
syntactically legal even when the result type is unrepresentable. The
compiler has to produce something — and "something" means boxed +
virtual + drop. There is no error at the point of harm; performance
loss is invisible at the source level.

---

## What `<?>` uniquely enables

The reason to pay the cost is the breadth of capability that becomes
expressible.

### 1. Heterogeneous containers of generic instantiations

```cajeta
// With <?>:
ArrayList<Stream<?>>          pipelines = ...;
HashMap<String, Optional<?>>  cache = ...;
ArrayList<Future<?>>          pending = ...;

// Without <?>, each generic needs a bespoke base:
ArrayList<AnyStream>   pipelines;
ArrayList<AnyOptional> cache;
ArrayList<AnyFuture>   pending;
ArrayList<AnyResult>   results;
```

Every generic in the stdlib eventually wants a heterogeneous container
of its instantiations. The bespoke-base alternative is the
Java-before-generics pattern: many shallow `Object`s instead of one
tracked escape hatch. Bespoke bases compose badly because each one has
its own surface to maintain.

### 2. Bounded wildcards (variance) — strictly impossible without `<?>`

```cajeta
// Producer side — read-covariant:
public static int64 sumAll(Stream<? extends Number> s) {
    int64 acc = 0;
    while (s.hasNext()) {
        Number n = s.next();
        acc = acc + n.toLong();
    }
    return acc;
}
sumAll(intStream);      // Stream<int32>   — accepted
sumAll(longStream);     // Stream<int64>   — accepted
sumAll(boxedStream);    // Stream<Number>  — accepted

// Consumer side — write-contravariant:
public static <T> void drainInto(Stream<T> src, Collection<? super T> sink) {
    while (src.hasNext()) { sink.add(src.next()); }
}
// Caller can drain a Stream<Cat> into a Collection<Animal>.
```

Bespoke bases cannot express either pattern. The closest is "take
AnyStream, downcast and pray," which defeats static typing. PECS is
the reason every serious collection library eventually wants
wildcards.

### 3. Capture identity — "same unknown `T`" without naming `T`

```cajeta
public static <T> void zip(Stream<T> a, Stream<T> b)         // SAME T
public static    void scan(Stream<?> a, Stream<?> b)         // unknown, possibly different
```

The wildcard provides a third position: "tracked but unknown,"
distinct from both specific-`T` and fully-erased. Bespoke bases
collapse these two signatures to the same shape.

### 4. Method-level flexibility without forcing caller commitment

```cajeta
public static int32 length(Stream<?> s) { ... }

length(intStream);
length(stringStream);
length(carStream);
```

Without `<?>`, the caller must either commit to a template
instantiation OR upcast through a bespoke base. Wildcards let "I
don't care what T is" be a first-class type rather than a workaround.

### 5. Type-keyed registries (factories, plugin systems, codecs)

```cajeta
HashMap<String, Supplier<?>> factories = ...;
factories.put("int",    () -> heap Counter<int32>());
factories.put("string", () -> heap Counter<String>());

Supplier<?> f = factories.get(key);
Object made = f.get();
```

Standard pattern across CLIs, codecs, plugin systems, dependency
injection containers. The per-generic alternative is `AnySupplier`,
`AnyCounter`, `AnyEncoder`, `AnyParser` … each a tiny pseudo-class
with no real semantics.

### 6. Reflection-shaped APIs

```cajeta
public static void dumpChainShape(Stream<?> head) {
    Stream<?> cur = head;
    int32 depth = 0;
    while (cur != null) {
        System.out.println(depth + ": " + cur.getClass().getName());
        cur = cur.unwrap();
        depth = depth + 1;
    }
}
```

Debug walkers, serializers, equality across heterogeneous keys,
structural pretty-printers — all want "any instantiation, I won't
touch the elements." Bespoke bases force this to be per-type.

### 7. Variance features compound for free

Declaration-site `out T` / `in T` (Kotlin/Scala-style) sits on the
same capture machinery. If `<?>` lands, declaration-site variance is
a 30% top-up later. If bespoke bases accrete for years and *then*
variance is wanted, the type-system work must still be done AND the
bespoke-base legacy persists.

---

## Linting analysis

Lint **can** catch the performance footgun cases:

- `Stream<?>` materializing `T` inside a loop ("likely boxing").
- Wildcard return crossing a hot-path boundary.
- Wildcard field in a small frequently-allocated class ("drop becomes
  virtual").
- Discarded wildcard `next()` result ("the box allocates even though
  the value is unused").

Lint **cannot** catch:

- Capture conversion soundness — type-checker territory.
- TemplateInstantiator cache aliasing — build-infrastructure
  territory.
- Drop-chain ABI changes — compiler-internal, not stylistic.

Linting reduces the surface area of misuse; it does not reduce the
foundational implementation cost. The type-system depth is
unavoidable.

---

## AnyStream as a comparative reference

A non-templated `AnyStream` base class makes the discipline structural:

```cajeta
public class AnyStream {
    public AnyStream unwrap()                          { return null; }
    public AnyStream cloneChainOver(AnyStream src)     { return this; }
    public boolean isStatefulWrapper()                 { return false; }
    public int64 splittableSize()                      { return 0; }
    public Pair<AnyStream, AnyStream> trySplitRoot()   { return null; }
}

public class Stream<T> extends AnyStream {
    public T next();
    public boolean hasNext();
    // ... element-touching methods stay on Stream<T>
}
```

Because `AnyStream` has no `next()`, misuse is a compile error:

```cajeta
AnyStream a = someChain;
a.next();      // ← compile error: AnyStream has no method next()
```

This is the load-bearing design property: misuse is caught by the
type system, not by convention or by a profiler.

**The trade-off**: AnyStream is a point fix. The pattern, once
normalized, will be copied to every generic in the stdlib
(`AnyOptional`, `AnyFuture`, `AnyResult`, `AnySupplier`,
`AnyConsumer`, `AnyCollection`). Eventually, you have wildcards —
just hand-rolled and inconsistent.

---

## The honest framing

The decision is not really about the parallel chain walk. It is about
language posture.

- **Bespoke-base posture**: Cajeta has no general erasure. Each
  generic gets a hand-rolled escape hatch as the need arises. The
  stdlib accretes `Any*` classes over time. The language stays
  simpler; the stdlib grows wider. Erasure is invisible at the
  language level but pervasive in the codebase.

- **`<?>` posture**: Cajeta has type-system support for "unknown but
  tracked." One mechanism handles heterogeneous containers, bounded
  variance, capture identity, and reflection-shaped APIs. The
  language is more complex; the stdlib stays narrower. Erasure is
  concentrated in the compiler where it is visible and maintained.

The bespoke-base posture solves the immediate task with the smallest
intervention. The `<?>` posture pays an upfront cost — call it 2-3
weeks of focused compiler work — and that cost is amortized across
every generic the language will ever ship.

If `<?>` is the chosen direction, the misuse-footgun cost is
real but addressable through linting at edit and compile time. The
type-system depth (capture conversion, drop-chain ABI,
TemplateInstantiator cache, diagnostics) is not addressable through
linting — that work is unavoidable.

---

## Recommended staging if `<?>` is chosen

Stage the work to deliver value incrementally rather than blocking
the parallel chain walk on a full implementation:

1. **Type-checker plumbing** at `src/cajeta/type/CajetaType.cpp:454`:
   parse `<?>`, basic assignability rules, no variance yet. Use it
   nowhere. Lands as a feature flag with a backout path.
2. **Drop-chain ABI**: virtual destructor on `Stream` (or fat-pointer
   convention for wildcard-typed locals). Validate against existing
   tests.
3. **TemplateInstantiator**: wildcard keys treated as an erased
   instantiation, separate cache bucket.
4. **Lint pass**: wildcard-in-hot-loop detector. Lives in the
   compiler, not only the linter, so users see it inline.
5. **Migrate parallel chain walk** to use `<?>`. ~50 LOC change once
   the foundation is in place.
6. **Bounded wildcards** (`? extends`, `? super`) as a separate later
   step. Do not block #5 on it.

Steps 1-4 are the load-bearing work. Step 5 is the immediate payoff
that motivated the investigation. Step 6 is the long-term dividend.

---

## Summary

| Dimension | Bespoke-base (AnyStream) | `<?>` wildcards |
|---|---|---|
| Solves chain-walk parallel | Yes | Yes |
| Heterogeneous containers | Per-generic Any* class | First-class |
| Bounded variance (PECS) | Impossible | Requires `<?>` foundation |
| Capture identity | Cannot express | Expressible with capture |
| Method-level flexibility | Through bespoke upcast | Native `<?>` parameter |
| Type-keyed registries | Per-generic Any* class | First-class |
| Reflection-shaped APIs | Per-type bespoke | Generic |
| Compiler infrastructure cost | Low (~1 week) | High (~2-3 weeks) |
| Performance footgun surface | None — type system enforces | Real — needs linting |
| Future variance compatibility | Re-do erasure work | 30% top-up |
| Stdlib growth pattern | Wide (one Any* per generic) | Narrow (one mechanism) |

The bespoke-base posture is right-sized for the immediate task in
isolation. The `<?>` posture is right-sized for the trajectory of a
language that will ship many generic APIs over time.
