# One kernel, any device

A kernel in Cajeta is written once, in the same language as the program that
launches it, and runs on NVIDIA through NVPTX, on AMD through AMDGPU, on any
Vulkan device through SPIR-V, and on the host CPU. That much is a compiler
feature. What this section documents is the harder half: how a kernel
written against one part **finds its shape on a part nobody has measured**,
without a table of devices, without a vendor branch in the source, and
without silently running slower or wrong.

## The stack

This tab is the compute stack of the language, bottom-up, and its sections
read the same way:

| layer | package or library | what it owns |
|---|---|---|
| **XPU** (this page and its neighbours) | `cajeta.xpu` | the kernel model and the device; the manifest, the adaptor, the [scheduler](scheduler.md), the filter, the measured choice and the store ([optimization](optimization.md)); the kernel catalogs of the two libraries built directly on it |
| [**Math**](math/README.md) | `cajeta.math` | `Tensor`, dtypes, linear algebra, FFT, `.npy` |
| [**Nucleo**](nucleo/README.md) | `cajeta.nucleo` | columns and tables, the autograd tape, `Fuse`, `Grad`, `Vmap`, `Jit` |
| [**ML**](ml/README.md) | `dev.cajeta.ml`, `dev.cajeta.xgboost` | estimators, layers, trainers, boosted trees |
| [**LLM**](llm/README.md) | `dev.cajeta.llm` | the decoder-only engine: load, prefill, decode, sampling, serving |

The kernels of the LLM engine and of the boosted trees are `@Kernel` code
against `cajeta.xpu`; nucleo's tensors and fusion sit on the same layer, and
the LLM engine uses `cajeta.math.Tensor` and `cajeta.xpu` directly without
nucleo. To see every layer at work on one concrete model, from the first
instruction to the last token, read [One model, start to last token](walkthrough.md).

The technology has three layers, and each answers one question.

| question | answered by | when |
|---|---|---|
| What does the kernel need? | the kernel's declarations and the compiler's measurement of its code | at compile time, into the kernel manifest |
| What does the device offer? | the driver, through the geometry surface | at the first device touch |
| Which configuration satisfies both, and which candidate is fastest here? | the feasibility filter, then a timed choice remembered per device | at bind, at first use of a shape, at load |

Nothing in the third layer runs an open search. Where a device can be
modelled it is computed; where two candidates both fit, they are timed
against each other once and the answer is remembered against the build that
produced it; where no candidate fits, the result is a refusal that names the
rule, never a fallback nobody asked for.

The pages in this section:

- **This page**: the model, the three layers, and the gates that keep them
  honest.
- [Language models on the device](kernels-llm.md): the 265 kernels of
  `dev.cajeta.llm`, how they serve decode, prefill, batched decode,
  mixture-of-experts and attention, how a kernel is chosen for a weight
  format and a batch shape, what is measured at load and per problem, and
  where the library stands against llama.cpp.
- [Gradient boosting on the device](kernels-xgboost.md): the 33 kernels of
  `dev.cajeta.xgboost`, how bit-identical results on every device constrain
  them, and the one adaptation they make.
- The engine those kernels serve is its own section,
  [Language models](llm/README.md): loading, the API, the scheduler,
  serving with cabra, and the measured standing.
- The complete catalogs, one row per registered kernel with its source, its
  reference counterpart and its census standing on each backend:
  [every cajeta-llm kernel](catalog-llm.md) and
  [every cajeta-xgboost kernel](catalog-xgboost.md).

The formal contract is the specification,
[cajeta.xpu](../specification/xpu/CajetaXPU.md); the how-to is the guide,
[Writing XPU Kernels](../guide/25-xpu-kernels.md). This page is the map.

## 1. The kernel

A kernel is a static method marked `@Kernel`. Its body is Cajeta restricted
to the device subset: no heap, no exceptions, calls to `@Device` helpers.
It reads its coordinates through classes rather than intrinsics:

```cajeta
@Kernel
public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,
                         float32 a, uint32 n) {
    uint32 i = KernelThread.globalIdX();
    if (i < n) { y[i] = a * x[i] + y[i]; }
}

saxpy.launch(stream, grid: [blocks], block: [256])(y, x, 3.0f, n);
```

`KernelBuffer<T>` is device memory the host fills and drains; `Shared<T>`
is workgroup memory declared inside the kernel; `Barrier.workgroup()`
synchronises a workgroup. The wave verbs (`Wave.reduceSumF32`,
`Wave.prefixSumI64`, `Wave.ballotSync`, `Wave.shuffleSync`), the
workgroup verbs (`Workgroup.reduce`, `Workgroup.scan`), the integer dot
products (`dotSum`, `dotAccum`) and the cooperative matrix fragments
(`CooperativeMatrix<T, 16, 16, Use>`, `TileOps`) are the vocabulary the
kernels in [llm.md](kernels-llm.md) and [xgboost.md](kernels-xgboost.md) are written in.

Three things about this surface make adaptation possible rather than merely
hoped for.

**The wave width is a query, not a constant.** `Group.width()` is the lane
count of the wave the kernel got: 32 on NVIDIA, 32 or 64 on AMD, the
subgroup size on Vulkan, the host's SIMD width on the CPU backend (16 on
AVX-512, 8 on AVX2). A kernel that indexes lanes as `tid % Group.width()`
is correct at every width. A kernel whose arithmetic needs a particular width
declares it, `@Wave(width = 32)`, and every backend builds it for that width:
NVIDIA natively (and refuses any other declared width), AMD by pinning the
wavefront size as a target feature, the CPU backend by running the work-item
loop at the declared lane count regardless of the host's SIMD width, and
Vulkan by a **virtual wave**, folding a declared width W onto subgroups of
size S as W/S slots per invocation (or a narrow mode when S exceeds W),
requesting a required subgroup size where the driver offers one. A kernel
whose built width does not match its declaration is refused by name at build,
and a launch on a device that cannot supply the width is refused with that
reason. Neither form contains the number 32 as an assumption the hardware is
not told about.

**Every backend is checked against one answer.** The kernel reference
interpreter runs a kernel's source on the host, sharing the front end and
none of the lowering, and refuses by name what it cannot define. A backend
that lowers a verb wrongly disagrees with it, and the disagreement is a test
failure, not a quietly wrong model.

**A launch is a submission, and it is checked.** The kernel's manifest
supplies what the launch reads and writes. After every launch the compiler
emits a check, and a refused launch throws with one of seven named reasons:
no code registered, a misaligned buffer, the scalar twin of a wave kernel
would have run, an unsupported declared wave width, a block that differs from
the one the kernel was pinned to, a missed deadline, too many threads or too
much shared memory. On CUDA the runtime can defer a stream's launches
(`KernelStream.setDeferred`): the first sight of a launch sequence runs
direct, repeats are recorded as a graph and replayed with changed grids and
arguments patched in, which is how a decode step of 1,360 launches is issued
in about a millisecond. Deferral is a no-op on the other backends, and an
armed profiler keeps every launch direct because it attributes each one at
submission. Launch sequences can also be recorded (`CAJETA_XPU_RECORD`) and
replayed through the reference interpreter (`CAJETA_XPU_CONFORMANCE`), which
is how a backend's conformance corpus is kept: integers bit-exact, floats
within a held list's ulps, a held kernel that now agrees reported as stale.

## 2. What the kernel declares, and what the compiler measures

Adaptation starts from facts about the kernel, half declared by its author
and half measured from its compiled code.

### 2.1 Declarations

| annotation | what it tells the toolchain |
|---|---|
| `@Occupancy(maxThreads = N)` | the kernel's structural launch ceiling, so the backend budgets registers for the true block rather than a worst-case 1024 threads |
| `@Occupancy(maxWaves = N)` | the same bound in waves, for a kernel written as N waves whose thread count follows the wave width (256 threads at wave 32, 512 at wave 64) |
| `@Occupancy(maxRegisters = N)`, `minResident = N` | explicit register and residency pins; a spilling kernel's remedy (NVPTX honours all four; AMD has no register pin; Vulkan's workgroup size is a specialisation constant and the annotation is a no-op there) |
| `@Wave(width = N)` | the lane count the body assumes |
| `@FastMath` | fused floating-point contraction permitted |
| `@Device` | a helper callable from kernels; a `Vector<int8>` through a `@Device` parameter dots as unsigned today, so signed dots stay on locals |
| `@Unlowered(backend, tracked, mayLower, hold, hostWaveBelow)` | a construct this backend cannot lower, declared against the plan item that tracks it; the gate below reads an unneeded declaration as STALE |
| `@Unbounded(tracked)` | a kernel launched with a non-constant block and no declared ceiling, tracked |
| `@Instantiate({T<…>, T<…>})`, `@Requires(expr)` | on a class template: the shapes built ahead of time, and the constraint a shape must satisfy or be refused by name at instantiation |

Class templates are how a kernel design is written once over its shape.
`QkTile<TM, TN, WM, WN, KS>` is one body; `@Instantiate` lists the members
(128×128 at 256 threads, 64×64 at 128), `@Requires` pins what the body
assumes (`TM == TN`, the warp grid's lane count equals the block, the K-run
is a half block), and an `@Occupancy` argument may be an expression over the
template's values so the bound travels with the shape. A member the body
cannot serve does not compile wrongly; it is refused with its name and the
failing clause.

### 2.2 The manifest

For every (kernel, backend) the compiler emits a manifest beside the device
code, readable at run time as `KernelManifest.of(name)`: the identity (name,
target, a hash of the device code, the compiler), the **footprint** measured
from the code object (registers a thread, spill bytes, static shared bytes,
threads a group, resident groups a multiprocessor, the budget that limits
occupancy, the feasible blocks), the **access modes** of every buffer
parameter (read, write, accumulate, streaming), and the **native operations**
the body lowered to, each with its instruction and its tier. A field a
backend cannot report is absent, never zero: the CPU backend has no register
count, and a rule that needs one prunes nothing there.

Two gates read the manifest at build time and fail the build by name rather
than print and pass. `[xpu-kernel-spill]`: a kernel with scratch is not a
tuned kernel, and the message says whether the cause is register pressure (cut
live registers or pin a smaller block) or a construct legalised through
memory with zero spill stores (fix the lowering). `[xpu-kernel-skipped]`: a
kernel that produced no device code for a backend the build declares, with
the lowering's reason, unless an `@Unlowered` declaration tracks it.
`CAJETA_XPU_KERNEL_GATE=warn` demotes both for a sweep.

### 2.3 Tiers

A cooperative-matrix multiply has more than one lowering. Where the silicon
has the shape natively (f16, bf16, int8 operands with f32 or int32
accumulators at 16×16×16 on sm_89 and RDNA3), the fragment is the hardware's
and the multiply is the tensor-core instruction. Elsewhere the tile is a
**portable software tile**: on AMD distributed across the wave's lanes
(replicated, the stdlib's f32 and f64 tiles spilled kilobytes a work-item;
distributed they spill none), on other backends held per work-item. The
tier is chosen per group of operands as a whole: a GEMM whose operands
straddle tiers demotes entirely rather than splitting, and the choice is
reported once through an `[mma-tiering]` note. The manifest records each
native operation's tier, which is how a route learns a kernel is on the slow
tier before it launches it.

Some verbs exist because one vendor's fragment layout differs from
another's. `CooperativeMatrix.fromWords` hands over four 32-bit words that
on AMD are a lane's B fragment outright; NVIDIA's int8 fragment is a
different partition of the same 256 bytes, so the compiler stages the words
through shared memory in the layout the hardware's load expects and reloads
them, once per fragment, sized from the kernel's declared block. The kernel
source is one; the cost is confined to the backend that has no cheaper form,
and it is a cost (the [llm catalog](kernels-llm.md) records a kernel that paid it at
every fragment and was replaced by one that stages whole tiles instead).

### 2.4 The CPU backend

The CPU backend is not a fallback interpreter; it is a target. A kernel's
body becomes a per-block work-item loop handed to LLVM's loop vectoriser at
the host's SIMD width, the wave verbs become vector-function variants the
vectoriser substitutes, and workgroup barriers are honoured by fission of the
loop into regions. `Group.width()` reads the host's lanes, or the declared
wave for a `@Wave` kernel, whose loop is versioned to that width. Shared
memory is a per-block array; blocks fan out across cores. A kernel the
vectoriser cannot carry within its deadline is declined with its name, and
the hold `@Unlowered(backend = "cpu", hold = true)` keeps a known one from
being tried. Because the CPU backend answers absent for registers and spills,
the feasibility filter prunes nothing there and every listed shape is a
candidate; what differs is which ones are fast.

## 3. What the device reports

`Device` answers for the backend that will run the kernel, which is what
`Device.activeBackend()` names (`cuda`, `hip`, `vulkan`, `cpu`), selected at
the first device touch in that order of preference or by `CAJETA_XPU_BACKEND`,
and forced in-process by `Device.force(name)` before that touch. The geometry
surface reads the driver, never a table of parts:

| fact | surface |
|---|---|
| multiprocessors, schedulers per multiprocessor, SIMDs in all | `multiprocessorCount()`, `simdsPerMultiprocessor()`, `simdCount()` |
| how many blocks fill the device at a given waves-per-block | `dispatchBlocks(wavesPerBlock)`, from a measured waves-per-SIMD target |
| shared memory a block may use, the raised opt-in ceiling, the per-multiprocessor total | `sharedBytesPerBlock()`, `sharedBytesPerBlockOptIn()`, `sharedBytesPerMultiprocessor()` |
| registers and threads a multiprocessor holds, blocks a multiprocessor holds | `registersPerMultiprocessor()`, `threadsPerMultiprocessor()`, `maxBlocksPerMultiprocessor()` |
| wave size, block and grid limits | `waveSize()`, `maxThreadsPerBlock()`, `maxBlockDimX()`, `maxGridDimX()` |
| memory, L2, integrated or discrete | `memoryBytes()`, `freeMemoryBytes()` (live), `vramBytes()`, `l2CacheBytes()`, `integrated()` |
| clocks, bus width, the datasheet bandwidth | `clockKHz()`, `memoryClockKHz()`, `memoryBusWidthBits()`, `peakBandwidthGBps()` |
| what other processes are doing | `foreignLoadPercent()` |
| features | `supports(Capability)`: native ray query, RT-core ray query, a launchable bf16 cooperative matrix, native 64-bit atomics |
| the kernel registry | `kernelAvailable(name)`, `registeredKernel(i)`, `kernelLaunchCount(name)`, `launchFailures()` |

A zero is a fact the backend does not report. The per-block shared limit is
the load-bearing one: an RTX 4090's multiprocessor holds 100 KB but caps a
block at 48 KB unless the launch opts in, while on RDNA the two are equal, so
a 54 KB tile sized against the per-multiprocessor figure assembles on one and
fails on the other. Size tiles against `sharedBytesPerBlock()`.

`Device.kernelAvailable(name)` is the registry question: does the active
backend hold device code for this kernel, and does its shared memory fit this
device. A route asks it before considering a candidate, and a kernel it
denies is not a candidate; nothing falls through to it. Libraries wrap it
with their own defect registry (a kernel known to lower wrongly on one
backend, listed by name against the plan item that tracks it) so that a
kernel the compiler produced but the tests condemned is also not a candidate.

## 4. The reconciliation

### 4.1 At bind: the feasibility filter

Given a family of candidate shapes for one problem, `ShapeFilter` checks
each candidate's `KernelFacts` (registers a thread, spill bytes, shared
bytes, from the manifest plus the launch's dynamic shared request) against
`DeviceFacts.current()`, at the problem's exact `(M, N, K)`:

| rule | prunes when |
|---|---|
| R1 | the kernel spills |
| R2 | threads × registers exceed the register file |
| R3 | shared bytes exceed the per-block limit, opt-in included |
| R4 | no workgroup is resident on a multiprocessor |
| R5 | never: records whether the grid fills the multiprocessors, or the split-K count that would |
| R6 | never: records the padding the shape wastes at this problem |

Every pruned candidate is kept with its rule, its need and the limit it
exceeded. An empty survivor set is a named refusal (`XpuLaunchException`
with the family, the problem and every verdict), not a fallback.

### 4.2 At first use: the measured choice

Among the survivors, `ShapeChoice` picks by measurement. One survivor is
taken outright. Several are timed through the family's probe, every survivor
once per round, five rounds interleaved so that a drifting clock cannot
favour an arm, and the fastest median wins; a lead under three percent is a
tie, recorded as such. The winner is remembered in the `Autotune` store under
`shape.<family>.<M>x<N>x<K>` against a build id made from every survivor's
code hash, so a rebuild that changes any candidate measures again, and a
hint written for a kernel that no longer exists is discarded and reported.
A measurement taken while another process held more than a quarter of the
device is used for this run and not stored. A sweep can force a member for a
family (`ShapeChoice.setShapeOverride`), and a test can hold the trial so
that an undecided problem runs its first survivor untimed.

The probe is the family's own: a burst of launches at the problem's shape, or,
for the mixture-of-experts GEMM families, the real launches of the running
model, bracketed in place by `KernelTimer`. The timer has tiers: device
events on CUDA and HIP, the synchronous host clock on the CPU backend, and
unavailable on Vulkan outside the profiler's timestamp seam, where it answers
so and the caller times on the host. On first use it calibrates the event
clock against the host clock (the CUDA event clock runs several percent fast
on WSL2) and refuses a scale outside sane bounds. Timing in place costs stream
synchronisations, which is why a library warms its probes at load and why a
measurement of the model is read only after the store has settled.

### 4.3 At load and once per process: the pays probes

A library's remaining choices are binary and measured once: does this
device's matrix-core GEMM beat its integer dot-product GEMM at all (on the
CPU backend it does not); does a prefetching variant of a mat-vec beat the
plain one; does serving four sequences from one weight pass beat four passes;
do fused launches beat separate ones. Each is an A/B of real launches at the
real width, remembered per device and per kernel-code fingerprint, with a
pin for tests. [llm.md §4](kernels-llm.md) lists them.

### 4.4 The store

`Autotune` is one small file per (device, name) under a directory the
application supplies: no parser, nothing to corrupt, a stale entry removed by
deleting a file. The device key is the backend plus the geometry that
changes what the right answer is, so a store copied between machines is
ignored rather than believed, and a part that reports no geometry is keyed
as unknown and shares nothing. `rememberFor` and `recallFor` carry a build
id; a value recalled against a different build is dropped and the drop is
reported. A box with no writable store still tunes once per execution rather
than once per call. The test suites run with the store off, so no earlier
run's verdict reaches a test.

## 5. The gates that keep it honest

Adaptation that can silently pick a fast wrong answer is worse than a
constant. Four gates hold the line.

**The reference interpreter** (§1) decides what a kernel means; every backend
is compared to it.

**The build gates** (§2.2) refuse a spilling kernel, a kernel with no device
code for a declared backend, and an unbounded launch, each by name, each with
one declared escape that itself goes stale when its cause is gone.

**The kernel census.** After a library's test run on a backend, every kernel
the backend registered must have been value-checked by some test against a
host or oracle value, or be excused by name against a plan item; an uncovered
kernel fails the run, and so does an excuse that is no longer needed. Running
is not coverage. The committed census tables are the record of what each
backend has proven ([llm.md §6](kernels-llm.md)).

**Measurement discipline.** A device-timing bracket near the launch floor
reads up to twelve percent of noise, so a timed choice uses twenty-launch
brackets, a warm round, interleaved rounds and medians, and a kernel-level gap
is believed only after the generated code is diffed. A route change in an
inference library is gated by token-for-token agreement with the reference
implementation, because a route that is fast and wrong looks like a win in
every other instrument.

## 6. What follows from this for a kernel author

- Ask the device, never a backend name. The one place a backend name is
  tested in the LLM library is documented as a watchdog workaround, not a
  capability.
- Declare what only you know: the structural block, the wave your indexing
  assumes, the shapes your body serves and the constraint that refuses the
  rest.
- Read what the compiler measured before you time anything: a spill or a
  serialised load chain is in the manifest and the generated code.
- Write a design as a template over its shape and let the filter and the
  timer choose the member; never hand-pick a tile for the part on your desk.
- When a kernel runs far below the memory bus, time the design that reaches
  the bus on the equivalent problem before reading its instruction stream.
- Every probe needs a pin for tests, and every tuned value needs a build id.
