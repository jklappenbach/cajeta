# File

`cajeta.io.file.File` — whole-file and positioned reads and writes. The sequential rows are measured on the run's storage device, named in the row; tmpfs is excluded.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `io/file-read-sequential` | read a 1 GB file in 1 MB chunks | C++ `read`, Rust `std::fs`, Go `os.File`, Java `FileChannel`, Python `open` | checksum |
| `io/file-write-sequential` | write 1 GB in 1 MB chunks with `fsync` at the end | same | size |
| `io/file-random-read` | 1e5 random 4 KB reads from the 1 GB file | same | checksum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: io/file-read-sequential
metric: throughput
```

```bench
id: io/file-write-sequential
metric: throughput
```

```bench
id: io/file-random-read
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench file-read-sequential               # Cajeta
./bench.sh --bench file-read-sequential --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [File](../../../docs/stdlib/io/file/File.md)
- Source: `runtime/src/cajeta/io/file/File.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/io/`
