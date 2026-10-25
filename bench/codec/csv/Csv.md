# CSV

`cajeta.codec.csv.Csv` — the CSV reader and writer on a generated 100 MB file with quoted fields and embedded commas.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `codec/csv-parse` | parse 100 MB into typed rows | Rust `csv`, Go encoding/csv, Java `univocity`, Python pandas `read_csv` and the csv module, C++ `fast-cpp-csv-parser` | row and field counts |
| `codec/csv-write` | write the same rows back | same | byte count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: codec/csv-parse
metric: throughput
```

```bench
id: codec/csv-write
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench csv-parse               # Cajeta
./bench.sh --bench csv-parse --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Csv](../../../docs/stdlib/codec/csv/Csv.md)
- Source: `runtime/src/cajeta/codec/csv/Csv.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/codec/`
