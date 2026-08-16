---
id: lang-overview
applies-to: [cajeta.lang]
title: cajeta.lang — stdlib root orientation & task routing
description: Map of cajeta.lang (Object, String, numeric tower, Optional/Pair/Guid, encoding, Math, streams) with ownership and error conventions.
---

# cajeta.lang — the stdlib root

The always-available core every cajeta program builds on: the `Object` root, the
single `String` type, the boxed numeric tower, the small value types
(`Optional`, `Pair`, `Guid`), text-encoding identifiers, `Math`, and the
`cajeta.lang.stream` pull-iteration framework. If you need text, numbers, an
optional/absent value, identifiers, or iteration, start here.

## Task → entry point

| You want to… | Start with | Note |
|---|---|---|
| Hold/transform text | `String` | immutable UTF-8; transforms return `#String` |
| Codepoint count vs byte count | `String.count()` / `String.size()` | **no `length()`** — it was removed as ambiguous |
| Represent maybe-absent value | `Optional<T>` | value type; `get()` throws on empty in v1 |
| Pair two values | `Pair<K, V>` | `first()` / `second()`, immutable surface |
| 128-bit UUID | `Guid` | `Guid.random()` / `Guid.parse(s)` → `#Guid` |
| Box a primitive number into an `Object` slot | `Int32.of(x)` … `Float64.of(x)` | wrappers extend `Number`; `of()` → `#Wrapper` |
| Accept "any number" generically | bound `<T extends Numeric>` (or `Integral`/`Floating`) | primitives satisfy intrinsically — no boxing, no `implements` |
| max / min / clamp / sqrt / sin … | `Math` | `max/min/clamp` are method-templated statics; transcendentals are compiler intrinsics (not declared) |
| Iterate / map / filter / reduce | `cajeta.lang.stream` (`ArrayStream` → `Stream<T>`) | pull protocol via `next() -> Optional<T>` |
| Name a text encoding | `Encoding` (enum) | no platform default — name it explicitly |
| Total ordering for a value type | implement `Comparable<T>` | `compareTo` returns signed int32 |
| **NOT here:** collections (List/HashMap), JSON/codecs, IO/files, time, exceptions base | `cajeta.collection`, `cajeta.codec`, `cajeta.io`, `cajeta.time`, `cajeta.error` | `Pair`/`Optional` live here, but the containers that produce them do not |
| **NOT here:** mutable/builder strings, regex, `printf`-style format | — | `String` is read-only; build via repeated transforms or (future) a builder elsewhere |
| **NOT here:** structural `==`/`toString()`/`clone()` auto-derivation | override manually | defaults are identity/placeholder — see hazards |

## Cross-cutting invariants (learn once, apply library-wide)

- **`#` is ownership transfer.** A `#T` parameter consumes its argument; a `#T`
  return hands the caller a new owned value to bind and (at scope exit) free.
  Every `String` transform (`trim`, `substring`, `toUpperCase`, `replace`, …)
  returns a fresh `#String`; every `Guid` factory and the numeric-wrapper `of()`
  return `#Guid` / `#Wrapper`. **Query methods that only read allocate nothing**
  (`count`, `size`, `contains`, `indexOf`, `startsWith`, `charAt`, `equals`).
- **Storage is caller-chosen.** Value types (`Optional`, `Pair`, `Guid`,
  wrappers) are placed with `stack Foo(...)` (lifetime = enclosing scope, no heap)
  or `heap Foo(...)` (escapes; owned `#`). `String` literals/transforms are heap.
- **Destruction is deterministic and virtual.** Every class implicitly extends
  `Object`; dropping at scope exit dispatches the most-derived `~T()` up the
  chain, then frees. No `close()` discipline in this library — there are no
  open handles to release here.
- **Equality routes through `hash()`.** `Object.operator==` is null-safe and
  defined as `a.hash() == b.hash()`. The default `Object.hash()` is *pointer
  identity* (address mixed with a per-process seed), so by default `==` means
  reference identity. Types get value-equality by overriding **`hash()` alone**
  (`String` = FNV-1a, `Guid`, the wrappers) — they do *not* declare `operator==`.
  `!=` is auto-derived.
- **Errors are thrown exceptions, not return codes.** `Guid.parse` throws
  `GuidFormatException`, encoding failures throw `EncodingException` — both
  `extends cajeta.error.RecoverableException` (catch at the boundary). Streams
  throw `cajeta.error.Exception`. Newer absence-aware APIs prefer returning
  `Optional<T>` over sentinels.
- **Null conventions.** Class references are nullable; compare against the
  `null` literal (a pointer compare, not a `hash()` dispatch). `Optional<T>`
  is the explicit "maybe absent" carrier. Out-of-range `charAt`/`codepointAt`
  return `0` rather than throwing.

## Canonical end-to-end example

```cajeta
import cajeta.lang.String;
import cajeta.lang.Optional;
import cajeta.lang.stream.ArrayStream;
import cajeta.lang.stream.Stream;

String greeting = "  Hello, World  ";        // a literal is a static view
String clean #= greeting.trim();              // owned: "Hello, World"
if (clean.contains("World")) {
    int64 cps = clean.count();                // 12 codepoints (not bytes)
}

int32[] nums = { 1, 2, 3, 4, 5 };
Stream<int32> s = heap ArrayStream<int32>(nums, 5);
int64 sumEven #= s.filter((x) -> x % 2 == 0)
                 .fold<int64>(0L, (acc, e) -> acc + e);   // 6

Optional<int32> first = (heap ArrayStream<int32>(nums, 5))
        .findFirst((x) -> x > 3);
int32 v = first.orElse(-1);                   // 4
```

## Hazards (library-wide, non-obvious)

- **`String` is immutable.** There is no in-place mutation and no `length()`;
  use `count()` (codepoints) or `size()` (bytes), which diverge for non-ASCII.
- **`==` on value types carries a ~2⁻⁶⁴ collision risk** because it compares
  hashes. For certainty use the type's exact check: `String.equals`,
  `Guid.equals`.
- **`Object.toString()` and `clone()` return `null` today** (synthesizer not
  landed) and `hash()` defaults to *identity*, not structural. Value-keyed
  classes must override `hash()` manually; for debug text, override `toString()`.
- **`Optional.get()` on an empty Optional throws** (v1: raw error code `1`).
  Guard with `isPresent()` or use `orElse(fallback)`.
- **`Encoding` has no platform default and its byte↔text methods aren't wired
  yet** — `Encoding`/`EncodingErrorPolicy` values are selectable by name, but
  `String.getBytes`/`fromBytes` land in a later phase. `EncodingErrorPolicy`
  has only `FAIL` (default) and `REPAIR` — no silent `IGNORE`.
- **`Math` transcendentals (`sqrt`, `sin`, `pow`, …) are compiler intrinsics**,
  not methods declared on `Math` — call `Math.sqrt(x)` and the compiler lowers
  it (host and `@Kernel`). Only `max`/`min`/`clamp` are real declared statics.
- **`Optional`/`Stream` combinator coverage is partial in v1.** `Optional` has
  no `map`/`filter`/`flatMap` yet (ctor + `isPresent`/`isEmpty`/`get`/`orElse`
  only). `Stream` has the full combinator surface but interfaces relying on
  templated-vtable dispatch (`Comparable`, `Splittable`) are still maturing —
  see each type's doc comment.
- **`String.byteAt`/`charAt` are not bounds-checked** under `--bounds=off`
  (array convention); `charAt`/`codepointAt` return `0` for out-of-range
  indices rather than throwing.

## Disambiguation

- **`String.equals` vs `==`** — `equals` is an exact byte-for-byte compare; `==`
  is the hash-based shortcut good enough for hash-keyed collections.
- **`count()` vs `size()`** — codepoints vs bytes; equal only for pure ASCII.
- **`Optional<T>` vs nullable reference** — use `Optional` for an intentional
  maybe-value in a return type; use plain `null` for an unset field/leaf.
- **Primitive numerics vs `Number` wrappers** — work with `int32`/`float64`
  directly for math; box to `Int32`/`Float64` only to flow through an
  `Object`-typed slot (generic container, reflection).
- **`reduce`/`fold` vs `collect`** — `fold<R>` for a scalar accumulator;
  `collect<R>(Collector)` to build a container.

## Setup

Implicitly available to every cajeta unit (`Object`, `String`, the primitive
wrappers, literals). Other symbols import by canonical name, e.g.
`import cajeta.lang.Optional;`, `import cajeta.lang.stream.ArrayStream;`.
The stream package additionally references `cajeta.collection.Collector` and
`cajeta.error.Exception`.

## Go deeper

- Text: `cajeta.lang.String`, `cajeta.lang.Encoding`,
  `cajeta.lang.EncodingErrorPolicy`, `cajeta.lang.EncodingException`.
- Numbers: `cajeta.lang.Number` + the `Int8…Float128` wrappers; the
  `Numeric`/`Integral`/`Floating`/`Comparable` markers; `cajeta.lang.Math`.
- Value types: `cajeta.lang.Optional`, `cajeta.lang.Pair`, `cajeta.lang.Guid`
  (+ `cajeta.lang.GuidFormatException`).
- Iteration: the `cajeta.lang.stream` package (`Stream`, `ArrayStream`,
  the `*Stream` combinators, `Splittable`, `ParallelDriver`).
- Root semantics & memory model: `cajeta.lang.Object`.
