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

## 4a. Design — 2026-08-02 (code-read, not yet implemented)

**Take 4.2, at one site.** `Expression::fromContext`,
`src/cajeta/asn/expression/Expression.cpp:218` — the `ctx->LPAREN()` branch
already forks cast-vs-postfix-call on `ctx->typeType().empty()`, and its own
comment states the two forms. The `else` arm building `CallExpression` is
where the reinterpretation goes.

**Rule:** when the callee (`ctx->expression(0)`) is a *parenthesized single
identifier*, build a `CastExpression` on that identifier's type instead of a
`CallExpression`.

Why this is narrow rather than clever:

- The shape is detectable **syntactically** — `expression(0)` is a primary
  holding `'(' expression ')'` whose inner expression is one identifier.
  Checking shape first is what keeps the change narrow.

  *(Corrected after implementing.* This design originally claimed the name
  must NOT be resolved here — that template bodies are skipped on the
  declaring walk, so resolving `E` at dispatch would work at instantiation and
  fail at declaration. That reasoning was wrong. A template body is only ever
  walked FOR an instantiation, and at that point the substitution stack has
  `E` bound, so `resolveNamed` succeeds. Tests 1 and 2 pin both the
  method-level and class-level cases. Resolving is in fact what makes the
  change safe: a name that is not a type falls through to the call unchanged,
  so no working program can break.)*
- **3.3 falls out for free.** `kernel.launch(...)(...)`'s callee is a method
  call, not a parenthesized identifier — the branch never fires.
- **3.2 is untouched.** `(T) a.b()` does not parse as a postfix call at all
  (its operand is unparenthesized), so it never reaches this arm and its
  precedence cannot move. This is the advantage over 4.1, which edits the
  grammar and must re-prove precedence for every suffix operator.
- **3.4 resolves toward Java**, as recommended: `(name)(expr)` is a cast. The
  indirect-call form `(fnValue)(args)` becomes unspellable with a *bare*
  parenthesized identifier callee; it remains available as `fnValue(args)` or
  `(this.fn)(args)` (a dotted callee is not a single identifier). Confirmed
  unused across runtime, tests, and the ML stack as of 2026-07-31.

**One divergence to pin, not paper over** — and it is an INCONSISTENCY, which
writing the test revealed and this spec originally got wrong.

`(T)(x).foo()` with T naming a type parses outermost as DOT over `(T)(x)`, so
this rule yields `((T)(x)).foo()` — cast, then suffix. Java yields
`(T)((x).foo())`.

But with a PRIMITIVE destination, `(int64)(x).foo()` cannot match the
postfix-call alternative at all, so the cast alternative takes the whole chain
and cajeta already agrees with Java. Measured, not assumed: an early draft of
test 5 used `(int32) (d).toString()` and failed with "no member 'toString' on
'float64'" — i.e. the primitive form had bound the whole chain, Java-style.

So `(D)(x).f()` reads differently depending on whether D is a primitive or a
type name. Matching Java for the type-name case would mean restructuring the
tree upward from the DOT parent, far beyond this change. Accept the split,
pin BOTH sides, and say so plainly rather than leaving it to be discovered.

**Tests (written first, all six in `test/expression/UnaryAndCastTests.cpp`):**

1. `typeParamCastOfParenthesizedOperand` — `(E) (acc / 2.0)` in a generic
   METHOD. The reported case.
2. `classTypeParamCastOfParenthesizedOperand` — the same over a CLASS type
   parameter (`Box<E extends Floating>`), which is the `GradTape<E>` shape.
3. `primitiveCastOfParenthesizedOperandStillWorks` — `(int64) (mm & 65535)`.
4. `castBindsWholePostfixChainNotJustTheReceiver` — the precedence pin.
5. `typeNameCastOfParenthesizedThenSuffixBindsCastFirst` — the divergence.
6. `primitiveCastBindsWholeChainLikeJava` — the other side of the split.

1, 2 and 5 failed with the reported `CAJETA_ERROR_NOT_IMPLEMENTED: general
postfix call` before the change; 3, 4 and 6 passed before and after.

## 4b. Implemented — 2026-08-02

`Expression::fromContext`, `src/cajeta/asn/expression/Expression.cpp`: a new
`castDestOfParenCallee(ctx)` helper, consulted in the `LPAREN` branch between
the existing typeType-cast arm and the postfix-call arm.

Two details that only surfaced in the writing:

- **The operand is the call's ARGUMENT, not a child expression.** `(E)(x)`
  parses with `expression(0)` = the callee `(E)` and `x` inside
  `parameterList`. The generic child-attach loop at the bottom of
  `fromContext` would have hung the *destination type* on the cast as its
  operand. The cast arm attaches `parameterList()->parameterEntry(0)`
  explicitly and skips that loop.
- **Shape is checked before resolution, and resolution is last.** The callee
  must be a parenthesized single identifier and there must be exactly one
  unlabelled, non-`#` argument; only then is the name resolved. So
  `kernel.launch(d)(a)` is excluded on shape, and a name that is not a type
  falls through to the call unchanged — the change cannot turn a working call
  into an error.

Validation: 28/28 in `CastTests`+`PrefixTests`+`PostfixTests`, 22/22 across
`XpuLaunch*`/`XpuKernelArg*` (3.3), light sweep 198/198. Full sweep at the
gate.

Then revert the 17 hoisted locals in `ml/grad/StructKernels.cajeta`? **No** —
leave them. They read better, which the spec already notes; the fix is for the
next generic kernel, not for churning working code.

## 5. Not blocking

Priority is low: the workaround is mechanical and reads better. Slot it
with the other open compiler defects at the bottom of the stack, not
inside a feature arc.
