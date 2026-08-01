# typeparam-cast-of-paren — `(E) (expr)` parses as a call, not a cast (draft)

## 1. Definition (defect)

A cast whose destination is a TYPE PARAMETER, applied to a parenthesized
expression, does not parse as a cast. It is captured by the postfix-call
alternative and dies in codegen:

```
CAJETA_ERROR_NOT_IMPLEMENTED: general postfix call —
only kernel.launch(...)(...) is supported
```

The same shape with a PRIMITIVE destination is fine — `(int64) (mm & 65535)`
compiles across the stdlib (`ImmutableMap.cajeta:102`, `Utf8.cajeta:48`).
And a type-parameter cast of a NON-parenthesized operand is fine —
`(E) acc`, `(E) x.flatGet(i)`. Only the combination fails.

```cajeta
static #Tensor<E> k<E extends Floating>(Tensor<E> a) {
    float64 acc = 1.5;
    a.flatSet(0, (E) acc);            // OK
    a.flatSet(0, (E) (acc / 2.0));    // CAJETA_ERROR_NOT_IMPLEMENTED
    return #a;
}
```

Observed 2026-07-31 building cajeta-ml v3 U3: 17 sites in one generic
kernel file (`ml/grad/StructKernels.cajeta`), every one of the form
"compute a reduction in float64, narrow to E on store" — the dominant
idiom in generic numeric code. Worked around by hoisting each to a named
`float64` local, which is arguably more readable, so U3 was not blocked.
It WILL recur in every generic kernel the ML stack adds (ml.nn, SPELA,
LoRA all narrow float64 → E).

## 2. Cause

`CajetaParser.g4`'s left-recursive `expression` rule lists

- the postfix call `expression '(' parameterList? ')'` at line ~802, and
- the cast `'(' annotation* typeType ('&' typeType)* ')' expression` at
  line ~816.

Earlier alternatives win in ANTLR4, so the postfix call takes `(E)(x)`:
`(E)` is a valid parenthesized IDENTIFIER expression, so the call
alternative matches. A primitive destination cannot match it — `(int64)`
is not an expression — which is exactly why primitives work today.

`Expression::fromContext` then sees `ctx->typeType().empty()` and builds a
`CallExpression`; `CallExpression::generateCode` throws (the callee is not
a `CajetaFunctionType`).

The grammar's own comment at line ~800 states the intended resolution —
"The classic cast-vs-call ambiguity on `(T)(x)` resolves to the earlier
cast alternative below" — but the cast alternative is *later*, not
earlier, so the stated intent is not what the grammar does.

## 3. Requirements

- 3.1 `(E) (expr)` where `E` names a type (including a method- or
  class-level type parameter in scope) must lower as a cast of `expr`,
  matching `(int64) (expr)` and `(E) expr`.
- 3.2 Cast PRECEDENCE must not change. A naive fix — moving the cast
  alternative above the postfix-call alternative — also lifts it above
  every suffix operator listed between them (`.`, `[]`, method call), so
  `(T) a.b()` would regress from `(T) (a.b())` to `((T) a).b()`. Any
  accepted fix must pin this.
- 3.3 `kernel.launch(...)(...)` must keep parsing as the XPU launch form
  (its callee is a method call, which cannot parse as a `typeType`, so a
  narrow fix should not reach it).
- 3.4 The indirect-call form `(fnValue)(args)` — a parenthesized callee of
  function type — is not used anywhere in the runtime, tests, or the ML
  stack as of 2026-07-31. Whichever way it resolves, state it in the spec
  and pin it. Java resolves the identical ambiguity syntactically in favor
  of the cast, then errors if the name is not a type; matching Java is the
  recommended choice.

## 4. Candidate approaches

- 4.1 **Narrow grammar alternative** — add `'(' typeType ')' '(' expression ')'`
  ahead of the postfix call. Fires only when BOTH sides are parenthesized,
  so it cannot capture `kernel.launch(...)(...)`. Still shifts precedence
  for the (vanishingly rare, reader-hostile) `(T)(x).foo()`.
- 4.2 **Semantic reinterpretation** — keep the parse; in
  `Expression::fromContext`'s `CallExpression` branch (or in
  `CallExpression`'s resolve), detect a callee that is a parenthesized
  single identifier resolving to a type in scope, and build a
  `CastExpression` instead. Zero precedence impact, but needs type-name
  lookup (including the substitution stack) at that point.

4.2 is the safer default; 4.1 is smaller if the precedence pin in 3.2
passes. Either way validation is a full `cajeta_tests.sh` sweep, since a
grammar or expression-dispatch change touches every parse.

## 5. Not blocking

Priority is low: the workaround is mechanical and reads better. Slot it
with the other open compiler defects at the bottom of the stack, not
inside a feature arc.
