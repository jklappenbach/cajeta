# Watcher

`cajeta.io.file.Watcher` — filesystem change notification.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `io/watcher-event-latency` | 1e3 file touches; the row is the median latency to the event (Cajeta skips: `Watcher` is a placeholder with no watch call or event stream yet; the competitors run) | Rust `notify`, Go `fsnotify`, Java `WatchService`, Python `watchdog` | event count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: io/watcher-event-latency
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench watcher-event-latency               # Cajeta
./bench.sh --bench watcher-event-latency --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Watcher](../../../docs/stdlib/io/file/Watcher.md)
- Source: `runtime/src/cajeta/io/file/Watcher.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/io/`
