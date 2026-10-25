# Cache

`cajeta.collection.Cache` — the bounded cache with LRU eviction and optional TTL.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/cache-hit` | 1e7 gets that hit, capacity 1e5 | Java Caffeine, Rust `moka` and `lru`, Go `ristretto`, Python `functools.lru_cache` | hit count |
| `collection/cache-miss` | 1e7 gets that miss and insert | same | size |
| `collection/cache-evict` | insert 1e7 into capacity 1e5 (steady eviction) | same | size |
| `collection/cache-ttl` | 1e6 inserts with a 1 ms TTL, read back after expiry | Caffeine, `moka`, `ristretto` | expired count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: collection/cache-hit
metric: time
```

```bench
id: collection/cache-miss
metric: time
```

```bench
id: collection/cache-evict
metric: time
```

```bench
id: collection/cache-ttl
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench cache-hit               # Cajeta
./bench.sh --bench cache-hit --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Cache](../../docs/stdlib/collection/Cache.md)
- Source: `runtime/src/cajeta/collection/Cache.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/collection/`
