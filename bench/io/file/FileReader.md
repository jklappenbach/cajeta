# FileReader

`cajeta.io.file.FileReader` — buffered line and token reading.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `io/filereader-lines` | read 1e7 lines of a 500 MB file (1 MiB chunks and a newline count; `FileReader` has no line API) | C++ `getline`, Rust `BufRead::lines`, Go `bufio.Scanner`, Java `BufferedReader`, Python iteration | line count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: io/filereader-lines
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench filereader-lines               # Cajeta
./bench.sh --bench filereader-lines --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [FileReader](../../../docs/stdlib/io/file/FileReader.md)
- Source: `runtime/src/cajeta/io/file/FileReader.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/io/`
