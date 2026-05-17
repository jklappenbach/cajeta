# Cajeta Lambdas & Method References — Specification v1

## Goals

- **First-class function types.** `(T1, T2) -> R` is a primitive type-former — a value of that type is callable. No `Runnable`, no `Callable`, no `@FunctionalInterface` indirection.
- **No boxing.** Primitives flow through function types directly. `(int32) -> int32` is not `Function<Integer, Integer>` in disguise.
- **No erasure.** Function types monomorphize like every other generic. `(T) -> R` instantiates per `(T, R)` pair.
- **Capture rules fall out of the memory model.** No "effectively-final" footgun, no Java `int[1]` workaround, no `[&]` vs `[=]` C++ ambiguity. Default is borrow, `#name` transfers ownership — same vocabulary the rest of the language uses.
- **Method references are sugar.** `obj::method` desugars to `(args) -> obj.method(args)`. No special machinery.
- **Zero-cost for non-capturing forms.** A lambda with no captures lowers to a bare function pointer.

## Non-goals (v1)

- Per-lambda type parameters (`<U> (U) -> U`). Same virtual-vtable reasoning as method-level templates — see `cajeta-docs/stdlib/MemoryModel.md` and the grammar note above `memberDeclaration` in `antlr4/CajetaParser.g4`. Class-level `<T>` referenced inside a lambda body is fine; T is bound by the enclosing class.
- Target-type inference for ambiguous method references. The LHS function type or method-parameter type must disambiguate; if it can't, the user writes an explicit lambda.
- "By-reference" capture of primitives. Primitives capture by value. If you need shared mutable primitive state across closures, wrap it in a heap value.
- Variadic lambdas (`(T...) -> R`). Out of scope for v1; revisit if there's demand.

---

## Function types

`(T1, T2, ..., Tn) -> R` is a type. Values of this type are callable with arguments matching the parameter tuple, returning `R`.

```
(int32, int32) -> int32 add = (a, b) -> a + b;
add(2, 3);   // 5
```

Zero-parameter form: `() -> R`. Returning void: `() -> void` (or just `void`-returning).

Function types are first-class:
- They can be variable types.
- They can be parameter types.
- They can be return types.
- They can be field types (in classes or actors).
- They can be element types of arrays / lists / channels.

```
(int32) -> int32 doubler = x -> x * 2;
(int32) -> int32 tripler = x -> x * 3;
(int32) -> int32[] pipeline = {doubler, tripler};
```

Function types are NOT classes — there's no inheritance, no methods on the function value beyond invocation. If you need OO machinery on top of a callable, wrap the function value in a class.

### Lowering

A function-typed value is a struct of `{ fn_ptr, captures_ptr }`. The captures pointer is null when the lambda has no captures, in which case the runtime call path skips the indirection and dispatches directly through `fn_ptr` — non-capturing lambdas are free.

---

## Lambda syntax

```
(parameterList) -> body
```

Where `body` is either an expression or a block. Parameter types may be omitted when the surrounding context pins the function type:

```
(int32 a, int32 b) -> int32 add1 = (a, b) -> a + b;       // explicit param types
(int32, int32) -> int32 add2 = (a, b) -> a + b;           // inferred from LHS
(int32, int32) -> int32 add3 = (int32 a, int32 b) -> a + b; // also fine
() -> int32 const42 = () -> 42;                            // no params, expression body
() -> void greet = () -> { print("hi"); };                 // block body
```

If a lambda's parameter types aren't pinned by context AND aren't written explicitly, that's a compile error — `(a, b) -> a + b` standing alone has no type.

---

## Method references

`obj::method`, `MyClass::staticMethod`, `MyClass::instanceMethod`, `MyClass::new`. Each desugars to a lambda; the desugaring determines what gets captured.

### Static method reference

```
class MathUtil {
    public static int32 abs(int32 x) { return x < 0 ? -x : x; }
}

(int32) -> int32 absFn = MathUtil::abs;
absFn(-5);   // 5
```

Desugars to `(x) -> MathUtil.abs(x)`. No captures.

### Bound instance method reference

```
class Stringifier {
    public string toJson(Record r) { ... }
}

Stringifier s = new Stringifier();
(Record) -> string fn = s::toJson;
fn(someRecord);
```

Desugars to `(r) -> s.toJson(r)`. `s` is captured — see the capture rules below; default is borrow.

### Unbound instance method reference

```
class Person {
    public string name() { ... }
}

(Person) -> string getName = Person::name;
string n = getName(somePerson);
```

Desugars to `(self) -> self.name()`. No captures — `self` is the first parameter.

### Constructor reference

```
class Point {
    public Point(int32 x, int32 y) { ... }
}

(int32, int32) -> Point makePoint = Point::new;
Point p = makePoint(3, 4);
```

Desugars to `(x, y) -> new Point(x, y)`. No captures.

### Overload resolution

When a method has overloads, the LHS function type (or the parameter slot accepting the value) must disambiguate:

```
class Convert {
    public string from(int32 x) { ... }
    public string from(fp64 x) { ... }
}

(int32) -> string a = c::from;   // picks the int32 overload
(fp64)  -> string b = c::from;   // picks the fp64 overload
```

If no context pins the type:

```
auto fn = c::from;   // compile error: ambiguous method reference
fn = c::from;         // ditto if fn's type isn't pinned
```

The user writes an explicit lambda to resolve:

```
(int32) -> string fn = x -> c.from(x);   // unambiguous — the int32 param picks the int32 overload
```

---

## Capture

A capture is a name from outer scope referenced in the lambda body. Capture rules follow the existing memory model: primitives copy, heap values borrow by default, `#name` transfers.

### Rule 1 — Primitives capture by value

`int32`, `fp64`, `bool`, `char`, etc. are value types. They're copied into the closure at capture time. Mutating the original local later does not affect what the closure sees.

```
int32 multiplier = 10;
(int32) -> int32 fn = x -> x * multiplier;   // multiplier copied into the closure

multiplier = 20;
fn(5);   // returns 50, NOT 100 — the closure has the captured 10
```

If you need shared mutable primitive state across closures, wrap it in a class:

```
class Cell {
    public int32 value;
    public Cell(int32 v) { value = v; }
}

Cell m = new Cell(10);
(int32) -> int32 fn = x -> x * m.value;       // borrows m
m.value = 20;
fn(5);   // returns 100 — closure sees updated state through the borrow
```

### Rule 2 — Heap values capture by borrow (default)

Class instances, arrays, strings, and any other heap-allocated owner — the closure holds a borrow. The original local still owns the value; the closure must not outlive the borrow's scope.

```
StringBuilder sb = new StringBuilder();
() -> string fn = () -> sb.toString();   // closure borrows sb

sb.append("hello");
fn();                                     // "hello" — closure sees live state via the borrow
```

The borrow checker enforces lifetime: a function returning a closure that borrows a local is a compile-time error.

```
() -> int32 makeCounter() {
    Counter c = new Counter();
    return () -> c.next();    // ERROR: borrow of `c` would dangle past the function return
}
```

Three legitimate cases pass automatically:

- **Sync inline use** — the closure is consumed within the call and goes out of scope before the borrow does.
  ```
  list.forEach(x -> processWith(x, sb));    // forEach runs the closure inline; sb outlives the call
  ```
- **Scoped spawn** — `scope { spawn ... }` guarantees children complete before the scope returns, so a borrow into a spawned task lives as long as the scope.
  ```
  scope {
      spawn () -> async void { await processWith(sb); };
  }
  ```
- **Stored briefly, used briefly** — the closure is held in a variable that goes out of scope before its captures do.

### Rule 3 — `#name` transfers ownership into the closure

To make a closure outlive its captures' original scope, transfer ownership. Same `#` operator as everywhere else in the language.

```
() -> int32 makeCounter() {
    Counter c = new Counter();
    return () -> #c.next();   // c is transferred into the closure
    // c is unavailable here — moved
}

() -> int32 fn = makeCounter();
fn();   // 1
fn();   // 2 — the closure owns c and mutates it across calls
```

When the closure itself drops, its captured `c` drops with it — same drop-chain machinery as ordinary owners.

For `detach`, transfer is mandatory (no scope to anchor borrows):

```
async void main() {
    LogSink sink = new LogSink();
    detach () -> async void {
        await #sink.flush();   // sink moves into the detached task
    };
    // sink unavailable here — moved
}
```

A `detach` body containing a borrow capture is a compile error.

### Rule 4 — `this` captures as a borrow; `#this` to transfer

When a lambda appears inside an instance method, `this` is implicitly in scope. The default capture is a borrow of the receiver; if the closure needs to outlive `this`, transfer it.

```
class Counter {
    int32 value;

    public () -> int32 readLater() {
        return () -> this.value;     // ERROR — borrow of this would dangle past method return
    }

    public () -> int32 takeReadLater() {
        return () -> #this.value;    // transfer self into the closure
        // `takeReadLater` consumes the receiver — caller must have ownership of the Counter
    }
}
```

Reading sugar: `() -> value` inside an instance method is implicit-this, captured as a borrow. To transfer self, the `#this` form is explicit; there's no `() -> #value` shorthand (the marker has to be on `this`, not on the field).

### Rule 5 — Mutating captures of heap values works; mutating captured primitives is a compile error

Heap-value captures are borrows (Rule 2). Mutating through a borrow is governed by the existing aliasing rules: an exclusive-mutable borrow precludes other access for its lifetime. So a lambda that mutates a heap-value capture is automatically safe — the compiler refuses any conflicting read of the same value while the closure exists.

```
StringBuilder buf = new StringBuilder();
() -> void appendDot = () -> { buf.append("."); };   // exclusive-mutable borrow of buf

appendDot();
appendDot();

// ERROR: cannot read `buf` while `appendDot` holds an exclusive borrow
// string snapshot = buf.toString();
```

Drop `appendDot` (let it go out of scope) before reading `buf` again, and the conflict resolves.

For **primitives**, this would be confusing. Rule 1 says primitives capture by value (the closure has its own copy), so a write to the captured name would modify the closure's private copy and never propagate back. Rather than allow code that looks like it mutates the outer variable but doesn't, Cajeta makes this a compile error:

```
int32 counter = 0;
() -> int32 inc = () -> { counter++; return counter; };
//                          ^^^^^^^
// ERROR: cannot write to value-captured primitive `counter`
//        primitives are captured by copy; writes would not affect the outer variable
//        hint: wrap in a heap value (e.g. a Cell class with an int32 field) and
//              mutate through the borrow
```

The wrapper-class workaround:

```
class Cell {
    public int32 value;
    public Cell(int32 v) { value = v; }
}

Cell c = new Cell(0);
() -> int32 inc = () -> { c.value++; return c.value; };  // borrows c
inc();   // 1
inc();   // 2
```

This eliminates Java's "effectively-final" rule via a different route: instead of forbidding all writes silently, Cajeta rejects writes to value-captured primitives with a clear error and a clear remedy. Writes to heap captures work without ceremony because the borrow checker keeps them honest.

---

## Passing methods as arguments

Function-typed parameters accept lambdas, method references, and function-typed variables interchangeably.

```
class List<T> {
    public void forEach((T) -> void action) {
        for (T t : items) action(t);
    }

    public List<U> map<U>((T) -> U f) {
        List<U> out = new List<U>();
        for (T t : items) out.add(f(t));
        return out;
    }

    public T? find((T) -> bool pred) {
        for (T t : items) if (pred(t)) return t;
        return null;
    }
}
```

(Wait — `map<U>` would be a method-level template, which doesn't exist in Cajeta. In practice `map` lives on `List<T>` and takes a target list as input, or `List` is enriched with `mapTo<U extends ...>` via the class template's parameters. Treat the example as illustrative of the *call site*, not the declaration; the real `List` API will pick a workable shape.)

Use sites:

```
List<int32> nums = ...;

// Inline lambda
nums.forEach(n -> print(n));

// Method reference (static)
nums.forEach(System::println);

// Method reference (bound to receiver)
class Logger {
    public void log(int32 n) { ... }
}
Logger l = new Logger();
nums.forEach(l::log);

// Function-typed variable
(int32) -> void handler = x -> handle(x);
nums.forEach(handler);

// Find with predicate
int32? first = nums.find(n -> n > 100);

// Method reference as predicate
int32? firstNeg = nums.find(MathUtil::isNegative);
```

### Strategy pattern via function types

Where Java would define a `Comparator<T>` interface, Cajeta uses `(T, T) -> int32`:

```
class List<T> {
    public void sort((T, T) -> int32 compare) { ... }
}

list.sort((a, b) -> a.score - b.score);
list.sort(MyClass::byScore);
```

Where Java would define a `Predicate<T>` interface, Cajeta uses `(T) -> bool`. `Supplier<T>` → `() -> T`. `Consumer<T>` → `(T) -> void`. `Function<T, R>` → `(T) -> R`. `BiFunction<A, B, R>` → `(A, B) -> R`. The functional-interface zoo collapses into the type system.

### Currying and partial application

There's no built-in `curry`, but the rules above compose freely:

```
(int32) -> (int32) -> int32 adder = a -> (b -> a + b);
(int32) -> int32 add5 = adder(5);
add5(7);   // 12
```

The outer `adder` takes an int32 and returns a closure. The returned closure captures `a` — by value, since it's primitive. No special "curry" keyword needed.

---

## Threading interactions

Lambdas compose with the threading model (see `cajeta-docs/stdlib/Thread.md`) according to the same capture rules:

### `spawn` inside `scope` — borrows allowed

The scope blocks until children complete, so a borrow into a spawned task is bounded by the scope's lifetime.

```
StringBuilder log = new StringBuilder();
scope {
    spawn () -> async void { await runWithLog(log); };   // borrows log
    spawn () -> async void { await runWithLog(log); };
}                                                          // scope joins; log is still in scope
```

### `detach` — transfers required

`detach` runs the task past the surrounding scope, so borrows would dangle. Transfer is mandatory:

```
Counter c = new Counter();
detach () -> async void {
    await #c.run();    // c transferred into the detached task
};
// c unavailable here
```

### Capturing actor references

Actor references are heap values; they capture by borrow by default. If the closure outlives the actor's scope, transfer:

```
async void wireUp() {
    Logger log = new Logger();                  // an actor
    Worker w = new Worker(#log);                // transfers log into the worker
    // log unavailable here
    await w.run();
}
```

(The worker's constructor takes a `#Logger`, declaring it consumes ownership.)

### Mutex / RwLock / Semaphore in a closure

A LockGuard holds a borrow of the locked value; capturing it into a lambda follows the same rules.

```
Mutex<Stats> stats = new Mutex<>(new Stats());

async void recordAll(List<Event> events) {
    scope {
        for (Event e : events) {
            spawn () -> async void {
                LockGuard<Stats> g = await stats.lock();
                g.record(e);   // LockGuard is local to this spawn's task
            };
        }
    }
}
```

A LockGuard escaping its acquiring scope (returned, stored in a long-lived structure) is a compile error — same lifetime rule as any other borrow.

---

## Compile-time errors — what you'd see

The lifetime / aliasing rules from the memory model produce error messages tied to lambda capture. Examples of what's rejected and what the error looks like:

```
() -> int32 makeFn() {
    int32 x = 0;
    Counter c = new Counter();
    return () -> c.next() + x;       // ERROR
}
//   ^ closure outlives its capture
//     `c` is borrowed by the returned closure but `c`'s owner drops at function return
//     hint: use `#c.next()` to transfer ownership of `c` into the closure
```

```
StringBuilder buf = new StringBuilder();
() -> void writer = () -> { buf.append("."); };
string snap = buf.toString();    // ERROR
//            ^ conflicting access
//              `buf` is exclusively borrowed by `writer`; reading it here would alias
//              hint: drop `writer` (let it go out of scope) before reading `buf`
```

```
int32 n = 0;
() -> int32 inc = () -> { n++; return n; };
//                          ^^^
// ERROR: cannot write to value-captured primitive `n`
//        hint: wrap in a heap value (e.g. a Cell class) and mutate through the borrow
```

```
detach () -> async void {
    await sink.flush();           // ERROR
};
//        ^ detach captures borrow
//          `sink` is borrowed but `detach` requires transferred captures
//          hint: write `#sink.flush()` to transfer ownership into the detached task
```

---

## Open design notes / deferred

- **Anonymous function types in declarations.** Today the syntax `(int32) -> int32 fn = ...` reuses the function-type grammar in variable-declaration position. Possible future addition: `fn` keyword to introduce a function-typed local with type inference (`fn add = (a, b) -> a + b;`). Not in v1.
- **Higher-rank function types.** Function types whose parameter or return is itself a function type (`((int32) -> int32) -> bool`) work today by composition; no special syntax. Naming them with a typealias is a separate language addition.
- **Comparison / equality.** Two lambdas are NOT comparable for equality — function values have identity-only semantics (two captures of the same source produce indistinguishable values). No `equals` method.
- **Serialization.** A closure with captures can't be serialized — its captures aren't necessarily representable on the wire. This is a hard rule, not a deferred feature.
- **Re-entrant lambdas / Y combinator.** A lambda referencing itself by name needs the name to be in scope first. Top-level function-typed bindings can self-reference; locally-bound lambdas need a forward declaration or the `Y`-combinator pattern. Possible future addition: `fn name = ... name(...) ...;` allowed by hoisting the name's binding before the initializer. Not in v1.
