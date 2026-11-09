# KernelTimer

`cajeta.xpu.KernelTimer` is the per-kernel timer. Every device number on this site depends on its resolution and its agreement with the host clock, and the row publishes both.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `xpu/kerneltimer-resolution` | 1e3 brackets of an empty kernel; the row is the minimum readable interval and the calibrated scale against the host clock on a busy bracket (Vulkan skips: no event clock) | CUDA events, HIP events | scale within 1% |

On Phoenix under WSL2 the CUDA event clock runs 4.56% fast. The timer calibrates once and the row shows the scale.

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: xpu/kerneltimer-resolution
metric: time
```

## Standing

Reading the timer is a win at 0.03x of a HIP event pair.

## Reproduce

```sh
cd bench
./bench.sh --bench kerneltimer-resolution               # Cajeta
./bench.sh --bench kerneltimer-resolution --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [KernelTimer](../../docs/stdlib/xpu/KernelTimer.md)
- Source: `runtime/src/cajeta/xpu/KernelTimer.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/xpu/`
