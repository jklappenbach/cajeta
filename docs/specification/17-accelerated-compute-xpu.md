# 17 — Accelerated Compute (XPU)

This chapter defines the accelerator programming model, package `cajeta.xpu`.
One portable kernel source compiles for four backends — NVIDIA (NVPTX), AMD
(AMDGPU), Vulkan (SPIR-V) and the host CPU — and a single binary may bundle
several, selecting one at the first device touch. The model's central claim is
not that a kernel runs everywhere but that it runs **at each device's
ceiling** without a table of devices and without a vendor branch in the
source: the kernel declares what only its author knows, the compiler measures
the rest into a manifest, the device reports its own facts through one
surface, and a filter and a timed choice reconcile the two at bind. The
borrow checker's guarantees hold across the host/device boundary (§17.6). The
internal contract is [cajeta.xpu — one kernel, any device](xpu/CajetaXPU.md);
the reader's map and the catalogs of kernels built on it are under
[Kernels](../kernels/README.md).

## 17.1 Kernel Declarations and the Device Subset

`@Kernel` on a static method makes it a device entry point; `@Device` marks a
static helper callable from kernel code as well as the host; `@Wave(width = N)`
declares the lane count a body assumes; `@Occupancy` declares the launch
ceiling and register pins; `@FastMath` permits fused floating-point
contraction; `@Unlowered` and `@Unbounded` declare, against a tracked plan
item, a construct a backend cannot lower or a launch with no constant block.
A class template carrying `@Instantiate({T<…>, …})` and `@Requires(expr)` is
how one kernel design is written over its shape: the listed members are built
ahead of time, and a member the constraint rejects is refused by name at
instantiation.

A kernel body is ordinary Cajeta restricted to the device subset: no heap
allocation and no exceptions; locals, control flow (including labelled
`break`/`continue`), and calls to `@Device` helpers. A device function value
is a tag over a closed set of `@Device` statics and lowers to direct calls on
every backend; an out-of-range dispatch is a defined no-op. Parameters are the
marshallable kinds of §17.4.

## 17.2 Execution Geometry

A launch names a grid of workgroups and a workgroup size, in up to three
dimensions. Inside a kernel, coordinates read through class statics:
`KernelThread.x()` (thread in workgroup), `KernelThread.globalIdX()`
(global), `Workgroup.x()`/`.y()`/`.z()` and `Workgroup.dimX()`. The wave the
kernel got is `Group.width()` and `Group.laneId()`: 32 on NVIDIA, 32 or 64 on
AMD, the subgroup size on Vulkan, the host's SIMD width on the CPU backend. A
kernel that indexes lanes as `tid % Group.width()` is correct at every width;
a kernel that declares `@Wave(width = N)` is built for N lanes on every
backend (natively, by a pinned wavefront, by a virtual wave over Vulkan
subgroups, or by a work-item loop vectorised at N on the CPU), and refused by
name where the width cannot be supplied. The grid-stride loop is built in:
`for (i, T v : buf.range(n))` iterates a buffer with the canonical
global-id/grid-size stride on every backend.

## 17.3 Memory

- **`KernelBuffer<T>`** is device memory, allocated on the host
  (`heap KernelBuffer<float32>(n)`), filled and drained with
  `upload(hostArray)` / `download(hostArray)` (and their `Async` forms against
  a stream), reinterpreted with `wordView()` and `halfView()`, sliced with
  `slice`. Kernels index it, and host code does not.
- **Workgroup-shared memory** is declared in-kernel:
  `Shared<float64> tile = shared float64[512];`, static or sized at launch
  (`sharedBytes:`). `Barrier.workgroup()` synchronises the workgroup. The
  layout types `Swizzled<T, S>` and `BlockPadded<T, Block, Pad>` name a bank
  layout and degrade to the identity layout where a backend has no swizzle.
- **Atomics and `MemoryOrder`** provide device-side atomic operations; 64-bit
  integer atomics are native on CUDA, HIP and the CPU and under the Vulkan
  `Int64Atomics` capability. `AsyncCopy` overlaps global-to-shared movement
  with compute where the backend supports it.

## 17.4 Kernel Arguments

A kernel's parameters marshal by kind. `KernelBuffer<T>` marshals as a device
handle, primitives by value, and a plain class whose fields are all
primitives — a POD, no inheritance, no marker interface — by value, read-only,
field by field. `Texture2D` with `Sampler` marshals for sampled reads
(`tex.sample(s, u, v)`, explicit-LOD, with cube and mipmapped variants, the
latter emulated over layered arrays on AMD), and `@PushConstant` where the
backend has the concept. Ownership of buffer arguments is §17.6.

**Example 17.4-1.** The canonical kernel shape. Requires an XPU-enabled build
(`--xpu-backend=…`). The compute pipeline is pinned by the on-device test
suites.

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

`Tile<T, Rows, Cols>` is the portable cooperative-matrix fragment: a tile of a
matrix multiply held cooperatively by the group, device-only and
register-resident, never addressable, never a host value. The author declares
element type and shape and never a role: `Group.mac(c, a, b)` computes
`c = a·b + c`, the roles inferred from argument position. The explicit
`CooperativeMatrix<T, Rows, Cols, Use>` remains for the paths that must name
a role, with `load`, `store`, `mma`, `fromWords` and the epilogue verbs of
`TileOps`.

**Tiering.** Each cooperative verb lowers at the fastest tier the device has
for its element type and shape: the native tensor core (`WMMA` on AMD, `wmma`
on NVIDIA, the Khronos cooperative matrix on Vulkan) for the 16×16×16 f16,
bf16 and int8 shapes with f32 or int32 accumulators, or the portable software
tile, distributed across the wave's lanes where the backend permits. The tier
is chosen per group of operands and whole: a shape that straddles capability
demotes the entire GEMM rather than splitting it, the choice is reported once
through an `[mma-tiering]` compiler note, and the kernel's manifest records
each native operation's instruction and tier so that a route can read it
before launching.

## 17.6 Launch, Completion, and Ownership

`kernel.launch(stream, grid: […], block: […])(args)` enqueues on a
`KernelStream`. `stream.sync()` completes it, and `Event`s order work across
streams. The runtime selects among the backends bundled in the binary — CUDA,
then HIP, then Vulkan, then CPU, or the one `CAJETA_XPU_BACKEND` or
`Device.force` names — at the first device touch, so kernel code written to the
portable surface runs wherever the program lands. Every launch is checked, and
a refused launch throws `XpuLaunchException` with its reason (no code
registered, a misaligned buffer, an unsupported declared wave, a block that
differs from the pinned one, a missed deadline, too many threads or too much
shared memory). On CUDA a stream may be deferred (`setDeferred`): a repeated
launch sequence is recorded once as a graph and replayed with its arguments
patched in.

The borrow checker holds across the boundary: a buffer lent to a launch is
borrowed by the in-flight kernel, and the borrow resolves at `sync()` — a
program cannot drop or re-transfer a buffer an enqueued kernel still reads.
This is the deferred-borrow-until-sync rule.

## 17.7 Capabilities and the Device Surface

`Device` answers for the backend that will run the kernel. `supports(Capability)`
reports native features (hardware ray query, RT-core ray query, a launchable
bf16 cooperative matrix, native 64-bit atomics); the geometry surface reports
the driver's facts (multiprocessors and SIMDs, shared memory per block and
per multiprocessor and the opt-in ceiling, the register file, wave size, block
and grid limits, memory and L2, clocks and bus width, the load other processes
place on the device), with zero for a fact the backend does not report; the
registry reports whether a named kernel has device code here
(`kernelAvailable`), what is registered, and how often each kernel launched.
A feature absent on the selected backend is either emulated where this
specification says so or unavailable through the query, never silently wrong.

## 17.8 Adaptation: the Manifest, the Filter, the Choice

For every (kernel, backend) the compiler records a **manifest** beside the
device code, readable at run time: the identity and a hash of the code, the
footprint measured from the code object (registers, spill, shared memory, the
pinned block, resident groups, the budget that limits occupancy), the access
mode of every buffer parameter, and the tier of every cooperative operation.
A field a backend cannot report is absent, never zero. Three gates read it at
build time and fail the build by name: a kernel with no device code for a
declared backend, a kernel launched with a non-constant block and no declared
ceiling, and a kernel that spills.

At run time a **family** of candidate shapes for one problem is filtered
against the device's facts and each candidate's measured facts: any spill,
threads times registers over the register file, shared memory over the
per-block limit, or no resident workgroup prunes a candidate with its rule;
whether the grid fills the device and how much a shape pads are recorded
without pruning. An empty survivor set is a named refusal. Among the
survivors, one is taken outright; several are timed against each other once,
interleaved and by medians, and the winner is remembered in the `Autotune`
store under the family and the exact problem, against a build id of the
device, the driver and every survivor's code, so a rebuild measures again and
a stale value is discarded and reported. A measurement taken under foreign
load is used and not stored. The store is one small file per (device, name)
under a directory the application supplies, keyed by the backend and the
geometry the answer depends on, so a store copied between machines is ignored
rather than believed.

## 17.9 Kernel Scheduling

Every launch is a **submission**. The plain form reaches the runtime as a
submission at the kernel's default policy, with its read, write and
accumulate sets taken from the manifest and bound to the buffers passed. A
`KernelSubmission` passed in the stream's place adds the policy (`throughput`,
`latency`, `frameBudget`, `energy`), a deadline window, protection, a
compute-unit budget, a capture identity and the target stream; a submission
whose window has closed is rejected with its reason and never run late.
`Scheduler.submit(sub)` is the explicit form. The scheduler that derives each
submission's arithmetic class against a measured per-device ridge, admits
co-runners by footprint and reserves compute is specified under
`xpu-tile-scheduling` and `xpu-tile-workload-profiles`; today `submit`
records, the launch after it executes immediately, and the API is the
compatibility contract.

**Calibration.** `DeviceCalibration.current()` measures a device once (peaks,
bandwidth idle and under host load, bytes in flight, launch cost, event and
inter-queue cost, scheduling delay beside resident kernels, the occupancy
probe) and records a fact it cannot measure as absent. Records are keyed by
device, driver, compiler and power mode and served from the artifact's
built-in records, then the platform's user cache; nothing is written under
`/tmp` or the artifact store. `cajeta calibrate <program>` and `cajeta build
--with-calibration=<snapshot>` build a snapshot in for a fixed device.

## 17.10 Kernel Routing

Which kernel serves which data format in which regime is a declaration the
layer audits, not a predicate each package restates: a route table of rows,
one kernel variant serving one format in one regime, with a static refusal
the audit walks from any machine and a runtime refusal read at bind, both
naming the gate that refused. See [Kernel Routing](xpu/CajetaXPU-Routing.md)
and, for how to write a kernel the routing can admit,
[Writing XPU Kernels](../guide/25-xpu-kernels.md).

> *Discussion.* Backend verification status varies by feature and is tracked
> in the internal capability matrix and the per-backend kernel census tables
> of the libraries built on this package. Graphics execution (raster and
> ray-tracing pipelines) is not part of `cajeta.xpu`; it is the
> `cajeta.render` layer's, outside this specification.
