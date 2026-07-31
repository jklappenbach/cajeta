# Template-typed field stores escape borrows silently — spec (draft)

Origin: compiler-mcp Unit 5 skill-example verification, 2026-07-31.

## 1. Definition

Storing a **borrowed** class-typed value into a field through a
template-typed slot compiles with no diagnostic and produces a
use-after-free at runtime. The borrow checker's field-borrow-escape
analysis does not fire when the field's declared type is a type parameter.

The guide's own `Box<T>` example (docs/guide/14-templates.md §Class
templates: `Box(T v) { this.value = v; }`) has this shape: for primitive
`T` the store is a copy and fine; for class `T`, a caller passing a fresh
temporary (`heap Box<Dog>(heap Dog())`) leaves `value` dangling — the
unowned `Dog` drops after construction — and the next `value` use
SIGSEGVs. Both `jit-run` and `--emit=exe` crash identically (the exe's
drop chain shows the same object registered twice, one entry active).

## 2. Repro (24 lines, verified 2026-07-31)

```cajeta
package dev.cajeta.skills;

public class Box<T> {
    public T value;
    public Box(T v) { this.value = v; }   // borrowed param stored into field
}

public class Animal { public int32 tag() { return 1; } }
public class Dog extends Animal { public int32 tag() { return 2; } }

public class Wc {
    public static int32 run() {
        Box<Dog> bd = heap Box<Dog>(heap Dog());
        return bd.value.tag();            // SIGSEGV — value dangles
    }
}
```

`cajeta jit-run src dev.cajeta.skills.Wc.run` → SIGSEGV. With the owning
shape — `Box(#T v) { this.value #= v; }` — the same caller returns 2, and
wildcard-typed parameter calls (`inspect(Box<? extends Animal>)`) work
correctly; the wildcard machinery is NOT at fault.

## 3. Expected behavior

The monomorphized store `this.value = v` (class-typed `T`, `v` a borrowed
parameter) is a borrow escaping into a longer-lived field and must be
rejected exactly as the non-template equivalent is (FieldBorrowEscape /
`CAJETA_ERROR_*` family), steering to `#T` + `this.value #= v`.

## 4. Notes

- Same silent-UB severity class as `stack-return-transfer-error-spec.md`.
- docs/guide/14-templates.md's `Box` and the wildcard snippets built on it
  should switch to the owning ctor when this is fixed (or before).
- The catalog skill `language-templates` documents the owning idiom as the
  default and cites this spec as the hazard.
- Adjacent stale-doc find, same session: docs/guide/15-lambdas.md still
  uses the retired `int32[] xs = {1, 2, 3, 4}` brace form
  (`CAJETA_ERROR_ARRAY_BRACE_INIT_RETIRED`); bracket literals are current.
