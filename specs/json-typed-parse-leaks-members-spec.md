# `dst #= call()` disarms the assignee's drop entry for plain-return callees

*(Filed as "`Json.parse<T>` leaks every owned member it deserializes" — that
is the symptom. The cause is general; see Root cause below.)*

**Filed 2026-08-21** (found converting cajeta-llama's `ModelConfig` to
annotation-driven config binding, which is the idiomatic way to keep
snake_case wire names out of camelCase source).

## Repro (20 lines, exact count)

```cajeta
package test;
import cajeta.codec.json.Json;
import cajeta.lang.Cajeta;

@JsonNamingStrategy("SNAKE_CASE")
public class Cfg {
    public String modelType;
    public int32 hiddenSize;
}

public final class D {
    static void once() {
        String src = "{\"model_type\":\"llama\",\"hidden_size\":7}";
        Cfg c #= Json.parse<Cfg>(src);
        return;
    }
    public static int64 run() {
        D.once();                                   // warm
        int64 a = Cajeta.liveCount();
        int32 i = 0;
        while (i < 4) { D.once(); i = i + 1; }
        return Cajeta.liveCount() - a;              // observed: 8
    }
}
```

**8 live objects after 4 parses — 2 per parse**, for a DTO with ONE
`String` member (the String wrapper and its byte buffer). The result is
bound with `#=`, so the DTO itself is owned and dropped; what survives is
what the synthesized deserializer stored into its fields.

## Why it matters

The synthesized field stores do not register the deserialized members on
the drop chain, so dropping the DTO reclaims the shell and abandons every
`String`, array, and nested object inside it. The leak scales with the
DTO: cajeta-llama's real `config.json` shape (a dozen strings/arrays plus
a nested `rope_scaling` object) leaked **8 per parse**, which turned three
independent ownership probes red the moment `ModelConfig.parse` was moved
onto `Json.parse<T>`:

```
BenchTest::probeCfgParseCycleIsBalanced   expected <0> but was <8>
BenchTest::probeBindCycleIsBalanced       expected <0> but was <8>
BenchTest::loadFreeCyclesReturnResidentToBaseline  expected <372> but was <384>
```

Those probes exist precisely to catch this, and they did — but only
because that project runs them. A consumer without such probes gets a
steady leak per document parsed, which for a server is per request.

## Scope

Typed binding only. The tree API (`Json.parse` → `JsonValue`) is not
implicated. `@JsonNamingStrategy` is incidental — it selects key names,
not ownership — and the leak reproduces without it.

## Investigation 2026-08-21 — narrowed, NOT fixed

Isolation by field kind (4 parses each, `Cajeta.liveCount()` delta):

| DTO | leaked | per parse |
|---|---|---|
| `int32` fields only | 4 | **1** |
| one `String` + `int32` | 8 | **2** |

So there are TWO components: a **field-independent 1 per parse**, plus
**1 more per String field**. Note `liveCount` cannot see arrays, so these
are CLASS instances and no array-binding change can move the number —
worth knowing before re-measuring.

**Corrected along the way, all verified in the generated source via
`CAJETA_DUMP_IR=1`, and none of which moved the count:**

- `JsonSynthesizer` stored deserialized `String`, array, nested-object,
  `Optional` and `@JsonRaw` members with plain `=` — a BORROW of a fresh
  heap allocation. Now `#=`.
- The synthesized preamble bound `JsonCursor jc = heap JsonCursor(...)`
  and `T out = heap T()` with `=`. Now `#=`.
- `JsonCursor`'s own constructor stored `this.idx = heap int32[...]` as a
  borrow, directly contradicting the comment above it ("idx is reclaimed
  by JsonCursor's own drop chain instead"). Now `#=`.

Those were all genuine mis-bindings under the project's own ownership
rules — a `heap` result bound with `=` is what
`CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER` exists to catch — and
`JsonSynthesizerTests` stays 64/64 with them. But the leak is unchanged,
so the mechanism is elsewhere.

### What leaks: the DTO OBJECT itself

A DTO with TWO `String` fields leaks **3 per parse**, not 2. That
discriminates cleanly: the result object is never dropped, and it takes
its owned members down with it. So the earlier "1 field-independent + 1
per String" reading is really "1 object + 1 per owned member it holds",
and every member-binding fix above was necessary-but-not-sufficient —
the members are correctly owned BY an object nobody frees.

### Five hypotheses tested and REFUTED (do not re-run these)

Each was checked with a hand-written equivalent measured the same way;
all leaked **0**:

1. *Synthesized-method locals don't drop.* A hand-written method with an
   owned local plus an owned return drops the local. 0.
2. *Template instantiations don't drop locals.* A generic
   `#Bar makeGeneric<T>()` with an owned local drops it. 0.
3. *`JsonCursor` leaks.* Allocated and dropped in isolation. 0.
4. *A fully-qualified `#test.T` return doesn't transfer* (the synthesizer
   emits FQN returns). Short-name and qualified returns behave
   identically. 0.
5. *The declaration's plain `T` return is the problem.* Changing
   `Json.parse<T>` to declare `#T` changed nothing — still 3 per parse.
   Reverted, since it alters a public signature for no benefit.

### Root cause (MEASURED 2026-08-21) — and it is not a JSON bug

Hypothesis 6, *"the caller's `#=` is not arming a drop for the result"*, is
also REFUTED. It arms one. A compiler trace of the drop decision
(`[localdrop]`) reports `dropEntry=yes` for the assignee, and a runtime
trace of the live set plus every drop-chain transition shows what
actually happens to it:

```
[live+]     0x...f75820 sz=264                      the result object
[droppush]  e=0x...e90 obj=0x...f75820  D.cajeta:25 pushed ARMED
[setflag]   e=0x...e90 obj=0x...f75820  flag=0      immediately DISARMED
[droppop]   e=0x...e90 obj=0x...f75820  active=0    never runs -> leak
```

The disarm is `src/cajeta/asn/expression/Expression.cpp` (MoveExpression,
sharp-store tail): for `dst #= call()` the runtime title flag was a
COMPILE-TIME CONSTANT, `bindingTakesTitle() ? 1 : 0`. `bindingTakesTitle()`
answers from the declared return stance, and `Json.parse<T>(String)`
declares a plain `T` — so the constant is 0, and every assignee's drop
entry is armed and then switched off.

This is precisely the error CLAUDE.md §2.1 documents: **a plain return is
not statically a borrow.** `Json.parse<T>(String)` is the canonical
counter-example — it declares `T` and tail-calls the synthesized
`Json.parse<T>(bytes, len)`, riding that method's title out through a
plain signature.

So the defect is general, not JSON-specific: `dst #= f()` leaked for ANY
callee that declares a plain class-pointer return and returns a title at
runtime. JSON was where it was noticed because `Json.parse<T>` is the
stdlib's most-used method of that exact shape.

The measured law before the fix — **leak = 1 + one per owned member,
per call** — falls straight out: the result object is abandoned, and
everything reachable only through it is abandoned with it.

### Why the earlier hand-written equivalents all measured 0

Every control in 1-5 bound its local from `heap X(...)` (a NewExpression)
or from a `#`-declared return. Neither reaches the constant-folding
branch: the first is not a call at all, and for the second the static
stance and the runtime flag AGREE. The one shape that discriminates —
`#=` from a call whose declared return is plain — was the one shape no
control covered. Controls must vary the mechanism under test, not just
the payload.

## Fix (SHIPPED)

`MoveExpression::generateCode` captures `__cajeta_return_flag_get()`
immediately after the inner call — while the TLS still holds that call's
bit — and feeds THAT to the assignee's drop entry. The compile-time
`bindingTakesTitle()` constant survives only as the fallback for callees
that never store a flag (raw-IR synthesized bodies, intrinsics), gated on
`Method::emitsReturnFlag() && returnsClassPointer()`.

The capture is at the call, not at the store: anything later reads a
stale TLS. That placement is the whole correctness argument.

### Sibling site inspected, deliberately unchanged

`BinaryOpExpression.cpp` (array element title-store, `arr[i] = rhs`) uses
the same `bindingTakesTitle()` constant, but only on its BARE-call branch
— a plain `=` store. Its `#=` branch already delegates to
`MoveExpression::getRuntimeTitleFlag()`, so `arr[i] #= call()` and
`this.f #= call()` are both fixed by this change with no edit there.

The bare branch stays static on purpose: a plain `=` records a borrow by
design, and a plain `=` from a `#`-returning call is already a compile
error (`CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER`). Making it consult the
runtime flag would let a plain store claim a title, which is a double-free
in exactly the cases the plain spelling exists to avoid.

The earlier owned-member binding corrections in
`src/cajeta/codec/JsonSynthesizer.cpp` (6 sites, `=` -> `#=`) stay — they
were necessary but not sufficient, and with the root cause fixed they are
what makes the members drop once their owner does.

## Acceptance

- The repro above returns **0**.
- A DTO with a nested object, a `String[]`, and a `String` returns 0.
- An owned member that the document does NOT supply (field left at its
  default) is still safe to drop — no double-free on the absent case.
- cajeta-llama's `ModelConfig` can bind through `Json.parse<T>` with
  `probeCfgParseCycleIsBalanced` staying at 0.
