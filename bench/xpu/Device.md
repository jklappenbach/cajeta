# Device

`cajeta.xpu.Device` is the active device and the kernels that run on it. BabelStream gives sustained bandwidth, mixbench gives the roofline, and the GEMM rows exercise the matrix cores through `CooperativeMatrix`. The last row is the one no other suite has. It runs one `@Kernel` source on CUDA, HIP, Vulkan and the CPU with a consistency check. Device rows are measured in interleaved rounds because GPU clocks are bimodal.

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
| `xpu/gemm-f64` | 4096 × 4096 GEMM at float64 (the first suite's device matmul, kept under its true type) | cuBLAS and rocBLAS, PyTorch `matmul` | against host within epsilon |
| `xpu/gemm-f32` | 4096 × 4096 GEMM through `CooperativeMatrix` at float32 | cuBLAS and rocBLAS, PyTorch `matmul` | against host within epsilon |
| `xpu/gemm-f16` | the same at float16 with float32 accumulate | same | within epsilon |
| `xpu/gemm-bf16` | the same at bfloat16 | same | within epsilon |
| `xpu/mandelbrot-device` | mandelbrot at 16000 × 16000 on device | CUDA and HIP C++ | within 1e-5 relative of the host (FMA contraction moves a handful of pixels) |
| `xpu/mixbench-sweep` | the mixbench operational-intensity sweep, points 0 to 256 as variants, on a fixed-point input so the check is exact | mixbench CUDA and HIP reference builds | exact |
| `xpu/launch-overhead` | 1e4 trivial dispatches; the row is the time for all 1e4 (divide by 1e4 for a launch) | CUDA and HIP empty kernels, Vulkan compute | count |
| `xpu/cross-backend-showcase` | the saxpy, reduce and GEMM kernels from one source on every backend present; variants cuda, hip, vulkan, cpu | Cajeta's own backends | bit-exact where exact, epsilon where not |

Rows record the backend and driver. CUDA and Vulkan rows come from Phoenix, and HIP and Vulkan rows from Proton. The compiler manifests on v0.46.3 report that bf16 is native matrix-core on NVPTX and the software tier on Vulkan, f32 is the software tier on NVPTX and native on Vulkan, and f16 is native on both, with no kernel spills. The CUDA competitor is one `devserver` program with cuBLAS, CUB and texture objects, and it also builds with hipcc on a ROCm machine.

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

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
id: xpu/gemm-f64
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

## Standing

On the Radeon 8060S the bandwidth rows are at parity: the five BabelStream kernels within 4% of the HIP C++ BabelStream, SAXPY at 1.00x of torch, mixbench within 4% to 12% of the HIP mixbench across the sweep, Mandelbrot a win at 0.95x. Launch overhead is a win at 0.17x of a HIP `<<<>>>` launch. Reduce is 1.16x of rocPRIM's DeviceReduce; scan 3.6x behind DeviceScan (cajeta-ml-gpu 18.2.2). GEMM: f64 is a win at 0.55x of torch and 1.9x ahead of rocBLAS; f32 is 1.9x behind rocBLAS; f16 is 2x ahead of rocBLAS and 2.9x behind torch's hipBLASLt path; bf16 is 18x behind torch and 3x behind rocBLAS, its strip kernel reading both operands from global memory on every K-step (23.2.2). The vulkan rows run at smoke size only and are not published.

Since that run, measured 2026-11-09 on the same box at full size and waiting for the next published run: scan is a single-pass look-back scan at 2.41 ms, 0.96x of DeviceScan; wave-reduce loads four floats a step, 1.06x of DeviceReduce; bf16 GEMM runs a 128x128 tile at 4.60 ms, 5.0x ahead of rocBLAS and 0.88x of torch.

## Reproduce

```sh
cd bench
./bench.sh --bench babelstream-copy               # Cajeta
./bench.sh --bench babelstream-copy --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Device](../../docs/stdlib/xpu/Device.md)
- Source: `runtime/src/cajeta/xpu/Device.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/xpu/`
