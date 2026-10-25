# FileWriter

`cajeta.io.file.FileWriter` — buffered writing.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `io/filewriter-lines` | write 1e7 50-byte lines (`FileWriter` is unbuffered: one write per line) | C++ `ofstream`, Rust `BufWriter`, Go `bufio.Writer`, Java `BufferedWriter`, Python `write` | size |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: io/filewriter-lines
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench filewriter-lines               # Cajeta
./bench.sh --bench filewriter-lines --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [FileWriter](../../../docs/stdlib/io/file/FileWriter.md)
- Source: `runtime/src/cajeta/io/file/FileWriter.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/io/`
