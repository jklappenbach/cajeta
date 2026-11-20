# kernel-atomic-scope — kernel atomics are device-scoped unless a kernel asks for more

Plan: [kernel-atomic-scope-plan](../../agents/archive/kernel-atomic-scope-plan.md).
Decided with Julian 2026-11-19: device scope by default, system scope as an explicit opt-in.

## 1. Definition

A kernel atomic orders and publishes its access to some set of observers, its scope. A
`shared` array's atomic reaches its workgroup. A `KernelBuffer` atomic reaches the whole
device, and that is what `IntegerAtomics.md` promises: "a global `KernelBuffer<T>` atomic
stays at Device scope". Vulkan already lowers it that way. The nvptx and amdgpu lowering
does not. It emits LLVM's default, system scope, so every `KernelBuffer` atomic on cuda is
an `atom.*.sys`: ordered against the host and every peer device as well as the GPU.

That reach costs real time on NVIDIA. The bench scan (cajeta-ml-gpu 18.2.4) publishes each
tile's prefix through release and acquire atomics, and on the 4090 it ran at 1490 us,
0.42x CUB. The same kernel with its atomics at device scope and its stores coalesced runs at
560 us, ahead of CUB's 594 (probe, 2026-11-19, every variant checked against the host):

| atomics | 16 scalar stores | four 16-byte stores |
|---|---|---|
| system scope (today) | 1490 us | 1390 us |
| device scope | 1225 us | 560 us |

CUDA and HIP both default to device scope: `atomicAdd` is device-scoped, and the
host-reaching form is the separately named `atomicAdd_system`. Scoped device fences already
work this way: `Barrier.deviceMemory` is `membar.gl` on nvptx and an `agent` fence on amdgpu.
The atomics are the outlier.

System scope is needed in one case only: a kernel that signals the host, or another device,
through a `Pinned` or `Unified` buffer while it is still running. A kernel boundary already
publishes everything a launch wrote, so a host that reads after `sync()` needs nothing more.
An audit of cajeta, cajeta-llm, cajeta-ml, cajeta-cabra and cajeta-xgboost on 2026-11-19
found no source that uses atomics on a host-visible buffer.

## 2. Features

### 2.1 A `KernelBuffer` atomic is device-scoped by default

Every `KernelBuffer` atomic (`atomicAdd` through `atomicCompareExchange`, integer and
float) lowers at device scope on every GPU backend: `.gpu` on nvptx, `agent` on amdgpu,
Device on Vulkan as today. A `shared` array's atomic stays at workgroup scope. The cpu
backend and the reference interpreter have a single observer set, so nothing changes there.

Use cases:
1. A decoupled look-back scan publishes tile prefixes at device scope and runs at CUB's pace.
2. A histogram, a split-K counter or a work queue in `KernelBuffer` memory pays device-scope
   atomics without asking.
3. An existing kernel's results are unchanged: every observer it had was on the device.

### 2.2 A kernel asks for system scope by name

An atomic takes an optional trailing `MemoryScope`, a compile-time constant after its
`MemoryOrder`: `Workgroup`, `Device` or `System`.
`flag.atomicExchange(0, 1, MemoryOrder.Release, MemoryScope.System)` is the form that
signals the host through a `Pinned` or `Unified` buffer mid-launch. Vulkan has no shader
scope wider than Device that the driver accepts (the same constraint that set AcqRel), so
there `System` lowers to Device and the reference says so. `Workgroup` on a `KernelBuffer`
atomic is legal and narrows the reach; it's for a buffer only one workgroup touches.

Use cases:
1. A persistent kernel publishes progress to a pinned buffer the host polls.
2. A kernel that wants the narrowest scope states it, and the compiler emits it.
3. A scope argument that isn't a compile-time constant is a compile error naming the method.

### 2.3 The documentation says what the code does

`IntegerAtomics.md`, `FloatAtomics.md` and CajetaXPU §2.2 describe the default, the opt-in,
and the one case that needs it. The seam comment that calls the default "monotonic system
scope" is corrected.

Use cases:
1. A reader learns from one table which scope each backend emits for each storage.
2. A kernel author porting CUDA code that calls `atomicAdd_system` finds the equivalent.
