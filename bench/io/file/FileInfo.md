# FileInfo

`cajeta.io.file.FileInfo` — file metadata.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `io/file-stat` | 1e6 `stat` of an existing file | C++ `stat`, Rust `metadata`, Go `os.Stat`, Java `Files.readAttributes`, Python `os.stat` | size sum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: io/file-stat
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench file-stat               # Cajeta
./bench.sh --bench file-stat --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [FileInfo](../../../docs/stdlib/io/file/FileInfo.md)
- Source: `runtime/src/cajeta/io/file/FileInfo.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/io/`
