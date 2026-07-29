# Pair\<K, V\>

`cajeta.lang.Pair` — two-field generic value type, the foundation for stdlib
APIs that pair two values together (`HashMap.entries()` returns a `Stream` of
`Pair<K, V>`, `ImmutableMap` is built from a list of pairs). The surface
exposes only the `first()` and `second()` getters — mutating the fields after
construction isn't supported; construct a new pair instead.

```cajeta
Pair<String, int32> p = heap Pair<String, int32>("ada", 36);
String who = p.first();   // "ada"
int32 yrs = p.second();   // 36
```

## Methods

| Signature | |
|---|---|
| `Pair(K first, V second)` ⚑ | Builds a pair from its two components |
| `K first()` | The first component |
| `V second()` | The second component |

⚑ = `@EntryPoint`

## See also

- Tour: [ImmutableMapDemo](../../../samples/tour/src/main/cajeta/tour/collection/ImmutableMapDemo.cajeta)
- Source: [`runtime/src/cajeta/lang/Pair.cajeta`](../../../runtime/src/cajeta/lang/Pair.cajeta)
- [HashMap](../collection/HashMap.md) — `entries()` streams pairs
- [ImmutableMap](../collection/ImmutableMap.md) — built from `ArrayList<Pair<K, V>>`
