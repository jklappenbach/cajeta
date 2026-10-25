# Path

`cajeta.io.file.Path` — path joining and normalization.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `io/path-join-normalize` | 1e7 joins of three components with `..` segments (`resolve()` then a normalization the benchmark does itself; `Path` has no `normalize`) | C++ `std::filesystem::path`, Rust `PathBuf`, Go `filepath.Join`, Java `Path.resolve().normalize()`, Python `pathlib` | length sum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: io/path-join-normalize
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench path-join-normalize               # Cajeta
./bench.sh --bench path-join-normalize --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Path](../../../docs/stdlib/io/file/Path.md)
- Source: `runtime/src/cajeta/io/file/Path.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/io/`
