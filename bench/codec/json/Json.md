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

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

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

## Standing

Tokenizing and bind-skip are 2x to 5x behind simdjson and within 1x to 2.3x of serde_json and Jackson, with Go's encoding/json 10x to 22x slower than Cajeta. Building a DOM is 18x to 43x behind simdjson and 3x to 4x behind serde_json; serializing is 20x behind yyjson and 5x to 9x behind serde_json and orjson; the round trip follows the DOM. The canada round trip and the conformance row did not pass their checks and carry no number. The tokenizer gap is simdjson's SIMD scanning; the DOM and serializer gaps are not yet investigated. Not yet scheduled.

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
