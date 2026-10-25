# FiberLocal

`cajeta.concurrent.FiberLocal` — ambient per-request state.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `concurrent/fiberlocal-get-set` | 1e8 get and set on one fiber | C++ `thread_local`, Rust `thread_local!`, Go (none; `context.Value`), Java `ScopedValue` and `ThreadLocal`, Python `contextvars` | sum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: concurrent/fiberlocal-get-set
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench fiberlocal-get-set               # Cajeta
./bench.sh --bench fiberlocal-get-set --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [FiberLocal](../../docs/stdlib/concurrent/FiberLocal.md)
- Source: `runtime/src/cajeta/concurrent/FiberLocal.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/concurrent/`
