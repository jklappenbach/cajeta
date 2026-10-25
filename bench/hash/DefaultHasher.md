# DefaultHasher

`cajeta.hash.DefaultHasher` — the hasher behind `String.hash()`, which a `HashMap` with string keys calls. These rows are the hashing share of the `collection/hashmap-*` numbers. Integer keys do not go through it: the map calls the `int64` hash intrinsic, so the int row times that call.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `hash/defaulthasher-string` | hash 1e7 16-byte strings from a pool of 1024 (`String.hash()` equals the `DefaultHasher` digest) | Rust `DefaultHasher`, Go `maphash`, Java `Arrays.hashCode` (uncached; `String.hashCode` caches), Python `hash` (cached; noted) | checksum |
| `hash/defaulthasher-int` | hash 1e8 int64 through the map's int64 intrinsic | same | checksum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: hash/defaulthasher-string
metric: time
```

```bench
id: hash/defaulthasher-int
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench defaulthasher-string               # Cajeta
./bench.sh --bench defaulthasher-string --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [DefaultHasher](../../docs/stdlib/hash/DefaultHasher.md)
- Source: `runtime/src/cajeta/hash/DefaultHasher.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/hash/`
