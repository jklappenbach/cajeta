# Hash

`cajeta.hash.Hash` — the hashing protocol a user type implements.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `hash/hash-struct` | hash 1e7 three-field structs through the protocol | Rust `#[derive(Hash)]`, Java `Objects.hash`, Go `maphash` over fields, C++ `boost::hash_combine` | checksum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: hash/hash-struct
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench hash-struct               # Cajeta
./bench.sh --bench hash-struct --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Hash](../../docs/stdlib/hash/Hash.md)
- Source: `runtime/src/cajeta/hash/Hash.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/hash/`
