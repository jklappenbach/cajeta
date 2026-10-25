# JSON

`cajeta.codec.json` — the JSON reader, binder, DOM and writer, measured on the nativejson-benchmark corpus (twitter, citm_catalog, canada) the way that suite measures: tokenize to end of document, bind with skipping, build a DOM, serialize, round-trip, and the JSON_checker conformance set. These six rows are the oldest on the site; the first `bench/` note was this comparison.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `codec/json-tokenize` | pull every token of each corpus file; variants twitter, citm, canada | simdjson, yyjson, serde_json, Go encoding/json, Jackson, orjson | token count |
| `codec/json-bind-skip` | bind a typed record from each document, skipping unknown fields | same | field checksum |
| `codec/json-dom` | build the full value tree | same | node count |
| `codec/json-serialize` | write the tree back out | same | byte count |
| `codec/json-roundtrip` | parse then serialize | same | equality |
| `codec/json-conformance` | the JSON_checker pass and fail files | same | accept and reject counts |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: codec/json-tokenize
metric: throughput
```

```bench
id: codec/json-bind-skip
metric: throughput
```

```bench
id: codec/json-dom
metric: throughput
```

```bench
id: codec/json-serialize
metric: throughput
```

```bench
id: codec/json-roundtrip
metric: throughput
```

```bench
id: codec/json-conformance
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench json-tokenize               # Cajeta
./bench.sh --bench json-tokenize --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Json](../../../docs/stdlib/codec/json/Json.md)
- Source: `runtime/src/cajeta/codec/json/`
- Benchmark source: `bench/src/main/cajeta/bench/codec/`
