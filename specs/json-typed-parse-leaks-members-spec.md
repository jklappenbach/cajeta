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

**Leading hypothesis, untested:** locals in SYNTHESIZED methods may not
get scope-drop entries emitted at all, which would explain the
field-independent cursor leak surviving a correct `#=`. Check whether the
synthesized body participates in normal drop-chain emission before
chasing binding modes further.

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
