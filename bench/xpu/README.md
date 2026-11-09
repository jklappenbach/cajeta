# cajeta.xpu

This section measures device bandwidth, the roofline, reductions, scans, GEMM on the matrix cores, launch overhead, and one kernel source on every backend. CUDA and Vulkan rows are from Phoenix, and HIP and Vulkan rows from Proton. Every device row carries its driver.

| Page | |
|---|---|
| [Device](Device.md) | The active device and the kernels that run on it |
| [KernelBuffer](KernelBuffer.md) | Device memory |
| [KernelStream](KernelStream.md) | Ordered device work |
| [KernelTimer](KernelTimer.md) | The per-kernel timer |
| [MeshSimplifier](mesh/MeshSimplifier.md) | Mesh simplification on device |

## Standing

Geometric mean 1.2x behind over 30 benchmarks on the Radeon 8060S, 8 wins and 22 losses, most within 10% of HIP C++. Bandwidth, launch, streams, timer, transfer and allocation are at parity or wins; f64 and f16 GEMM beat rocBLAS. The losses that matter are scan (3.6x behind rocPRIM), bf16 GEMM (18x behind torch) and f32 GEMM (1.9x behind rocBLAS), tracked in cajeta-ml-gpu 18.2.2 and 23.2.2. Phoenix (CUDA) rows arrive with its run.
