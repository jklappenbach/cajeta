# Integer atomics: the universal concurrency primitive

[Float atomics](FloatAtomics.md) cover lock-free reductions. Integer
atomics on `KernelBuffer<int32>` / `KernelBuffer<uint32>` are the broader concurrency
toolkit, covering counters, histograms, bitsets, flags, work queues, and, with
compare-exchange, arbitrary lock-free updates. Every form is one hardware
instruction that returns the old value.

```
a.atomicAdd(i, v);       // *(a+i) += v          (also atomicSub)
a.atomicMin(i, v);       // *(a+i) = min(old, v)  (signed buffer -> SMin, unsigned -> UMin)
a.atomicMax(i, v);       // *(a+i) = max(old, v)
a.atomicAnd(i, v);       // *(a+i) &= v           (also atomicOr, atomicXor)
a.atomicExchange(i, v);  // swap: old = *(a+i); *(a+i) = v
uint32 old = a.atomicCompareExchange(i, expected, desired);
                         // if *(a+i) == expected: *(a+i) = desired; returns old either way
```

The float atomics ride `SPV_EXT_shader_atomic_float`. The integer
forms are core, with no extension, on every backend.

## The canonical uses

**Histogram / counters.** Many threads bump shared bins with no locks:

```
@Kernel
public static void histogram(KernelBuffer<uint32> bins, KernelBuffer<uint32> data, uint32 n) {
    uint32 i = KernelThread.globalIdX();
    if (i < n) {
        bins.atomicAdd(data[i], 1);   // ++bins[data[i]] atomically
    }
}
```

**Lock-free update via compare-exchange.** This is the universal primitive. Read the
current value, compute a new one, and swap it in only if nobody changed it
meanwhile. Retry on contention:

```
uint32 old = slot.atomicCompareExchange(0, expected, desired);
// old == expected  -> the swap happened
// old != expected  -> someone else won; `old` is the current value, retry
```

> **Warning — the GPU spin-CAS trap.** A retry loop where every lane hammers
> one shared location can livelock on a GPU. Under lockstep subgroup
> execution the lane that would make progress can be starved by the lanes
> spinning around it. Prefer the lock-free RMW ops (`atomicAdd` etc.) for
> contended counters, and reserve CAS loops for low-contention or
> per-lane-distinct targets.

## How it lowers

| Backend | Lowering |
|---|---|
| **Vulkan** (RADV) | core `OpAtomicIAdd/ISub/SMin/UMin/SMax/UMax/And/Or/Xor/Exchange/CompareExchange`, **Device** scope + **AcquireRelease** (the same memory-model constraint the float path hit — Vulkan rejects CrossDevice scope / SequentiallyConsistent). |
| **AMD / NVIDIA** | native global `atomicrmw` / `cmpxchg` (the hardware atomic). |
| **CPU** | a lock-prefixed atomic / `cmpxchg`. |

The op is just a generic LLVM `atomicrmw` (or `cmpxchg`). The SPIR-V backend
picks the opcode from the BinOp, and `isSigned` selects `SMin/SMax` or
`UMin/UMax`. It is lowered on a `LoweringTarget::atomicIntRMW` / `atomicCompareExchange`
seam, whose default is monotonic system scope and which Vulkan overrides to Device + AcquireRelease.

## Shared-memory (LDS) atomics

The same `atomic*` methods work on a `shared T[]` array. This is the fast path for
per-block reductions, histograms, and allocation, since an LDS atomic is far
cheaper than a global one:

```
Shared<uint32> acc = shared uint32[1];
...
acc.atomicAdd(0, 1);     // a Workgroup-scope LDS atomic, not a global one
```

The atomic's memory scope follows the pointer's storage. A `shared` array is
Workgroup storage (LLVM addrspace 3), so its atomics emit at Workgroup scope
(`ds_add_u32` on AMD; `OpAtomicIAdd` with Workgroup scope on Vulkan), while a
global `KernelBuffer<T>` atomic stays at Device scope. The scope is derived from the
pointer address space, so there is no separate API. This is device-verified on RADV and gfx1151
(`Xpu*SharedDeviceTests.sharedAtomicCounterRunsOnDevice`).

## Memory order (`MemoryOrder`)

The ordering is an optional, compile-time-constant trailing argument. Omit it
for the safe default, which is the backend's release/acquire, `AcqRel` on Vulkan and the
native default elsewhere:

```
out.atomicAdd(0, 1, MemoryOrder.Relaxed);   // histogram/counter: no ordering needed
flag.atomicExchange(0, 1, MemoryOrder.Release);
```

`MemoryOrder` is `{ Relaxed, Acquire, Release, AcqRel, SeqCst }`. LLVM bakes the
ordering into the atomic instruction at IR-build time, so the value must be a
literal `MemoryOrder.X` rather than a runtime variable. The dominant use is `Relaxed`
for pure counters, histograms and reductions where only the final value matters.
It is the cheapest atomic, since atomicity still holds and only ordering is dropped.

**Per-backend.** CPU / AMD / NVPTX honour all five orderings. For example, `Relaxed` becomes
a `monotonic` atomicrmw, the native relaxed atomic. Vulkan clamps `Relaxed`
and `SeqCst` up to `AcqRel`, because its memory model rejects a bare-relaxed device
atomic (strict `spirv-val` requires storage-class acquire/release semantics on
the op). So the relaxed-atomic perf win lands on CPU/AMD/NVPTX while Vulkan
stays correct. This is device-verified with a relaxed-atomic counter (exact count) on
CPU, RADV and gfx1151, and emit-verified order→ordering on NVPTX. The same surface
applies to the scoped memory fences (`Barrier.deviceMemory(MemoryOrder.Acquire)`).

> A shared atomic on element 0 surfaced another facet of the same upstream
> `SPIRVLegalizePointerCast` gap as `atomicCompareExchange`. The element GEP folds
> to the aggregate base, so the `atomicrmw` flows through a `spv_ptrcast` whose
> atomic user was unhandled, while load/store users are handled. It is fixed on `cajeta-spirv`
> alongside the cmpxchg case, and the upstream PR covers both.

### A pre-existing upstream SPIR-V bug surfaced here

`atomicCompareExchange` was the first construct to exercise an unhandled case
in upstream LLVM's `SPIRVLegalizePointerCast` pass. A `cmpxchg` whose pointer
flows through a `spv_ptrcast` (the descriptor-bound storage-buffer element
pointer) hit an `llvm_unreachable("Unsupported ptrcast user. Please fix.")`, which is a
release-mode segfault. HLSL's `InterlockedCompareExchange` lowers its pointer
differently, so upstream never tripped it. It is fixed on `cajeta-spirv` by handling
the `spv_cmpxchg` user like the existing `spv_gep` case, bypassing the spurious
cast, and it is upstream-reportable.

## Caveats

- **32-bit (`int32`/`uint32`) v1.** 64-bit integer atomics need the `Int64Atomics`
  capability, which is a follow-on. `atomicMin/Max` signedness comes from the buffer
  element type (`int32` → signed, `uint32` → unsigned).
- **Returns the old value.** Every op returns the pre-update value. CAS returns
  the old value whether or not the swap happened, so compare it to `expected`.

---

**Rules.** `KernelBuffer<int32|uint32>.atomic{Add,Sub,Min,Max,And,Or,Xor,Exchange}
(index, value)` and `.atomicCompareExchange(index, expected, desired)` are
device atomic read-modify-writes returning the old value. They lower to core
`OpAtomicI*`/`OpAtomicCompareExchange` (no extension), at Device scope +
AcquireRelease on Vulkan. They are device-verified bit-exact on CPU, RADV and gfx1151
(`XpuAtomicDeviceTests.intAtomicsRunOn{Cpu,VulkanDevice,AmdDevice}`) and
spirv-val-clean (`XpuVulkanEmitTests.lowersIntAtomicsToSpirv`). See
[`FloatAtomics.md`](FloatAtomics.md) for the float reduction family.
