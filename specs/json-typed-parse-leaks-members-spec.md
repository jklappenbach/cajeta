# `Json.parse<T>` leaks every owned member it deserializes

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

### Where to look next

The caller's `#=` on the synthesized call is not arming a drop for the
result, even though every equivalent hand-written shape does. Per
CLAUDE.md §2.1 a plain return's drop entry is armed from the ARRIVING
RETURN FLAG (`LocalVariableDeclaration.cpp:251`), so the question is
whether the synthesized instantiation SETS that flag on return. Instrument
the flag at the boundary — `__cajeta_return_flag_get` after the call —
rather than inferring from source shape, which is what refuted 1–5.

## Fix

The JSON synthesizer (`src/cajeta/codec/JsonSynthesizer.cpp`) must store
deserialized reference members with the mode-carrying store that records
a title (`#=` semantics), so the DTO's drop walks and frees them. The CSV
synthesizer shares the shape and should be checked in the same pass, as
should `Json.parse<T>`'s siblings (`parseObjectFromReader<T>`,
`walkValue<T>`, `walkElement<T>`).

## Acceptance

- The repro above returns **0**.
- A DTO with a nested object, a `String[]`, and a `String` returns 0.
- An owned member that the document does NOT supply (field left at its
  default) is still safe to drop — no double-free on the absent case.
- cajeta-llama's `ModelConfig` can bind through `Json.parse<T>` with
  `probeCfgParseCycleIsBalanced` staying at 0.
