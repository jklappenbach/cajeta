# instanceof-call-lhs (defect)

Found 2026-11-15 by Phoenix (cajeta-ml spela-kernels), reproduced on proton under
the JIT at 30b01764 (v0.53.7) and in a `--release` exe on Phoenix.

## 1. Defect

`expr instanceof Class` is always `false`, silently, when `expr` is a method-call
result. The same value bound to a local first tests `true`.

```cajeta
Holder h = heap Holder();
h.xs.add(#heap Lin());                 // ArrayList<Base>, Lin extends Base
Base f = h.at(0L);                     // at() returns this.xs.get(i)
boolean viaLocal = f instanceof Lin;           // true
boolean viaCall = h.at(0L) instanceof Lin;     // false (wrong)
boolean viaGet = h.xs.get(0L) instanceof Lin;  // false (wrong)
```

It holds standalone, inside an `&&` chain and inside a pattern guard
(`x.get(i) instanceof ReLU r`).

## 2. Impact

- cajeta-ml's `Sequential.forward` Linear+ReLU fusion
  (`this.layers.get(i + 1) instanceof ReLU r`) never fired; fixed in cajeta-ml
  cf22aa7 by binding to a local.
- A grep of cajeta-llm and cajeta-xgboost main finds no other `) instanceof`.
- The answer is wrong without a diagnostic, so any other such test in user code
  takes its false branch unnoticed.

## 3. Use cases

- **3.1** When the lhs of `instanceof` is a method-call result whose runtime class
  is the target or a subclass of it, the test is true; otherwise it is false.
- **3.2** When that test is a pattern (`call() instanceof T t`), `t` binds the call's
  result on a match and the call runs once.
- **3.3** When the lhs is a call that returns an interface, a template parameter
  type or `Object`, the test asks the runtime as a local of that type does.
- **3.4** When the lhs is a call whose static type already is the target, the test
  stays the constant it folds to for a local.

## 4. Cause and fix

`InstanceOfExpression::generateCode` read the lhs's static type before generating
the lhs. A method call's return type is resolved with its callee during code
generation (`MethodCallExpression::generateCode` sets `resolvedType`), so for a
call it was still null. With no static type, neither runtime check ran, and the
fold compared a null type with the target: always false. The lhs is now generated
first, once, and its type read after; the runtime check and the fold see the
call's real return type.

## 5. Acceptance

- **5.1** `InstanceofCallLhsTests` (3.1 to 3.4) pass under the JIT, and the
  existing `InstanceofInterfaceTests`, `InstanceOfTests` and the reified-capture
  `instanceof` tests still pass.
- **5.2** The full cajeta suite passes.
