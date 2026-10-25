# KernelTimer

`cajeta.xpu.KernelTimer` — the per-kernel timer. Its resolution and its agreement with the host clock are what every device number on this site depends on; the row publishes both.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `xpu/kerneltimer-resolution` | 1e3 brackets of an empty kernel; the row is the minimum readable interval and the calibrated scale against the host clock | CUDA events, HIP events, Vulkan timestamp queries | scale within 1% |

On Phoenix under WSL2 the CUDA event clock runs 4.56% fast; the timer calibrates once and the row shows the scale.

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: xpu/kerneltimer-resolution
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench kerneltimer-resolution               # Cajeta
./bench.sh --bench kerneltimer-resolution --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [KernelTimer](../../docs/stdlib/xpu/KernelTimer.md)
- Source: `runtime/src/cajeta/xpu/KernelTimer.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/xpu/`
