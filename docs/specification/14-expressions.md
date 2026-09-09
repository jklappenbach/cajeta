# 14 — Expressions

This chapter defines expression forms and their evaluation: operators and their order, method invocation and its ownership modes, instantiation expressions, lambdas, and method references.

## 14.1 Evaluation Order

Operands evaluate left to right: in `a() + b()`, `a()` completes before `b()` begins. The conditional operators short-circuit — `&&` skips its right operand when the left is `false`, `||` when the left is `true` — and `cond ? x : y` evaluates only the selected branch.

Operator precedence follows the C family, as fixed by the expression grammar: unary binds tightest, then multiplicative, additive, shift, relational (`instanceof` among them), equality, bitwise `&` `^` `|`, logical `&&` `||`, the conditional, and assignment last.

## 14.2 Operators

The operator set over primitives: arithmetic `+ - * / %`, bitwise `& | ^ ~`, shifts `<< >> >>>`, comparisons `== != < > <= >=`, logical `! && ||`, increment and decrement `++ --`, and the compound assignments `+= -= *= /= %= &= |= ^= <<= >>= >>>=`. `n += 3` and `n++` mutate their target in place. Class types may declare their own binary and index operators (Classes §8.6). String concatenation and its conversions are Conversions §6.4.

## 14.3 Method Invocation

An invocation names a receiver (or a class, for statics), a method, and arguments. Overload selection is by name and parameter types; transfer mode is not part of the signature (Ownership §5.5.1).

Each class-typed argument travels with the caller's ownership decision: `f(x)` lends, `f(#x)` transfers, and the hidden per-call flag tells the callee which it got (Ownership §5.5). A call result binds per the callee's return spelling: a `#T` result must be received with `#=` (Ownership §5.5.2).

## 14.4 Instantiation Expressions

`stack T(args)` and `heap T(args)` construct an instance (Allocation §4.1) and are expressions: they may initialize a binding, pass as an argument, or stand in any value position. Construction selects a constructor by argument types; the absence of a match is a compile-time error, `CAJETA_ERROR_NO_MATCHING_CONSTRUCTOR` — an instance is never left with its constructor unrun.

An anonymous `heap T(args)` in transfer position promotes its title implicitly (Allocation §4.1).

## 14.5 Lambdas

A lambda `(params) -> expr` or `(params) -> { statements }` is a function-typed value; `(int32) -> int32` is the type of a function from `int32` to `int32`.

Capture follows the memory model, with no separate capture syntax:

- **Primitives capture by value**, copied at capture time; later mutation of the source is invisible to the closure.
- **Class-typed values capture as borrows** by default; `#name` in the body transfers the capture.
- **A lambda with no captures is a bare function**: the function value's captures pointer is null and calls dispatch directly, so non-capturing lambdas add no cost.

**Example 14.5-1.** Value capture, a capturing lambda, and a pipeline.

```cajeta
import cajeta.collection.ArrayList;
public final class C {
    public static int32 dbl(int32 v) { return v * 2; }
}
int32 base = 10;
ArrayList<int32> xs = heap ArrayList<int32>();
xs.add(1); xs.add(2); xs.add(3);
int32 sum = xs.stream()
    .map<int32>((v) -> v + base)     // captures base by value
    .map<int32>(C::dbl)              // method reference — no captures
    .reduce(0, (acc, v) -> acc + v);
System.stdout.println(sum);          // 72
```

> *Discussion.* Compile-time rejection of a closure-captured borrow that outlives its source is interim as of 0.27.0 — a lint plus debug-runtime checks rather than a strict type error; strict enforcement is planned. Per-lambda type parameters are not supported; a class-level `T` referenced in a lambda body is fine.

## 14.6 Method References

`::` forms a function value from an existing method; each form desugars to a lambda, and the desugaring determines the captures:

- `MyClass::staticMethod` — no captures.
- `obj::method` — captures `obj` (a borrow, by default).
- `MyClass::instanceMethod` — unbound: the receiver becomes the first parameter; no captures.
- `MyClass::heap` — a constructor reference producing a fresh instance.

## 14.7 Ownership Spellings in Expressions

`#v` surrenders the title its source holds, written at a store, a call argument, or a return (Ownership §5.3). `dst #= v` is the passthrough store, which hands along whatever title the source holds (Ownership §5.4). The positional flag accessor `Cajeta.moveMask()` is retired (`CAJETA_ERROR_MOVEMASK_RETIRED`) in favor of `Cajeta.owned(formal)`.
