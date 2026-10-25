# KernelBuffer

`cajeta.xpu.KernelBuffer` — device memory.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `xpu/kernelbuffer-alloc-free` | 1e4 allocate-and-free of 64 MB | `cudaMalloc`/`hipMalloc`, PyTorch caching allocator | count |
| `xpu/host-device-transfer` | copy 1 GB host → device → host | `cudaMemcpy`/`hipMemcpy`, PyTorch `.to()` | checksum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: xpu/kernelbuffer-alloc-free
metric: time
```

```bench
id: xpu/host-device-transfer
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench kernelbuffer-alloc-free               # Cajeta
./bench.sh --bench kernelbuffer-alloc-free --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [KernelBuffer](../../docs/stdlib/xpu/KernelBuffer.md)
- Source: `runtime/src/cajeta/xpu/KernelBuffer.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/xpu/`
