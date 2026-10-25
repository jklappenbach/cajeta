# Directory walk

`cajeta.io.file` — walking a tree. The tree is generated: 1e5 files across 1e3 directories.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `io/directory-walk` | enumerate every entry of the generated tree, 10 times | C++ `recursive_directory_iterator`, Rust `walkdir`, Go `filepath.WalkDir`, Java `Files.walk`, Python `os.scandir` | entry count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: io/directory-walk
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench directory-walk               # Cajeta
./bench.sh --bench directory-walk --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [File](../../../docs/stdlib/io/file/File.md)
- Source: `runtime/src/cajeta/io/file/`
- Benchmark source: `bench/src/main/cajeta/bench/io/`
