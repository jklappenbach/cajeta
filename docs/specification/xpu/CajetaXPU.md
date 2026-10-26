# cajeta.xpu — one kernel, any device

The accelerator substrate of the Cajeta language: how a kernel is declared,
how it is compiled for NVIDIA, AMD, Vulkan and the host CPU from one source,
what the compiler measures about it, what the runtime learns about the device
it lands on, and how the two are reconciled so that the kernel runs at that
device's ceiling without a table of parts and without a vendor branch in the
source. This document is the contract. The map for readers is
[One kernel, any device](../../xpu/README.md); the how-to is
[Writing XPU Kernels](../../guide/25-xpu-kernels.md); the catalogs of kernels
built on this substrate are [cajeta-llm](../../xpu/catalog-llm.md) and
[cajeta-xgboost](../../xpu/catalog-xgboost.md).

Every statement here describes what the toolchain does today (2026-10-24),
with the engineering specification that governs it named where one exists.
Design that has not landed is marked as such.

## 1. Definition

`cajeta.xpu` is a package of the standard library and a set of compiler
backends. A **kernel** is a static method marked `@Kernel`, written in the
device subset of the language. The compiler lowers it for every backend the
build declares (`--xpu-backend=nvptx,amdgpu,vulkan,cpu`), bundles the device
code with the program, and emits beside each a **manifest** of what it
measured. At run time one backend is selected at the first device touch, the
kernel's launch resolves to that backend's code, and the host queries the
device through one geometry surface.

Three questions stand between a kernel and a device it has never run on, and
the substrate answers each in its own place (xpu-kernel-adaptor §1):

| question | answered by | home |
|---|---|---|
| what does the kernel need | its declarations, and the compiler's measurement of its code | the manifest, §4 |
| what does the device offer | the driver | `Device`, §5 |
| what configuration satisfies both, and which candidate is fastest here | the feasibility filter and the measured choice | `ShapeFilter`, `ShapeChoice`, `Autotune`, §6 |

The non-goals are as important. The substrate does not sweep: a device that
can be modelled is computed, and timing is reserved for candidates that all
fit. It does not fall back silently: a candidate that does not fit is pruned
with its rule, and a problem no candidate fits is a refusal that names them.
It does not substitute a value for a fact the driver did not report: absent is
absent, and the rule that needs it does not apply. And it does not let a
kernel assume a wave width, a block, or a vendor the hardware was not told
about.

## 2. The kernel

### 2.1 Declarations

```cajeta
@Kernel
public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,
                         float32 a, uint32 n) {
    uint32 i = KernelThread.globalIdX();
    if (i < n) { y[i] = a * x[i] + y[i]; }
}
```

| attribute | meaning |
|---|---|
| `@Kernel` | a device entry point, registered under its qualified name (a class template's kernel registers once per instantiation, `Tile_128_128_32_64_128_q4kQ8`) |
| `@Device` | a static helper callable from kernels as well as from the host; lowered as an always-inlined device function per kernel that uses it |
| `@Wave(width = N)` | the lane count the body assumes (§3.2) |
| `@Occupancy(maxThreads, maxWaves, minResident, maxRegisters)` | the structural launch ceiling and the register and residency pins (§3.4) |
| `@FastMath` | fused floating-point contraction permitted in this body |
| `@Unlowered(backend, tracked, mayLower, hold, hostWaveBelow)` | a construct the named backend cannot lower, declared against the plan item that tracks it (§3.6) |
| `@Unbounded(tracked)` | a kernel launched with a non-constant block and no declared ceiling, tracked (§3.6) |
| `@Intrinsic`, `@TargetIntrinsic`, `@TargetAsm` | the escape hatches: a body-less declaration, or per-backend arms beside a portable body that the reference interpreter runs |
| `@Instantiate({T<…>, …})`, `@Requires(expr)` | on a class template: the members built ahead of time, and the constraint a member must satisfy or be refused by name (§3.5) |

The body is Cajeta restricted to the device subset: no heap allocation, no
exceptions; locals, control flow including labelled `break` and `continue`,
calls to `@Device` helpers, and the verbs below. A function-typed value in
device code is a tag over a closed set of `@Device` statics and lowers to
direct calls; an out-of-range dispatch is a defined no-op. A
`CooperativeMatrix` fragment may be passed to a helper at its exact declared
shape and at no other.

### 2.2 Parameters and memory

| kind | marshals as |
|---|---|
| `KernelBuffer<T>` | a device handle; the buffer is **borrowed by the launch until the stream's next `sync()`**, and freeing it earlier is error XPU-K02 |
| primitives | by value |
| a plain class of primitive fields | by value, read-only, field by field |
| `Texture1D/2D/3D`, `Texture2DArray`, `TextureCube`, `Sampler`, `Image2D` | the backend's sampled or writable image, emulated where the backend lacks a kind (cube and mipmapped textures on AMD over layered arrays) |

`KernelBuffer<T>` is an RAII device allocation with `upload`, `download`, their
asynchronous forms against a stream, `slice`, and the `wordView` and
`halfView` reinterpretations the kernels use to read a byte slab as 32-bit
words or halves. Workgroup memory is declared inside the kernel,
`Shared<int8> tile = shared int8[TM * 144];`, static or sized at launch
(`sharedBytes:`), with the address-space marker types `Global<T>`,
`Shared<T>`, `Constant`, `Private` and the layout types `Swizzled<T, S>` and
`BlockPadded<T, Block, Pad>`, which degrade to the identity layout where a
backend has no swizzle. `Barrier.workgroup()` synchronises a workgroup.
Atomics carry a `MemoryOrder`; 64-bit integer atomics are native on CUDA, HIP
and the CPU backend and under `Int64Atomics` on Vulkan, and `Device.supports`
answers for them.

### 2.3 Coordinates, waves and collectives

| | |
|---|---|
| `KernelThread.x()/y()/z()`, `KernelThread.globalIdX()` | the thread in its workgroup, the flat item |
| `Workgroup.x()/y()/z()`, `Workgroup.dimX()` | the workgroup and its size |
| `Group.width()`, `Group.laneId()`, `Wave.width()`, `Wave.laneId()`, `Wave.isFirstLane()` | the wave the kernel got (§3.2) |
| `Wave.reduceSumF32/I64`, `reduceMaxF32`, `prefixSumI64`, `ballotSync`, `shuffleSync*`, the segmented reductions | wave verbs, exact at every width |
| `Workgroup.reduce(GroupOp, v)`, `Workgroup.scan(GroupOp, v)` | barrier collectives across every wave of the workgroup, with no wave width or count in the source; partials combine from wave 0 upward, so the result is deterministic at a given width |
| `Vector<T, N>`, `vload<N>`, `vstore`, `asWords`, `asBytes`, `lut4` | wide loads and stores; an aligned byte-vector load lowers to a word load |
| `dotSum`, `dotAccum` | the integer dot products (`dp4a` and its equivalents) |
| `CooperativeMatrix<T, 16, 16, Use>`, `load`, `store`, `mma`, `fromWords`, `splat`, `TileOps.scaledAccumI32`, `rank1Accum`, `scaledAccumInto(2)` | the matrix fragments and their epilogues (§3.3) |
| `Tile<T, Rows, Cols>`, `Group.mac` | the role-free fragment form; roles inferred from position |

### 2.4 Launch, streams, events

```cajeta
k.launch(stream, grid: [workgroups], block: [threads], sharedBytes: [n])(args);
```

`grid` counts workgroups. `KernelStream.current()` is the per-thread default
stream; `create()` a real stream; `sync()`, `waitFor(Event)` and `destroy()`
the rest. After every launch the compiler emits `Device.checkLaunch()`, and a
refused launch throws `XpuLaunchException` with one of seven reasons: no code
registered, a misaligned buffer base, the scalar twin of a wave kernel would
have run, an unsupported declared wave width, a block that differs from the
one the kernel was pinned to, a missed deadline, too many threads or too much
shared memory.

When every launch site of a kernel passes the same constant block, the
compiler pins it and budgets registers for it (§3.4). A `KernelSubmission` in
the stream's place adds a policy, a deadline window and a budget to the
launch; `Scheduler.submit` is the explicit form. The scheduler that acts on
those is design under the tile-scheduling specifications; today `submit`
records and the launch after it executes immediately.

**Deferred streams.** On CUDA, `KernelStream.setDeferred(true)` queues a
stream's launches and flushes them at the next point where a result becomes
observable. The first sight of a launch sequence runs direct; a sequence seen
three times is recorded as a graph and replayed with its changed grids and
arguments patched into the nodes. A launch carrying a texture, an image, a
buffer array, or more than 48 arguments or 384 bytes of them goes direct.
`CAJETA_XPU_DEFER=0` never defers, `all` defers every stream from its first
launch, unset leaves it to `setDeferred`. Deferral is a no-op on the other
backends, and it is disabled while a profiler's device sink is live, because
the profiler attributes each launch at submission. `deferStat` reports queued,
replayed, direct, graphs, graph launches, patches and flushes.

### 2.5 One meaning, every backend

The **reference interpreter** runs a kernel's source on the host, sharing the
front end and none of the lowering, one work-item at a time with workgroups,
shared memory, barriers and waves modelled as rendezvous under deterministic
scheduling. It refuses by name what it cannot define (XPU-REF01) and reports
undefined behaviour it observes (XPU-REF02). Launches recorded from a run
(`CAJETA_XPU_RECORD`) replay through it (`CAJETA_XPU_CONFORMANCE=<dir>`, with
a held list and a budget): integers must agree bit for bit, floats within the
held list's ulps, and a held kernel that now agrees is STALE. Its semantics
are [ReferenceInterpreter.md](ReferenceInterpreter.md). Every backend is
judged against it; a kernel's meaning is never what one vendor's compiler
happened to produce.

## 3. Compile time: what a kernel becomes on each backend

### 3.1 Backends

One AST walk lowers every kernel through a seam of virtual methods, one
implementation per target (`LoweringTarget`), so the body, control flow,
operators and buffer addressing are shared and only the coordinate source,
address spaces, wave and matrix intrinsics fork:

| backend | triple and artifact | selected at run time as |
|---|---|---|
| `nvptx` | PTX assembled by `ptxas` to a cubin per architecture; needs an assembler newer than CUDA 12.0, which miscompiles spilling cooperative-matrix kernels silently | `cuda` |
| `amdgpu` | AMDGPU ISA linked by `lld` to a code object per `gfx` (a comma list builds a multi-architecture bundle) | `hip` |
| `vulkan` | SPIR-V, dispatched through the Vulkan loader; MoltenVK on macOS | `vulkan` |
| `cpu` | host IR: the grid becomes threads, the body a vectorised work-item loop (§3.7) | `cpu` |

NVPTX, AMDGPU and SPIR-V are in-tree targets of the cajeta-llvm fork
(LLVM 23); the toolchain does not depend on the out-of-tree SPIR-V
translator. `CAJETA_GPU_<FEATURE>_IMPL=software|native` forces a feature's
tier for a sweep.

### 3.2 Wave width

`Group.width()` is the lane count of the wave the kernel got, folded to a
constant per target: 32 on NVIDIA; 32 or 64 on AMD, as the kernel's target
features pin; the subgroup size on Vulkan; the host's SIMD width on the CPU
backend, or the declared wave. A kernel that indexes lanes as
`tid % Group.width()` is correct at every width, and that is the recommended
form.

`@Wave(width = N)` declares a width the body assumes, and each backend builds
the kernel for it: NVIDIA accepts 32 only; AMD pins `+wavefrontsize32` or
`+wavefrontsize64`; the CPU backend runs the work-item loop at N lanes
regardless of the host's vector width; Vulkan folds a declared W onto
subgroups of size S as a **virtual wave** of W/S slots per invocation, or a
narrow mode when S exceeds W, and requests a required subgroup size where the
driver offers one (vulkan-virtual-waves). A kernel whose built width differs
from its declaration is refused at build. `Group.laneBlock()` reads the
device's wave at run time and `Group.laneBlockOf(kernel)` the width a kernel
was built at, which differ when a distributed software tile forced 32.

### 3.3 Cooperative matrices and tiers

A cooperative-matrix verb has more than one lowering, and the compiler picks
one per operand group:

| backend | Native | Portable |
|---|---|---|
| nvptx | 16×16×16 f16, bf16 and int8 operands with f32 or int32 accumulators (`wmma`) | everything else |
| amdgpu | the same table (`WMMA`, wave32) | everything else, distributed across the wave |
| vulkan | f16, f32 and integer under `SPV_KHR_cooperative_matrix`; bf16 Portable | everything else |
| cpu | none | all |

The **portable software tile** is either replicated (the whole tile per
work-item, which lands in scratch: the standard library's f32 and f64 tiles
spilled kilobytes a work-item replicated and none distributed) or
**distributed** (each lane holds its 1/W share; the default on AMD and NVPTX,
`DIST=off` the replicated control). The tier is chosen for a group of operands
as a whole: a GEMM whose operands straddle tiers demotes entirely rather than
splitting (amdgpu-coopmatrix-tier-straddle), and each distinct (type, shape,
backend) reports once through an `[mma-tiering]` note. The manifest records
every native operation's instruction and tier (§4), so a route can learn a
kernel is on the slow tier before it launches it.

`fromWords` builds a B fragment from four 32-bit words a lane already holds.
On AMD those words are the lane's fragment. NVIDIA's int8 fragment is a
different partition of the same 256 bytes, so the lowering stages the words
through a per-wave shared buffer in the layout the hardware's load expects
and reloads them with `wmma`, once per fragment, for the 16×16 shape only,
sized from the kernel's `@Occupancy(maxThreads)`, without which the kernel is
refused (XPU-N04). The kernel source stays one; the cost is confined to the
backend with no cheaper form, and it is a cost a kernel on a hot path
measures before accepting.

### 3.4 Occupancy: the block the kernel is budgeted for

A backend budgets a kernel's registers for the largest block it may be
launched with. Without a bound AMDGPU assumes the part's ceiling (1024
threads, the smallest register allowance a thread can get) and NVPTX applies
no bound. So the compiler scans every launch site for a constant block and
pins the largest, and a kernel whose block is not constant at every site must
declare its ceiling or fail the build (`[xpu-kernel-unbounded]`).

`@Occupancy(maxThreads = N)` is that ceiling, a structural fact of the
kernel's design. `maxWaves = N` is the same bound in waves, for a kernel
written as N waves whose thread count follows the wave width (256 threads at
wave 32, 512 at wave 64); declaring both is refused. `minResident` and
`maxRegisters` are explicit pins. Per backend: NVPTX lowers all four
(`maxntid`, `minctasm`, `maxnreg`); AMD lowers the block and residency
(`amdgpu-flat-work-group-size`, `waves-per-eu`) and has no register pin, and
finishes `maxWaves` once the wave is known; Vulkan's workgroup size is a
specialisation constant and the annotation is a no-op there. In a class
template an argument may be an expression over the template's values, so the
bound travels with the shape: `@Occupancy(maxWaves = (TM / WM) * (TN / WN))`.

### 3.5 Shapes as templates

A kernel design is written once over its shape as a class template, and the
members are built ahead of time:

```cajeta
@Instantiate({QkTile<64, 64, 32, 32, 128>, QkTile<128, 128, 32, 64, 128>})
@Requires(TM == TN && TM % WM == 0 && TN % WN == 0 && WM % 16 == 0
    && WN % 16 == 0 && (TM / WM) * (TN / WN) * 16 == TN && KS == 128)
public final class QkTile<uint32 TM, uint32 TN, uint32 WM, uint32 WN, uint32 KS>
        implements Q4TileShape { ... }
```

`@Instantiate` lists the instantiations (default cap eight, raised with
`max`), each registering its kernel under a name carrying its arguments.
`@Requires` is evaluated over the non-type parameters at instantiation, and a
member that fails it is refused by name with the failing clause
(`CAJETA_ERROR_TEMPLATE_REQUIREMENT`), never miscompiled. The family object
that lists the members and chooses among them is the library's (§6.2);
the specification is xpu-tile-shape-selection and the template mechanics are
[ExplicitInstantiation](../lang/templates/ExplicitInstantiation.md).

### 3.6 The build gates

A build fails by name, under one error (`CAJETA_ERROR_XPU_KERNEL_GATE`), after
every offending kernel has been listed, for:

- `[xpu-kernel-skipped]`: a `@Kernel` with no device code for a backend the
  build declares, with the lowering's reason. A launch would find nothing.
- `[xpu-kernel-unbounded]`: a non-constant block with no declared ceiling.
- `[xpu-kernel-spill]`: a kernel with scratch on NVPTX or AMDGPU. The message
  distinguishes a portable software tile (the scratch is the tile; use a
  natively tiered type), zero spill stores (a construct legalised through
  memory; fix the lowering), and register pressure (cut live registers or pin
  a smaller block).
- `[xpu-kernel-hung]`: a CPU vectorisation that exceeded its deadline
  (`CAJETA_XPU_CPU_VECTORIZE_DEADLINE_S`, 600 s).

Each has one escape, a declaration naming the plan item that tracks the
condition, and the line becomes a note tagged `[tracked: <item>]`.
`@Unlowered(backend = "cpu", tracked = "...")` is per backend; a held kernel
that now lowers is STALE and fails until the declaration goes. `mayLower =
true` makes the hold per instantiation of a template (one member needs 73 KB
of shared memory and is declined; another fits), at the price of stale
detection. `hold = true` skips the attempt before lowering. `hostWaveBelow =
N` holds on the CPU backend only on a host narrower than N lanes.
`@Unbounded(tracked)` tracks the derived block. A box without the assembler is
not the kernel's failure: the line stays a note saying `no assembler`.
`CAJETA_XPU_KERNEL_GATE=warn` demotes the gates for a sweep.

### 3.7 The CPU backend

The host is a target, not an emulator. A kernel's body is wrapped in a
per-block work-item loop and handed to LLVM's loop vectoriser at the host's
SIMD width (16 lanes on AVX-512, 8 on AVX2), or at the declared `@Wave`
width. Wave verbs lower to scalar calls carrying vector-function-ABI variants
that the vectoriser substitutes, so a wave is a vector: reductions become
vector reductions, ballots a mask, shuffles a gather; a divergent block
takes the masked variant, so active-lane semantics hold. A workgroup barrier
is honoured by fission of the loop into regions, with uniform loops
scaffolded so the work-item loop stays innermost. Shared memory is a
per-block array; blocks fan out across pthread workers (capped by
`CAJETA_XPU_CPU_WORKERS` or `Device.setCpuWorkerCap`). A kernel whose loop the
vectoriser would leave scalar is refused at launch rather than run slowly
(reason 3 of §2.4), and the manifest records the width it was built at, or 1
for a kernel with no wave verb. The device-geometry surface on this backend
describes the host: cores as multiprocessors, the SIMD width as the wave,
physical memory, integrated. The backend reports no register or spill count,
so every feasibility rule that needs one passes (§6.1).

## 4. The kernel manifest

For every (kernel, backend) the compiler records a manifest beside the device
code, embedded in the artifact and registered at load, readable as
`KernelManifest.of(name)` for the active backend (xpu-tile-manifest):

| group | fields |
|---|---|
| identity | kernel, target (`nvptx/sm_89`, `cpu/<host>`), `codeHash` (SHA-256 of the shipped device code), compiler version, XPU ABI version, schema version |
| footprint, from the code object | `waveWidth` (the lane block), `vgpr`, `sgpr`, `spillBytes`, `spillStoreBytes`, `ldsStaticBytes`, `ldsDynamicParam`, `threadsPerGroup` (the pinned block), `residentGroupsPerCu`, `feasibleBlocks`, `occupancyLimiter` |
| access | per buffer parameter: read, write, accumulate, streaming; the derived `restartable`, `drainsDevice`, `captureSafe` |
| native operations | per cooperative verb: the instruction chosen and its tier (`wmma…`, or `software-tile.replicated` / `.distributed`) |

Absent is a value: a field a backend cannot report is an empty `Optional`,
never zero, and `KernelManifest.of` is `null` where the backend has no device
code for the kernel. On NVPTX the footprint comes from `ptxas -v`; on AMDGPU
from the code object's kernel descriptor. The manifest is what `KernelFacts`
reads (§6.1), what `Group.laneBlockOf` reads, and what the spill gate judges.

## 5. Run time: what the device reports

### 5.1 Selection

One backend is selected at the first device touch, in the order CUDA, HIP,
Vulkan, CPU among those the binary bundles, restricted by `CAJETA_XPU_BACKEND`
or by `Device.force(name)` called before that touch. A forced backend that is
unavailable degrades like the environment route, with one note. Selection is
per process and cached; `Device.activeBackend()` names what was got, and is
itself a touch.

### 5.2 The geometry surface

`Device` answers for the backend that will run the kernel, reading the
driver rather than a table of parts. A zero is a fact the backend does not
report.

| fact | surface |
|---|---|
| multiprocessors (SMs; WGPs on RDNA), schedulers per multiprocessor, SIMDs in all | `multiprocessorCount()`, `simdsPerMultiprocessor()`, `simdCount()` |
| blocks that fill the device at a waves-per-block | `dispatchBlocks(wavesPerBlock)`, from an empirical waves-per-SIMD target (1 on NVIDIA, 2 on AMD); a floor and the start of a sweep, not its answer |
| shared memory a block may use, the raised opt-in ceiling, the per-multiprocessor total | `sharedBytesPerBlock()`, `sharedBytesPerBlockOptIn()`, `sharedBytesPerMultiprocessor()` |
| registers, threads and blocks a multiprocessor holds | `registersPerMultiprocessor()`, `threadsPerMultiprocessor()`, `maxBlocksPerMultiprocessor()` |
| wave size, block and grid limits | `waveSize()`, `maxThreadsPerBlock()`, `maxBlockDimX()`, `maxGridDimX()` |
| memory, L2, discrete or integrated | `memoryBytes()`, `freeMemoryBytes()` (live), `vramBytes()`, `l2CacheBytes()`, `integrated()` |
| clocks and bandwidth | `clockKHz()`, `memoryClockKHz()`, `memoryBusWidthBits()`, `peakBandwidthGBps()` |
| identity and neighbours | `uuid()`, `driverVersion()`, `foreignLoadPercent()` |
| capabilities | `supports(Capability)`: `RayQueryNative`, `RayQueryRtCore`, `CoopMatrixBf16F32Acc`, `AtomicInt64` |
| the registry | `kernelAvailable(name)`, `registeredKernelCount()`, `registeredKernel(i)`, `kernelLaunchCount(name)`, `launchFailures()`, `checkLaunch()` |

The per-block shared limit is the load-bearing one: an RTX 4090 holds 100 KB
a multiprocessor and caps a block at 48 KB unless the launch opts in to
99 KB, while on RDNA the two are equal; a tile sized against the wrong figure
assembles on one part and fails on the other. Size tiles against
`sharedBytesPerBlock()`, and against the opt-in ceiling only where the launch
asks for it.

`kernelAvailable(name)` is the registry question: device code for this kernel
on the active backend, with a manifest whose shared memory fits this device.
A route asks it before considering a candidate, and nothing falls through to a
kernel it denies. The remaining capability traits that older versions of this
document described as `xpu.cap.*` are empty markers nothing reads; the
capability surface is `Device.supports` and the registry.

### 5.3 Timing

`KernelTimer` brackets a stream between two events and answers nanoseconds at
a **tier**: device events on CUDA and HIP, the synchronous host clock on the
CPU backend, unavailable on Vulkan outside the profiler's timestamp seam
(it answers -1 and the caller times on the host). On first use it calibrates
the event clock's scale against the host clock over busy copies (the CUDA
event clock runs 4.6 percent fast on WSL2) and refuses a scale outside
[0.5, 2]. `impliedGBps`, `impliedOpsPerSecond` and `withinPeak` turn a
reading into a figure against the device's roofline.

### 5.4 Profiling

`CAJETA_PROFILER=1` arms the host sampler and the device capture in one
window before any backend initialises. Every device span carries the tier
that produced it: DEVICE from the vendor's dispatch records (CUPTI on CUDA,
rocprofiler on HIP), EVENT from device-event brackets (CUDA with only the
driver installed), HOST from submit-to-complete; on Vulkan, timestamp writes
inside the command buffer around each profiled dispatch, with the period
refined by a drift fit, degrading to host windows where the queue family has
no valid timestamp bits. Records flow through lock-free per-sink rings that
never block the launch path; a long run's pending table grows rather than
dropping launches, and the record's `gpu_pending_overflow`, `unclaimed` and
`records_dropped` counters say when a capture is incomplete. An armed capture
keeps every launch direct (§2.4). The specification is cajeta-profiler.

## 6. The reconciliation

### 6.1 The feasibility filter

A **family** is a list of candidate shapes of one kernel design. At a problem
`(M, N, K)` the family builds a `ShapeFilter` over `DeviceFacts.current()`
(registers per multiprocessor, shared per block with and without opt-in,
shared per multiprocessor, threads and blocks per multiprocessor,
multiprocessor count, threads per block) and considers each
`ShapeCandidate(kernel, tileM, tileN, threads, kBlock, maxSplits)` with its
`KernelFacts.measured(kernel, dynamicSharedBytes)`: registers a thread, spill
bytes, and static plus dynamic shared bytes from the manifest, each -1 when
absent.

| rule | prunes when | note |
|---|---|---|
| R1 | the kernel spills | any spill |
| R2 | threads × registers exceed the register file | |
| R3 | shared bytes exceed the per-block limit | the opt-in ceiling where present |
| R4 | no workgroup is resident on a multiprocessor | |
| R5 | never | records whether the grid fills the multiprocessors, or the split-K count that would |
| R6 | never | records the padding waste at this problem, per mille |

Every pruned candidate is kept with its rule, its need and the limit; a fact
the backend does not report prunes nothing; an empty survivor set is a
`refusal()` that names the family, the problem and every verdict, thrown as
`XpuLaunchException` by the family, never silently replaced.

### 6.2 The measured choice

`ShapeChoice.choose(dir, filter, probe)` decides among survivors. An override
set by `setShapeOverride(family, kernel)` wins (a sweep's lever). No survivor
refuses. One survivor is picked outright. Several are first looked up in the
store under `shape.<family>.<M>x<N>x<K>` against a **build id** of the device
UUID, the driver version and every survivor's code hash; a hit is taken with
its recorded margin (`.margin`, per mille; under 30 is a tie). A miss times
every survivor through the family's `ShapeProbe` at the `KernelTimer` tier,
five interleaved rounds, and the fastest median wins; the winner and its
margin are remembered unless another process held 25 percent or more of the
device while it was timed (`Device.foreignLoadPercent`, read once per five
seconds), in which case the result is used and not stored, with a note. The
result (`ShapeResult`) says which kernel, whether it came from the cache, a
timing or an override, the margin, the tie, and the refusal if any.

`prewarm` runs the same at load so that a model's first real launch is the
settled choice. A family whose probe is the running program's own launches,
bracketed in place, holds its trial through the warm-up and spends the first
uses of a shape on the trial's samples; such a program's throughput is read
only after the store has settled.

### 6.3 The store

`Autotune` is one small file per (device key, name) under a directory the
caller supplies (`<dir>/<deviceKey>/<name>`), with an in-memory memo so that a
box without a writable store tunes once per execution. The **device key** is
the backend plus the geometry the answer depends on (multiprocessors × SIMDs,
wave, registers, shared memory, threads); a part that reports nothing is
keyed `unknown` and shares nothing. `remember`/`recall` are the plain forms;
`rememberFor`/`recallFor` carry a build id, and a value recalled against a
different build is discarded and reported (XPU-T02). `invalidate` drops an
entry; `reportUnderperforming` records a tuned value that measured below its
promise (XPU-T01); `claimOnce` serialises a one-time probe across threads;
`persists()` says whether this execution's values are surviving. `cajeta tune
--reset <dir>` clears a store. No per-user directory is assumed: an
application knows where its cache belongs, and libraries resolve one from
their own option or environment.

The separate **calibration store** (`DeviceCalibration.current()`,
`CalibrationStore`) holds device-wide measured facts (peaks, bandwidth idle
and under host load, bytes in flight, launch cost, event and inter-queue
cost) keyed by device, driver, compiler and power mode, served from records
built into the artifact and then the platform cache directory, never `/tmp`;
`cajeta calibrate` and `--with-calibration` build a snapshot in for a fixed
device.

### 6.4 Routing

Which kernel serves which data format in which regime is a declaration the
layer audits, not a predicate each package restates: a `RouteTable` of rows,
each one kernel variant serving one format in one regime, with a static
`shapeRefusal` the audit walks and a runtime `readyRefusal` read at bind, both
answering with the gate that refused rather than a boolean. The audit names,
from any machine and with no model loaded, every row a new format lacks.
[CajetaXPU-Routing.md](CajetaXPU-Routing.md) is the contract.

### 6.5 What the compiler configures without being asked

Two configuration decisions need no library code. The compiler pins each
kernel's launch block from its constant launch sites (§3.4), so registers are
budgeted for the real occupancy. And `DeviceProfile`, the compile-time model,
fills `feasibleBlocks` and `occupancyLimiter` into the manifest from the
kernel's measured footprint where its architecture table knows the part; on a
part it does not know those fields are absent, which is why the run-time
filter above reads the driver instead. Applying a run-time-chosen geometry to
a kernel with a tunable block is design (xpu-kernel-adaptor §5, §6); today
the kernels that adapt their geometry do so as families (§6.1, §6.2).

## 7. Discipline

**Measure in brackets, interleaved, by medians.** A device-timing bracket
near the launch floor (6 to 13 us a launch on WSL2) reads up to twelve
percent of noise. A kernel-level choice uses twenty-launch brackets, a warm
round, fifteen interleaved rounds and medians, and a gap is believed only
after the generated code (PTX, then SASS) is diffed.

**Poison fresh device memory.** `CAJETA_XPU_POISON=1`, the test suites'
default, fills every fresh device buffer with 0xFF so that a kernel reading a
region nothing wrote answers NaN deterministically, where a zero page let it
pass and a recycled chunk made a test red only after some unrelated test had
run. An order-dependent red is an unwritten read.

**Census.** A library's test run on a backend enumerates every registered
kernel (`Device.registeredKernel`) and requires each to have been
value-checked, or excused by name against a plan item; a kernel that ran is
not thereby covered. The committed tables per backend are the record of what
each has proven.

**A route change is gated by the answer, not the speed.** An inference
library's route change passes only when its greedy walk agrees with the
reference implementation token for token; a batched throughput figure is read
only beside the tokens it produced. A fast route that is wrong is
indistinguishable from a win in every other instrument.

## 8. Lineage

The v1 design of this package (2026-07) specified the three native backends
under one namespace, the dlopen-on-demand runtime library, graph capture and
the vendor FFI; those remain, and the contract above supersedes its
descriptions of capability traits, build configuration and device detection.
The engineering specifications behind the present behaviour: route-table
(closed 2026-09-18), xpu-kernel-independence (`@Instantiate`, `@Requires`,
the interpreter, the escape hatches), xpu-kernel-adaptor (the three
questions, the declaration vocabulary, the gate), xpu-tile-manifest (§4),
xpu-tile-shape-selection (§6.1, §6.2), vulkan-virtual-waves (§3.2),
amdgpu-coopmatrix-tier-straddle (§3.3), cajeta-profiler (§5.4). Feature notes
beside this document: [the capability matrix](CajetaXPU-Matrix.md) and the
[CPU bring-up log](CajetaCPU.md) (historical; the current CPU model is §3.7),
[the launch FFI](CajetaXPU-FFI.md), the wave verbs
([WaveReductions](WaveReductions.md), [WavePrefixScan](WavePrefixScan.md),
[SubgroupRotate](SubgroupRotate.md), [QuadControl](QuadControl.md)), the
atomics ([IntegerAtomics](IntegerAtomics.md), [FloatAtomics](FloatAtomics.md)),
[ShaderClock](ShaderClock.md), and the device foundation
[CajetaGPU](../gpu/CajetaGPU.md).
