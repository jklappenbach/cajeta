# 17 — Accelerated Compute (XPU)

This chapter defines the accelerator programming model, package `cajeta.xpu`. One portable kernel source compiles for four backends — NVIDIA (NVPTX), AMD (AMDGPU), Vulkan (SPIR-V), and CPU — and a single binary may bundle several, selecting at launch. The model's central claim: write kernels against the portable surface — `KernelBuffer`, `Tile`, the coordinate and wave classes — and the scheduler takes multiple kernels and, from the hardware profile it runs on, finds the scheduling for compute given the program's declared goals and constraints (§17.8). The borrow checker's guarantees hold across the host/device boundary (§17.6).

## 17.1 Kernel Declarations and the Device Subset

`@Kernel` on a static method makes it a device entry point; `@Device` marks a static helper callable from kernel code; `@Backend` restricts a declaration to named backends; `@Wave` marks wave-level code (§17.5). `@FastMath` permits fused floating-point contraction in the marked kernel.

A kernel body is ordinary Cajeta restricted to the device subset:

- No heap allocation and no exceptions. Locals, control flow (including labeled `break`/`continue`), and calls to `@Device` helpers are available.
- Function-typed values are bounded: a device function value is a tag over a closed set of `@Device` statics, and a call through one lowers to direct calls on every backend — device code never requires function pointers. An out-of-range dispatch index is a defined no-op.
- Parameters are the marshallable kinds of §17.4.

## 17.2 Execution Geometry

A launch names a grid of workgroups and a workgroup size, in up to three dimensions. Inside a kernel, coordinates read through class statics: `KernelThread.x()` (thread in workgroup), `KernelThread.globalIdX()` (global), `Workgroup.x()`/`.y()`/`.z()` and `Workgroup.dimX()`, `Wave.width()` and `Wave.laneId()`.

The grid-stride loop is built in: `for (i, T v : buf.range(n))` iterates a buffer with the canonical global-id/grid-size stride on every backend.

## 17.3 Memory

- **`KernelBuffer<T>`** is device memory, allocated on the host (`heap KernelBuffer<float32>(n)`), filled and drained with `upload(hostArray)` / `download(hostArray)` (and their `Async` forms against a stream). Kernels index it; host code does not.
- **Workgroup-shared memory** is declared in-kernel: `Shared<float64> tile = shared float64[512];`. `Barrier` synchronizes the workgroup.
- **Atomics and `MemoryOrder`** provide device-side atomic operations; `AsyncCopy` overlaps global-to-shared movement with compute where the backend supports it.

## 17.4 Kernel Arguments

A kernel's parameters marshal by kind: `KernelBuffer<T>` as a device handle; primitives by value; a plain class whose fields are all primitives — a POD, no inheritance, no marker interface — by value, read-only, field-by-field; `Texture2D` with `Sampler` for sampled reads (`tex.sample(s, u, v)`, explicit-LOD, with cube and mipmapped variants); `@PushConstant` where the backend has the concept. Ownership of buffer arguments is §17.6.

**Example 17.4-1.** The canonical kernel shape. Requires an XPU-enabled build (`--xpu-backend=…`); the compute pipeline is pinned by the on-device test suites.

<!-- snippet: skip -->
```cajeta
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public final class K {
    @Kernel
    public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x, float32 a, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) { y[i] = a * x[i] + y[i]; }
    }
    public static void run() {
        float32[] hx = heap float32[1024];
        float32[] hy = heap float32[1024];
        KernelBuffer<float32> x = heap KernelBuffer<float32>(1024);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(1024);
        x.upload(hx);  y.upload(hy);
        KernelStream s #= KernelStream.current();
        saxpy.launch(s, grid: [4], block: [256])(y, x, 3.0f, (uint32) 1024);
        s.sync();
        y.download(hy);
    }
}
```

## 17.5 Tiles and Cooperative Matrix Compute

`Tile<T, Rows, Cols>` is the portable cooperative-matrix fragment: a tile of a matrix multiply held cooperatively by the group, device-only and register-resident — never addressable, never a host value. The author declares element type and shape and **never a role**: `Group.mac(c, a, b)` computes `c = a·b + c`, and the accumulator/operand roles are inferred from argument position. A K-loop accumulates in place over sub-tiles selected by offset and stride. The explicit `CooperativeMatrix<T, Rows, Cols, Use>` remains for the residual paths that must name a role.

**Tiering.** `Group.mac` selects the fastest available realization per element type and shape: the native tensor core (WMMA on AMD, wmma on NVIDIA), the SIMD integer dot (dp4a) for `int8`, or the portable software tile. Selection is per group and whole: a shape that straddles capability demotes the entire GEMM one tier rather than splitting it, and the chosen tier is reported once through a `[mma-tiering]` compiler note — never silently.

## 17.6 Launch, Completion, and Ownership

`kernel.launch(stream, grid: […], block: […])(args)` enqueues on a `KernelStream`; `stream.sync()` completes it, and `Event`s order work across streams. The runtime backend dispatcher selects among the backends bundled in the binary — CUDA, then HIP, then Vulkan, then CPU — at launch time; kernel code written to the portable surface runs wherever the program lands.

The borrow checker holds across the boundary: a buffer lent to a launch is borrowed by the in-flight kernel, and the borrow resolves at `sync()` — a program cannot drop or re-transfer a buffer an enqueued kernel still reads. This is the deferred-borrow-until-sync rule.

## 17.7 Capabilities

`Capability` / `Capabilities` express per-device feature availability — wave operations, cooperative matrix, texture kinds — and programs query them at run time; `@Backend` gates code at compile time. A feature absent on the selected backend is either emulated where the specification says so (AMD cube and mipmapped textures are emulated over layered arrays and a hand-built descriptor) or unavailable through the capability query, never silently wrong.

## 17.8 Kernel Scheduling

Every launch is also a **submission**: `Scheduler.submit(sub)` records a `KernelSubmission` — geometry, policy, and the kernel's read/write sets — and returns the kernel's resource descriptor. The access sets are the raw material of the dependency DAG; the launch takes its geometry from the recorded submission.

The scheduling model over that seam:

- **Kernel manifests.** For each (kernel, target) pair the compiler emits a manifest beside the artifact: footprint from the code object (registers, spill, shared memory), cost expressions over the kernel's own parameters, access modes (including `accumulate` and `streaming`), tile granularity, restartability and group-boundary yield points.
- **Scheduling behavior.** The scheduler derives each submission's arithmetic class against a measured per-device ridge — never a declared one — builds the DAG from access sets, admits co-runners by footprint and complementarity, reserves compute (CU-mask or green context) as the precondition for co-running, paces launches, and captures short-kernel chains for replay. A persisted per-device calibration set carries the measurements.
- **Goals and constraints.** A submission's policy is one of `throughput`, `latency`, `frameBudget`, or `energy`; the scheduler optimizes the mix of resident kernels against the policy and the hardware profile (`TargetDescriptor` plus calibration).
- **Workload conformance profiles** — game rendering, multimodal ML, ML training, engineering simulation — define what a conforming scheduler must deliver for each workload shape.

> *Discussion.* The `submit` seam ships and records; the scheduler behind it is in active development under the `xpu-tile-manifest`, `xpu-tile-scheduling`, and `xpu-tile-workload-profiles` specifications (all approved 2026-09-06). Until those units land, `submit` gates nothing — the launch after it executes immediately — and the behavior described above is design, not yet guarantee. The seam is the compatibility contract: call sites written against it are unchanged when the scheduler arrives.

> *Discussion.* Backend verification status varies by feature and is tracked in the internal capability matrix; the CPU, Vulkan, and AMD columns of the core model are device-measured, with some NVIDIA paths verified at emission only. Graphics execution (raster and ray-tracing pipelines) is not part of `cajeta.xpu`; it is the `cajeta.render` layer's, outside this specification.
