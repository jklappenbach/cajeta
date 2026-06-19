# STACK — template / Tensor capture work (branch `feature/tensor-capture`)

LIFO worklist for the remaining reified-template features. **Top = next.** Pop
from the top; when an item is done move it to `COMPLETED-STACK.md` (newest on
top). If a detour/unblocker appears mid-item, push it onto the TOP, finish it,
then resume the item beneath. Precursors/most-basic at the top, most-dependent
at the bottom.

Design sources: `agents/documents/cajeta-templates/reified-capture-spec.md`,
`agents/documents/cajeta-templates/numeric-bounds-spec.md` /
`numeric-bounds-plan.md`, and the Tensor docs under
`agents/documents/cajeta-math/`.

## Capture — extended targets (toward `Tensor<T>` / `Tensor<? extends ...>`)

- class-bounded-wildcard capture (capture plan 5b) — `(Foo<? extends Animal>) w`
  succeeds for `Foo<Dog>`, fails for `Foo<Cat>`. Depends on the name→RTTI
  registry above. NOTE: `isAssignableToWildcard`'s class-bound check uses
  `isParentOrKind`, which skips `implements` — see
  [[isparentorkind-skips-interfaces]]; use a conforms-to-incl-interfaces helper
  here.

## Numeric — the deep variance item

- bare `Tensor<? extends Floating>` variable/field over a mutable primitive
  container (numeric plan 5a) — wildcard monomorphization of non-uniform
  primitive elements + a capture-aware PECS exemption (allow the enclosing
  class's own-`T` internal writes). Deepest item; the parameter form
  `<T extends Floating>(Tensor<T>)` already covers dtype-generic routines, so
  this is additive.
