# HashMap

`cajeta.collection.HashMap` — the SwissTable-backed map over a dense entry array. Integer and string keys are measured separately because the string rows pay for hashing and comparison and the integer rows do not; the methodology follows Martin Ankerl's hashmap suite.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/hashmap-int` | insert 1e6 int64 keys, then look each up | C++ `unordered_map` and `unordered_dense`, Rust `HashMap` and `hashbrown`, Go map, Java `HashMap`, Python dict | sum of values |
| `collection/hashmap-string` | insert 1e6 16-byte string keys, then look each up | same | sum |
| `collection/hashmap-lookup-hit` | 1e7 lookups that hit, 1e6-entry map | same | sum |
| `collection/hashmap-lookup-miss` | 1e7 lookups that miss | same | miss count |
| `collection/hashmap-iterate` | iterate a 1e6-entry map 10 times | same | sum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: collection/hashmap-int
metric: time
```

```bench
id: collection/hashmap-string
metric: time
```

```bench
id: collection/hashmap-lookup-hit
metric: time
```

```bench
id: collection/hashmap-lookup-miss
metric: time
```

```bench
id: collection/hashmap-iterate
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench hashmap-int               # Cajeta
./bench.sh --bench hashmap-int --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [HashMap](../../docs/stdlib/collection/HashMap.md)
- Source: `runtime/src/cajeta/collection/HashMap.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/collection/`
