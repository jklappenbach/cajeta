# KernelStream

`cajeta.xpu.KernelStream` — ordered device work. Two independent streams must overlap; the row is the speed-up of two streams over one.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `xpu/two-stream-overlap` | two 2^24 saxpy chains on two streams against one; the speed-up is in the row's notes (about 0.95 on the cpu backend, where `create()` returns the default stream) | CUDA and HIP streams | results equal, speed-up reported |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: xpu/two-stream-overlap
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench two-stream-overlap               # Cajeta
./bench.sh --bench two-stream-overlap --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [KernelStream](../../docs/stdlib/xpu/KernelStream.md)
- Source: `runtime/src/cajeta/xpu/KernelStream.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/xpu/`
