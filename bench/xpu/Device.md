# Device

`cajeta.xpu.Device` — the active device and the kernels that run on it. BabelStream gives sustained bandwidth, mixbench the roofline, the GEMM rows the matrix cores through `CooperativeMatrix`, and the last row is the one no other suite has: one `@Kernel` source run on CUDA, HIP, Vulkan and the CPU with a consistency check. Device rows are measured in interleaved rounds because GPU clocks are bimodal.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `xpu/babelstream-copy` | BabelStream copy at 2^25 float64 | BabelStream CUDA and HIP reference builds | checksum |
| `xpu/babelstream-mul` | mul | same | checksum |
| `xpu/babelstream-add` | add | same | checksum |
| `xpu/babelstream-triad` | triad, the canonical bandwidth number | same | checksum |
| `xpu/babelstream-dot` | dot | same | within epsilon |
| `xpu/device-saxpy` | `y = a*x + y` on device at 2^26 float32 | CUDA and HIP C++, PyTorch | against host |
| `xpu/wave-reduce` | sum of 2^26 float32 with wave reductions | CUB and rocPRIM `DeviceReduce`, PyTorch `sum` | within epsilon |
| `xpu/scan` | inclusive scan of 2^26 int32 | CUB and rocPRIM `DeviceScan`, PyTorch `cumsum` | last element and spot checks |
| `xpu/gemm-f32` | 4096 × 4096 GEMM through `CooperativeMatrix` at float32 | cuBLAS and rocBLAS, PyTorch `matmul` | against host within epsilon |
| `xpu/gemm-f16` | the same at float16 with float32 accumulate | same | within epsilon |
| `xpu/gemm-bf16` | the same at bfloat16 | same | within epsilon |
| `xpu/mandelbrot-device` | mandelbrot at 16000 × 16000 on device | CUDA and HIP C++ | checksum |
| `xpu/mixbench-sweep` | the mixbench operational-intensity sweep; variants per intensity | mixbench CUDA and HIP reference builds | checksum |
| `xpu/launch-overhead` | 1e4 trivial dispatches; the row is time per launch | CUDA and HIP empty kernels, Vulkan compute | count |
| `xpu/cross-backend-showcase` | the saxpy, reduce and GEMM kernels from one source on every backend present; variants cuda, hip, vulkan, cpu | Cajeta's own backends | bit-exact where exact, epsilon where not |

Rows record the backend and driver; CUDA and Vulkan rows come from Phoenix, HIP and Vulkan rows from Proton.

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: xpu/babelstream-copy
metric: throughput
```

```bench
id: xpu/babelstream-mul
metric: throughput
```

```bench
id: xpu/babelstream-add
metric: throughput
```

```bench
id: xpu/babelstream-triad
metric: throughput
```

```bench
id: xpu/babelstream-dot
metric: throughput
```

```bench
id: xpu/device-saxpy
metric: throughput
```

```bench
id: xpu/wave-reduce
metric: throughput
```

```bench
id: xpu/scan
metric: throughput
```

```bench
id: xpu/gemm-f32
metric: throughput
```

```bench
id: xpu/gemm-f16
metric: throughput
```

```bench
id: xpu/gemm-bf16
metric: throughput
```

```bench
id: xpu/mandelbrot-device
metric: throughput
```

```bench
id: xpu/mixbench-sweep
metric: throughput
```

```bench
id: xpu/launch-overhead
metric: time
```

```bench
id: xpu/cross-backend-showcase
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench babelstream-copy               # Cajeta
./bench.sh --bench babelstream-copy --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Device](../../docs/stdlib/xpu/Device.md)
- Source: `runtime/src/cajeta/xpu/Device.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/xpu/`
