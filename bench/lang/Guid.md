# Guid

`cajeta.lang.Guid` — the 128-bit UUID held in one `uint128`.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `lang/guid-generate` | 1e6 random Guids | C++ libuuid, Rust `uuid`, Go `google/uuid`, Java `UUID.randomUUID`, Python `uuid4` | distinct count |
| `lang/guid-parse` | parse 1e6 canonical strings | same | round-trip equality |
| `lang/guid-format` | format 1e6 Guids | same | length |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: lang/guid-generate
metric: time
```

```bench
id: lang/guid-parse
metric: time
```

```bench
id: lang/guid-format
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench guid-generate               # Cajeta
./bench.sh --bench guid-generate --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Guid](../../docs/stdlib/lang/Guid.md)
- Source: `runtime/src/cajeta/lang/Guid.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/lang/`
