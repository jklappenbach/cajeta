# Method-Level Templates

Status: design draft, 2026-05-18. Triggered by stdlib gaps surfaced
during the P2 collections rollout: `Stream<T>.fold<R>`,
`Stream<T>.collect<R>(Collector<T, R>)`, and the
`Optional<int32>.Some(42)` templated-static-factory call syntax all need
type parameters introduced *on the method itself*, separate from the
type parameters introduced on the enclosing class.

Outcome (proposed): allow methods to introduce their own `<...>` type
parameter list, with one hard requirement — **a method that introduces
method-level type parameters must be declared `final` (instance
method) or `static` (no receiver)**. The modifier is mandatory and
the compiler rejects declarations that lack it.

Underneath that surface requirement, the method occupies no vtable
slot regardless of the modifier — the templating itself excludes it
from dispatch (a vtable slot is one function pointer; a method-
templated declaration is a *family* of bodies, one per type-arg
tuple). The mandatory `final` / `static` is a clarity rule rather
than a mechanism: it surfaces the non-virtuality at the declaration
site so readers don't have to reason about why a templated method
isn't dispatched.

## Background

Cajeta's class-level templates already work: `class Stream<T>`, `class
Optional<T>`, `class Pair<K, V>`. Each instantiation is monomorphized
(C++ semantics, not Java erasure — see [[project_language_basis]] and
[[feedback_never_call_templates_generics]]). `Stream<int32>` and
`Stream<Counter>` are distinct types with distinct vtables and distinct
method bodies.

What does *not* work today is type parameters introduced by the method
itself:

```cajeta
public class Stream<T> {
    // Class-level T is in scope, but R is new — introduced by fold.
    public <R> R fold(R seed, (R, T) -> R fn) { ... }   // ← can't parse
}

public class Optional<T> {
    // Static factory, but the call site wants to write Some(42) and
    // have T inferred as int32. Today's grammar can't bind T here.
    public static <T> Optional<T> Some(T value) { ... }  // ← can't parse
}
```

Both forms appear in `ToDo.md` Priority 2 §§ 2–3 and §7, and in
`Features.md` row L-22. The rest of the stdlib has been written around
their absence: `Stream<T>.reduce(T, (T, T) -> T)` is the same-type
degenerate case of fold; `Optional<int32>` callers write `heap
Optional<int32>(true, 42)` instead of `Optional<int32>.Some(42)`.

## The vtable problem

This is the question worth getting right, because Java solves it with
erasure (which Cajeta has explicitly rejected) and C++ sidesteps it by
forbidding the construct.

Concrete scenario: `Stream<T>` is the base class. `ArrayStream<T>`,
`MapStream<T, U>`, `FilterStream<T>`, etc. all extend `Stream<T>` and
override `next()`. A vtable slot is one function pointer per virtual
method per concrete instantiation of the class.

If `fold<R>` were virtual:

- `Stream<int32>`'s vtable would need a slot for `fold<R>` — but
  `fold<R>` is *not one function*, it's a family indexed by `R`. There
  is no single function pointer that covers `fold<int64>`,
  `fold<String>`, `fold<Pair<int32, int32>>`, etc.
- Subclasses overriding the slot would have the same problem squared:
  `ArrayStream<int32>` would need to provide an override for *every*
  `R` the program ever instantiates with, but the set of `R`s isn't
  known until link time.

C++ forbids `template <typename R> virtual R fold(...)` for exactly
this reason. Java sidesteps it because erasure means there's only one
`fold` body anyway — the type parameters vanish at runtime. We've
already chosen monomorphization, so we inherit C++'s problem.

## The fix: require `final` (instance) or `static`

**A method-templated declaration MUST be marked `final` (if instance)
or `static` (no receiver). The compiler rejects declarations that
lack it.** Underneath the modifier, the templating itself is what
actually excludes the method from the vtable — the modifier is the
explicit marker that surfaces that fact at the declaration site.

### Why mandate the modifier when the templating alone would suffice?

A method-templated declaration is non-virtual no matter what. The
compiler could implicitly exclude it from the vtable and silently
shadow any inherited same-name method. But that would be surprising —
in Java/Cajeta, instance methods are virtual by default, and readers
have strong intuitions about "subclass declarations override." The
mandatory `final` (or `static`) modifier makes the opt-out explicit:
the reader sees `final <R>` and immediately knows this method
participates in neither vtable dispatch nor override resolution.

Worth knowing: `final` in Java/Cajeta does **not** by itself remove
a method from the vtable. It only prevents further overrides past
that point in the hierarchy. Here we're co-opting it as the
declaration-site marker for a stronger property (no vtable slot at
all) that the templating itself enforces. The rule "`final` is
required" is closer to a lint-style ergonomic check than a semantic
load-bearing constraint — but readers benefit, and it cleanly
prevents the surprising shadowing scenario described below.

### What's allowed and what isn't

```cajeta
public class Stream<T> {
    // OK — `final` instance method with method-level R. Non-virtual,
    // no vtable slot, monomorphized per (T, R) at the call site.
    public final <R> R fold(R seed, (R, T) -> R fn) {
        R acc = seed;
        Optional<T> o = this.next();   // ← virtual call on next() works normally
        while (o.isPresent()) {
            acc = fn(acc, o.get());
            o = this.next();
        }
        return acc;
    }
}

public class Optional<T> {
    // OK — static + method-level type param. No receiver, no
    // dispatch concern, monomorphized per U at the call site.
    public static <U> Optional<U> Some(U value) {
        return heap Optional<U>(true, value);
    }
}

public class Stream<T> {
    // ERROR — method-level <R> requires `final` (or `static` if no
    // receiver). Diagnostic CAJETA_ERROR_METHOD_TEMPLATE_NOT_FINAL.
    public <R> R fold(R seed, (R, T) -> R fn) { ... }
}
```

The diagnostic should read approximately:

```
error: method 'fold' introduces method-level type parameter 'R'
       but is not declared 'final' or 'static'. Method-level
       templates are non-virtual (they occupy no vtable slot) and
       must be marked explicitly to surface that property at the
       declaration site. See cajeta-docs/stdlib/MethodLevelTemplate.md.
       fix: add 'final' modifier (or 'static' if no receiver is
       needed).
```

### Constructors and operators excluded

Constructors and operator-overload declarations cannot carry method-
level type parameters — the construct/operator entry shapes don't
compose with per-call monomorphization. Construct an instance of a
templated class by writing the class-level args explicitly (`new
Box<int32>(42)`) and rely on diamond inference where supported.

### No shadowing case to worry about

Because `final` (or `static`) is mandatory and `final` prevents
subclass overrides anyway, the surprising shadowing scenario from
earlier drafts of this doc cannot arise. A subclass cannot redeclare
a parent's `final <R> foo(...)` method without a normal override
error. A subclass that needs different behavior overrides the virtual
primitive the templated method calls through to (the template-method
pattern: `Stream.fold<R>` calls `next()`; `ArrayStream<T>` overrides
`next()`).

## Syntax

The grammar addition is small. In `methodDeclaration`, allow a
`typeParameters` list immediately before the return type, mirroring
Java:

```
methodDeclaration
    : modifier* typeParameters? type Identifier formalParameters
      throwsClause? methodBody
    ;
```

Where `typeParameters` is the same nonterminal already used for
`classDeclaration`:

```
typeParameters : '<' typeParameter (',' typeParameter)* '>' ;
typeParameter  : Identifier ('extends' typeBound)? ;
```

Examples:

```cajeta
// Final instance method, one method-level type param.
public final <R> R fold(R seed, (R, T) -> R fn);

// Static factory, one method-level type param.
public static <U> Optional<U> Some(U value);

// Static utility, two method-level type params.
public static <K, V> Pair<K, V> pair(K k, V v);

// Bounded method-level param.
public static <T extends Comparable<T>> T max(T a, T b);

// Final instance method, method-level param distinct from class T.
public final <R> Stream<R> mapTo((T) -> R fn);
```

Call sites can either pass type arguments explicitly or rely on
inference from the value arguments:

```cajeta
// Explicit method-level type args.
int64 sum = numbers.fold<int64>(0L, (int64 acc, int32 x) -> acc + x);
Optional<int32> o = Optional<int32>.Some<int32>(42);

// Inferred from arguments (preferred; matches Java/Kotlin/Swift
// ergonomics).
int64 sum = numbers.fold(0L, (int64 acc, int32 x) -> acc + x);
Optional<int32> o = Optional<int32>.Some(42);

// On a static method whose receiver class is itself templated, the
// class-level args are written normally; method-level args follow the
// method name (and may be omitted if inferable).
Pair<String, int32> p = Pair.pair("a", 1);                    // both inferred
Pair<String, int32> p = Pair.pair<String, int32>("a", 1);     // both explicit
```

## What this solves

Each item below is currently blocked or working around the gap.

### 1. `Stream<T>.fold<R>` — cross-type accumulator

Today: `reduce(T seed, (T, T) -> T fn)` only — the accumulator must
share the element type. You can sum an `int32` stream into an `int32`
total but not into an `int64`. You can't fold a `Stream<Counter>`
(class T) into an `int32` count without first mapping to a primitive
stream and then reducing.

With method-level templates:

```cajeta
public class Stream<T> {
    public final <R> R fold(R seed, (R, T) -> R fn) {
        R acc = seed;
        Optional<T> o = this.next();
        while (o.isPresent()) {
            acc = fn(acc, o.get());
            o = this.next();
        }
        return acc;
    }
}
```

Call-site examples:

```cajeta
// Sum int32 stream into int64 to avoid overflow.
int32[] xs = { 1, 2, 3, 1000000000, 1000000000 };
ArrayStream<int32> s = heap ArrayStream<int32>(xs, 5);
int64 total = s.fold(0L, (int64 acc, int32 x) -> acc + (int64) x);

// Count Counter instances satisfying a predicate (no map needed).
Counter[] cs = { new Counter(1), new Counter(2), new Counter(3) };
ArrayStream<Counter> cs_s = heap ArrayStream<Counter>(cs, 3);
int32 above1 = cs_s.fold(0, (int32 acc, Counter c) -> {
    return c.v > 1 ? acc + 1 : acc;
});

// Build a String from a stream of objects.
ArrayStream<Counter> cs_s2 = heap ArrayStream<Counter>(cs, 3);
String joined = cs_s2.fold("", (String acc, Counter c) -> acc + c.v);
```

Monomorphization: each unique `(T, R)` pair produces one specialized
function symbol. `Stream<int32>::fold<int64>`,
`Stream<Counter>::fold<int32>`, `Stream<Counter>::fold<String>` are
three separate bodies, generated on-demand at call sites.

`reduce` stays as the same-type convenience wrapper (no behavior
change, common case stays terse):

```cajeta
public T reduce(T seed, (T, T) -> T fn) {
    return this.fold<T>(seed, fn);
}
```

### 2. `Stream<T>.collect<R>(Collector<T, R>)` — generic terminal accumulator

Today: blocked. Collectors can't even be expressed since the terminal
that consumes them can't be written.

With method-level templates:

```cajeta
public class Collector<T, R> {
    R seed;
    (R, T) -> R accumulator;
    public Collector(R seed, (R, T) -> R accumulator) {
        this.seed = seed;
        this.accumulator = accumulator;
    }
}

public class Stream<T> {
    public <R> R collect(Collector<T, R> c) {
        return this.fold<R>(c.seed, c.accumulator);
    }
}

public class Collectors {
    public static <T> Collector<T, ArrayList<T>> toList() {
        return heap Collector<T, ArrayList<T>>(
            heap ArrayList<T>(),
            (ArrayList<T> acc, T x) -> { acc.add(x); return acc; }
        );
    }

    public static <T, K> Collector<T, HashMap<K, ArrayList<T>>>
        groupingBy((T) -> K keyFn) {
        return heap Collector<T, HashMap<K, ArrayList<T>>>(
            heap HashMap<K, ArrayList<T>>(),
            (HashMap<K, ArrayList<T>> acc, T x) -> {
                K key = keyFn(x);
                if (!acc.containsKey(key)) {
                    acc.put(key, heap ArrayList<T>());
                }
                acc.get(key).add(x);
                return acc;
            }
        );
    }
}
```

Call-site examples:

```cajeta
ArrayList<int32> doubled = src.collect(Collectors.toList<int32>());

HashMap<String, ArrayList<Counter>> byName =
    counterStream.collect(Collectors.groupingBy<Counter, String>(
        (Counter c) -> c.name
    ));
```

`collect<R>` is an instance method on `Stream<T>` and inherently non-
virtual (templated). `Collectors.toList` / `Collectors.groupingBy` are
`static <T> ...` / `static <T, K> ...` factories. Everything in this
stack dispatches statically.

### 3. `Optional<T>.Some(value)` — templated static factory call syntax

Today: `Optional<int32> o = heap Optional<int32>(true, 42);` is the
only way to construct a present `Optional`. It's verbose, exposes the
internal `(boolean present, T value)` constructor shape, and forces
callers to know the storage layout.

With method-level templates *and* the related call-syntax piece
(`TypeName<args>.staticMethod(...)`):

```cajeta
public class Optional<T> {
    public static <U> Optional<U> Some(U value) {
        return heap Optional<U>(true, value);
    }

    public static <U> Optional<U> None() {
        return heap Optional<U>(false, null);
    }
}
```

Call-site examples:

```cajeta
// Inferred from argument.
Optional<int32> o1 = Optional.Some(42);

// Explicit method-level type arg (rarely needed).
Optional<int32> o2 = Optional.Some<int32>(42);

// None has no value to infer from, so the class arg is named at the
// type position; method-level <U> binds to int32 by unification with
// the assignment target's class arg.
Optional<int32> o3 = Optional.None();

// Class T also lexically introduced via TypeName<T>.method syntax (an
// alternative spelling supported by the same grammar work):
Optional<int32> o4 = Optional<int32>.Some(42);
Optional<int32> o5 = Optional<int32>.None();
```

The factory pattern generalizes the same way to `Pair`, `Result`,
`Either`, and any future tagged-union stdlib type.

### 4. Static generic utilities

`max`, `min`, `swap`, `clamp`, etc. — utilities that don't belong on
any one class.

```cajeta
public class Math {
    public static <T extends Comparable<T>> T max(T a, T b) {
        return a.compareTo(b) >= 0 ? a : b;
    }
    public static <T extends Comparable<T>> T min(T a, T b) {
        return a.compareTo(b) <= 0 ? a : b;
    }
    public static <T extends Comparable<T>> T clamp(T x, T lo, T hi) {
        return Math.max(lo, Math.min(hi, x));
    }
}
```

Call-site examples:

```cajeta
int32 a = Math.max(3, 7);              // T inferred as int32
String s = Math.max("apple", "pear");  // T inferred as String
int32 c = Math.clamp(score, 0, 100);
```

Without method-level templates these would either need one overload
per primitive (the C path) or be unrepresentable for class T (the
current state).

### 5. Generic conversion / mapping helpers

```cajeta
public class Stream<T> {
    public <R> Stream<R> mapTo((T) -> R fn) {
        return heap MapStream<T, R>(this, fn);
    }
}
```

`mapTo` is an intermediate combinator, but until method-level templates
land it can only be written by tying R into the receiver type's class-
level parameters (the `MapStream<T, U>` workaround from
`stdlib/Lambdas.md` line 347). With method-level templates, `Stream<T>`
gets a clean `mapTo<R>` that produces a `Stream<R>`.

## Codegen model

Same as class-level templates: lazy monomorphization at the call site.

1. Parser captures the `typeParameters` on the method declaration as a
   `vector<string>` on the `MethodDeclaration` AST node (mirroring how
   class-level template parameters are captured on `CajetaClass`).
2. The method gets no vtable slot — the templating itself excludes
   it. Vtable build skips method-templated declarations entirely.
3. Type checking on the *body* of the method binds the method-level
   T-vars as abstract type variables; they resolve against the same
   scope rules as class-level T-vars and shadow them if names collide
   (with a warning).
4. At each call site, the compiler infers (or accepts explicit) values
   for the method-level type parameters. The (receiver type args ×
   method-level type args) tuple becomes the monomorphization key.
5. The compiler maintains a per-method instantiation cache. On first
   sight of a new key, it specializes the method body — substituting
   the type variables in parameter types, return type, local variable
   types, and `new`/`heap` expressions — and emits a fresh LLVM
   function with a mangled name encoding both class-level and method-
   level args.
6. The call site emits a direct `call` to the mangled symbol. No
   vtable lookup. No dispatch cost.

For static methods, omit the receiver entirely (no `this` parameter,
no class-level instantiation needed if the class itself isn't
templated; if the class is templated, only the method-level args feed
mangling for static methods declared inside it — class-level args on
static methods are independent of any receiver and would conventionally
be declared as method-level too).

Name mangling sketch (illustrative, final form TBD):

```
cajeta.lang.stream.Stream$T=int32$.fold$R=int64$
cajeta.lang.stream.Stream$T=test.Counter$.fold$R=cajeta.lang.String$
cajeta.lang.Optional$.Some$U=int32$              // static, no class args
cajeta.lang.Math$.max$T=int32$
cajeta.lang.Math$.max$T=cajeta.lang.String$
```

## Edge cases

### Bounds

`<T extends Comparable<T>>` works the same as on class-level templates.
The bound is checked at instantiation time against the inferred /
supplied T. Diagnostics point to the call site, not the declaration.

### Inference failure

If the compiler cannot infer all method-level type parameters from
value arguments and the target type, the call must spell them
explicitly:

```cajeta
// Inference fails — no argument constrains R.
Stream.empty();                          // ERROR
Stream.empty<int32>();                   // OK
Optional<int32> o = Stream.empty();      // OK (target type fixes R)
```

### Interaction with virtual `next()`

A `<R> R fold(R seed, ...)` on `Stream<T>` calls `this.next()`, which
IS virtual. The dispatch happens normally — fold itself is non-virtual
(by virtue of templating), but the methods it *calls* are dispatched
per usual. This is the template-method pattern.

### Failed-override-via-templating

If a subclass declares a method-templated signature with the same
name and parameter pattern as a non-templated virtual method on the
base, that's a compile-time error. The declaration cannot fill the
base's vtable slot. Diagnose as "templated method 'foo' cannot
override non-templated 'A.foo' inherited from 'A'; templated methods
do not occupy vtable slots."

### Shadowing across hierarchy

Two templated methods with the same name in a hierarchy are
independent shadows (see § Shadowing above). Emit a warning at the
subclass declaration: "templated method 'foo' shadows 'A.foo'; calls
dispatch on the receiver's static type, not dynamically."

### Method-templated methods on interfaces

Allowed only when the interface declares the method as `default`
(i.e., the implementation lives on the interface; subclasses inherit
the same body and cannot specialize per type-arg). Today Cajeta's
interface support is minimal; revisit when interface defaults land.

## What this does *not* solve

- **Per-lambda type parameters** (`<U> (U) -> U`). Same vtable
  constraint, separate grammar work. See `cajeta-docs/stdlib/Lambdas.md`
  § "Per-lambda type parameters."
- **Higher-kinded types** (`F<_>` where `F` itself is a type
  parameter). Not on the roadmap.
- **Variance annotations** (`<? extends T>`, `<? super T>`). Cajeta's
  monomorphization model makes Java's wildcards mostly unnecessary;
  bounded `<T extends Bound>` covers the vast majority of real cases.

## Implementation phases

The work breaks down naturally across three increments. None require
runtime changes; this is entirely a parse + monomorphize +
mangling story.

### Phase 1 — Grammar + parsing + scope binding

- Add optional `typeParameters` slot to `methodDeclaration` in
  `antlr4/CajetaParser.g4`.
- Wire ANTLR rule output through `Method`/`MethodDeclaration`
  construction; store the method-level type parameter names.
- Bind the method-level T-vars in the method body's scope so they
  appear in parameter types, return type, locals, and `new`/`heap`
  expressions during parse / first-pass type checking.
- Skip vtable-slot assignment for any method whose declaration
  introduces method-level type parameters.
- Detect and reject "templated method overrides non-templated
  virtual"; emit warning for shadowing.
- Add parser tests covering valid and invalid declarations.

Deliverable: declarations parse, are stored, are bound, but emit no
LLVM code yet (calls to method-templated methods are deferred to
Phase 2). Existing tests stay green (no existing method declarations
introduce method-level params).

### Phase 2 — Monomorphization + call-site dispatch

- Extend the type checker to bind method-level type variables in
  scope of the method body during specialization.
- Build the per-method instantiation cache keyed on (receiver class
  args, method-level args).
- Implement inference from value arguments and from the assignment
  target type.
- Emit specialized LLVM functions with mangled names.
- Emit direct (non-vtable) calls at the call site.

Deliverable: `Stream<T>.fold<R>` and similar callable end-to-end with
explicit type args. Inference may be partial in this phase.

### Phase 3 — Static factory call syntax + stdlib uptake

- Allow `TypeName.method<args>(values)` at the call site (Form C —
  type args after the identifier, mirroring `Type<args>` at the
  type-use site). This is the only call-site syntax; Java's
  `TypeName.<args>method(...)` dot-prefix form is rejected.
- Update `Optional.Some` / `Optional.None` / similar stdlib factories.
- Rewrite `Stream<T>.reduce` as a wrapper around `fold<T>`.
- Add `Math.max` / `Math.min` / `Math.clamp` static utilities.
- Add the corresponding parser + dispatch tests.

Deliverable: `Optional<int32>.Some(42)` works; the verbose `heap
Optional<int32>(true, 42)` form remains valid but is no longer the
recommended idiom.

## Spec updates required

Once this lands, these existing notes need to be revised:

- `cajeta-docs/stdlib/Lambdas.md:347` — paragraph saying "method-level
  template ... doesn't exist in Cajeta" needs replacement with a
  pointer to this doc and a corrected example using `mapTo<R>`.
- `cajeta-docs/stdlib/Lambdas.md:14` — "same virtual-vtable reasoning
  as method-level templates" reference can become a forward link to
  this doc's "vtable problem" section.
- `cajeta-docs/stdlib/Views.md:324` — "No method-level generics" needs
  to be relaxed (or kept as a view-specific restriction with its own
  rationale, if views have a separate reason to forbid them).
- `cajeta-docs/stdlib/Streams.md:245-246` — "needs method-level type
  parameters (P2.3)" gets a link to this doc.
- `ToDo.md` § Priority 2 §§ 2, 3, 7 — mark these as unblocked once
  Phase 1 lands.
- `Features.md` row L-22 — flip to implemented once Phase 2 lands.

## Status

Shipped, 2026-05-18. All three phases landed:

- Phase 1 (grammar + parse + capture + final/static enforcement +
  vtable-skip): pinned by `test/parser/MethodTemplateParseTests.cpp`.
- Phase 2 (per-call monomorphization, inferred type args via
  function-type unifier, mid-codegen save/restore around
  instantiation): pinned by `test/parser/MethodTemplateCallTests.cpp`.
- Phase 3 (post-identifier explicit-type-arg call syntax — Form C —
  `receiver.method<TypeArgs>(args)`: covers both
  `Util.identity<int32>(42)` and `b.passthrough<R>(99)`): pinned by
  `test/parser/MethodTemplateExplicitArgsTests.cpp`. Also wired
  `Stream<T>.fold<R>` (pinned by `test/parser/StreamFoldTests.cpp`)
  with `reduce` as a one-line wrapper, and added `Math.max` /
  `Math.min` / `Math.clamp` static utilities (pinned by
  `test/parser/MathTests.cpp`).

Known limitations:

- **`TypeName<TypeArgs>.method(args)` form** (type-name-with-args as
  receiver expression — distinct from the working
  `TypeName.method<TypeArgs>(args)` post-identifier form). Needs
  additional grammar work to allow a parameterized type name as a
  primary expression. (Form B in the call-syntax taxonomy. Not
  planned; Form C subsumes its use cases.)
- ~~Lambda body with class-typed parameters~~ **Resolved 2026-05-18.**
  `LambdaExpression::resolveTypes` now pushes a temporary scope with
  the lambda's declared parameters before walking the body, so bare-
  identifier references (`acc`, `c`) resolve correctly. Also requires
  skipping the expectedType propagator on method-templated targets
  (templates carry placeholder T-vars and pre-existing instantiations
  may have the wrong T-args for this call site) — see
  `MethodCallExpression`'s lambda-as-arg propagator.
- **Templated method whose T-vars don't appear in value params** (e.g.
  `static <T> int32 sizeOf()`). Multiple instantiations would share
  the same `toCanonical` and collide in `addMethod`'s duplicate-static
  check. Mangling the name to disambiguate breaks the lambda
  expectedType propagator (it looks up by bare name and would only
  find the template's placeholder formals). Tracked by
  `MethodTemplateExplicitArgsTests.DISABLED_staticExplicitWhenInferenceWouldFail`.
