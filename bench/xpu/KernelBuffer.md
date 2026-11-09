# KernelBuffer

`cajeta.xpu.KernelBuffer` is device memory.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `xpu/kernelbuffer-alloc-free` | 1e4 allocate-and-free of 64 MB through the block cache (`CAJETA_XPU_ALLOC_CACHE=0` for the driver's cost) | `cudaMalloc`/`hipMalloc`, PyTorch caching allocator | count |
| `xpu/host-device-transfer` | copy 1 GB host → device → host | `cudaMemcpy`/`hipMemcpy`, PyTorch `.to()` | checksum |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: xpu/kernelbuffer-alloc-free
metric: time
```

```bench
id: xpu/host-device-transfer
metric: throughput
```

## Standing

Allocate and free is a win at 0.06x of torch's caching allocator, and 36000x ahead of hipMalloc and hipFree: the runtime's device block cache recycles blocks. Host-to-device transfer of pageable memory is 1.04x of hipMemcpy and 4.7x ahead of torch's `.to()`.

## Reproduce

```sh
cd bench
./bench.sh --bench kernelbuffer-alloc-free               # Cajeta
./bench.sh --bench kernelbuffer-alloc-free --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [KernelBuffer](../../docs/stdlib/xpu/KernelBuffer.md)
- Source: `runtime/src/cajeta/xpu/KernelBuffer.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/xpu/`
