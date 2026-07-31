# Field-store title trap — surrendered rvalue freed at callee exit — spec (draft)

Origin: compiler-mcp Unit 5 skill-example verification, 2026-07-31.
**Re-diagnosed 2026-07-31** while fixing; the original filing
(`template-field-borrow-escape-spec.md`) was wrong on both counts and is
superseded by this document. See §5 for the correction.

## 1. Definition

A **freshly-constructed owned rvalue** passed to a **plain (non-`#`) class-typed
formal**, which the callee stores into a field with a **plain `=` store**, is
**freed when the callee returns** — leaving the field dangling. The program
compiles with no diagnostic and reads freed memory.

```cajeta
public class BoxA {
    public Animal value;
    public BoxA(Animal v) { this.value = v; }   // plain formal, plain store
}
BoxA b = heap BoxA(heap Animal(2));             // fresh rvalue -> title surrendered
return b.value.tag;                             // reads freed memory
```

This is the transfer ABI operating as specified — "formals are runtime owners;
an unconsumed flag-true formal drops in the callee" (title-tracking spec §4 REV
2 / `Method::emitFormalDropEntries`). The store does not consume the title, so
the formal's armed drop entry fires at callee exit. The defect is that the most
natural constructor spelling silently corrupts.

## 2. Verified behavior (2026-07-31)

| Call shape | Formal | Store | Result |
|---|---|---|---|
| `heap BoxA(heap Animal(2))` — fresh rvalue, title **surrendered** | `Animal v` | `this.value = v` | **freed at ctor exit; field dangles** (garbage / SIGSEGV) |
| `Animal a = heap Animal(2); heap BoxA(a)` — named local, title **lent** | `Animal v` | `this.value = v` | **correct** — field aliases, `a` still owns |
| `heap Box<Animal>(heap Animal(2))` | `#T v` | `this.value #= v` | **correct** — field owns |

Templated and concrete field types behave identically; the template case merely
crashes more visibly (SIGSEGV vs. garbage read).

## 3. Why the obvious fix is wrong

A static "borrow stored into a field" error **must not** be added:
`docs/specification/lang/FieldOwnership.md` explicitly **dropped** that rule —
Solution A (`@Borrow` annotation) and Solution D (force transfer at constructor)
were both **rejected**, and fields may legitimately alias. The stdlib depends on
it (`ArrayStream<T>.data` aliases `ArrayList<T>.data`; `Optional<T>.value`
aliases its constructor argument). Auto-drop discriminates owner from alias at
**runtime** via the live-set claim (Solution B, shipped). The lend row in §2 is
that design working correctly and must not regress.

## 4. Proposed fix (semantic — needs sign-off)

Make a plain field store **consume the formal's runtime title**, mirroring what
`#=` already does:

- Formal's transfer bit **true** (caller surrendered) → the field takes the
  title (ownership bit set) and the formal's drop entry is deactivated, so
  nothing frees at callee exit. Fixes row 1.
- Formal's transfer bit **false** (caller lent) → unchanged: the field aliases.
  Row 2 and every stdlib aliasing site keep their current behavior.

The machinery exists: `BinaryOpExpression`'s field-ownership-bit path already
supports a runtime-conditional owned store (`fobRuntimeFlag`, used today for
`MoveExpression` sources), and the callee-side transfer word is available via
`Method::getTransferWordArg()`. The work is (a) extracting the bit for a plain
formal source and (b) deactivating that formal's drop entry under the same
condition.

**This changes ratified ownership semantics for every plain field store of a
formal, so it is a design decision, not a defect fix.** It also warrants the
full 51-minute sweep, not the light gate — the title-tracking area has a history
of subtle, sweep-only regressions.

Alternative considered and rejected: a compile-time warning at the store site.
It cannot distinguish the corrupting call from the legitimate one (the
discriminator is the *caller's* runtime flag), so it would fire on every
legitimate aliasing store in the stdlib.

## 5. Correction to the original filing

The superseded spec claimed (a) the bug was specific to **template-typed
fields** and (b) the **borrow-escape analysis fails to fire** through them.
Both are false:

- **(a)** A concrete field type (`public Animal value;`) reproduces it
  identically — verified.
- **(b)** There is no borrow-escape analysis to fire; that rule was
  deliberately removed (§3). The mechanism is title consumption, not escape
  analysis.

The `language-templates` skill has been corrected accordingly.

## 6. Tests

`test/expression/SignatureAbiTests.cpp`:
- `DISABLED_freshRvalueIntoPlainFormalFieldStoreDangles` — pins the trap;
  enable when §4 lands.
- `lentLocalIntoPlainFormalFieldStoreAliases` — pins the lend path that must
  not regress.
- `owningTemplateFieldStoreCompilesAndRuns` — pins the documented `#T`+`#=` fix.
