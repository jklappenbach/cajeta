# SchemaEncoder

`cajeta.wire.SchemaEncoder` — the schema-driven encoder. protobuf and Cap'n Proto compile their schemas ahead of time, so they are measured through their runtime-schema paths (`DynamicMessage`, `DynamicStruct`); Avro through `GenericDatum`.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `wire/schemaencoder-encode` | encode 1e6 records through a `Schema` | protobuf, Cap'n Proto, Avro | byte count |
| `wire/schemaencoder-decode` | decode them | same | checksum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: wire/schemaencoder-encode
metric: throughput
```

```bench
id: wire/schemaencoder-decode
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench schemaencoder-encode               # Cajeta
./bench.sh --bench schemaencoder-encode --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [SchemaEncoder](../../docs/stdlib/wire/SchemaEncoder.md)
- Source: `runtime/src/cajeta/wire/SchemaEncoder.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/wire/`
