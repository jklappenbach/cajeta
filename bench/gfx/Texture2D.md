# Texture2D

`cajeta.gfx.Texture2D` — texture sampling from a kernel.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `gfx/texture-sample-rate` | sample a 4096² RGBA8 texture 2^28 times with bilinear filtering | CUDA texture objects, Vulkan sampled images | checksum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: gfx/texture-sample-rate
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench texture-sample-rate               # Cajeta
./bench.sh --bench texture-sample-rate --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Texture2D](../../docs/stdlib/gfx/Texture2D.md)
- Source: `runtime/src/cajeta/gfx/Texture2D.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/gfx/`
