# null-owned-interface-arg — runtime SIGSEGV passing `null` to a `#`-interface formal

## 1. Definition (defect)

Passing the literal `null` as an OWNED interface-typed argument
(`#Transformer a` formal, call site passes `null`) compiles but SIGSEGVs at
runtime (fault addr 0x10 — a vtable-half dereference on a null fat
pointer), at or before the receiving constructor stores it with `#=`.
Found 2026-07-31 building cajeta-ml's `Pipeline`: `heap Pipeline(null,
null, null, 0, #fin)` crashed inside `Pipeline.of` on the v0.12.1-dev
toolchain; padding the slots with real `IdentityTransformer` instances
fixed it (the shipped workaround, commented at the site).

## 2. Requirements

- 2.1 Minimal repro: interface `I`, class with `#I` ctor formal, call with
  `null`, store with `#=`, drop.
- 2.2 Decide + implement the contract: either a null owned-interface value
  is legal (fat pointer `{null, null}`, transfer and drop are no-ops —
  preferred, matches nullable owned class fields like `DynFrame.backing`)
  or passing `null` to a `#`-interface formal is a compile-time error.
  Silent runtime corruption is the only wrong answer.
- 2.3 Regression pins either way.
- 2.4 Ships with cajeta v0.13.0 (cajeta-ml-v2 plan U7).

## 3. Workaround (in place)

Never store null in an owned interface slot — use a real no-op conformer
(`Pipeline`'s unused stage slots hold `IdentityTransformer`).
