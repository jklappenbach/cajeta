# Integer atomics: the universal concurrency primitive

Where [float atomics](FloatAtomics.md) cover lock-free *reductions*, integer
atomics on `Buffer<int32>` / `Buffer<uint32>` are the broader concurrency
toolkit — counters, histograms, bitsets, flags, work queues, and (with
compare-exchange) arbitrary lock-free updates. Every form is one hardware
instruction that returns the **old** value.

```
a.atomicAdd(i, v);       // *(a+i) += v          (also atomicSub)
a.atomicMin(i, v);       // *(a+i) = min(old, v)  (signed buffer -> SMin, unsigned -> UMin)
a.atomicMax(i, v);       // *(a+i) = max(old, v)
a.atomicAnd(i, v);       // *(a+i) &= v           (also atomicOr, atomicXor)
a.atomicExchange(i, v);  // swap: old = *(a+i); *(a+i) = v
uint32 old = a.atomicCompareExchange(i, expected, desired);
                         // if *(a+i) == expected: *(a+i) = desired; returns old either way
```

Unlike the float atomics (which ride `SPV_EXT_shader_atomic_float`), the integer
forms are **core** — no extension, on every backend.

## The canonical uses

**Histogram / counters** — many threads bump shared bins, no locks:

```
@Kernel
public static void histogram(Buffer<uint32> bins, Buffer<uint32> data, uint32 n) {
    uint32 i = Thread.globalIdX();
    if (i < n) {
        bins.atomicAdd(data[i], 1);   // ++bins[data[i]] atomically
    }
}
```

**Lock-free update via compare-exchange** — the universal primitive. Read the
current value, compute a new one, and swap it in *only if nobody changed it
meanwhile*; retry on contention:

```
uint32 old = slot.atomicCompareExchange(0, expected, desired);
// old == expected  -> the swap happened
// old != expected  -> someone else won; `old` is the current value, retry
```

> **Warning — the GPU spin-CAS trap.** A retry loop where *every* lane hammers
> *one* shared location can **livelock** on a GPU: under lockstep subgroup
> execution the lane that would make progress can be starved by the lanes
> spinning around it. Prefer the lock-free RMW ops (`atomicAdd` etc.) for
> contended counters, and reserve CAS loops for *low-contention* or
> *per-lane-distinct* targets.

## How it lowers

| Backend | Lowering |
|---|---|
| **Vulkan** (RADV) | core `OpAtomicIAdd/ISub/SMin/UMin/SMax/UMax/And/Or/Xor/Exchange/CompareExchange`, **Device** scope + **AcquireRelease** (the same memory-model constraint the float path hit — Vulkan rejects CrossDevice scope / SequentiallyConsistent). |
| **AMD / NVIDIA** | native global `atomicrmw` / `cmpxchg` (the hardware atomic). |
| **CPU** | a lock-prefixed atomic / `cmpxchg`. |

The op is just a generic LLVM `atomicrmw` (or `cmpxchg`); the SPIR-V backend
picks the opcode from the BinOp and `isSigned` selects `SMin/SMax` vs
`UMin/UMax`. Lowered on a `LoweringTarget::atomicIntRMW` / `atomicCompareExchange`
seam (default monotonic system-scope; Vulkan overrides to Device + AcquireRelease).

### A pre-existing upstream SPIR-V bug surfaced here

`atomicCompareExchange` was the **first** construct to exercise an unhandled case
in upstream LLVM's `SPIRVLegalizePointerCast` pass: a `cmpxchg` whose pointer
flows through a `spv_ptrcast` (the descriptor-bound storage-buffer element
pointer) hit an `llvm_unreachable("Unsupported ptrcast user. Please fix.")` — a
release-mode segfault. HLSL's `InterlockedCompareExchange` lowers its pointer
differently, so upstream never tripped it. Fixed on `cajeta-spirv` by handling
the `spv_cmpxchg` user like the existing `spv_gep` case (bypass the spurious
cast); upstream-reportable.

## Caveats

- **32-bit (`int32`/`uint32`) v1.** 64-bit integer atomics need the `Int64Atomics`
  capability — a follow-on. `atomicMin/Max` signedness comes from the buffer
  element type (`int32` → signed, `uint32` → unsigned).
- **Returns the old value.** Every op returns the pre-update value (CAS returns
  the old value whether or not the swap happened — compare it to `expected`).

---

**Rules.** `Buffer<int32|uint32>.atomic{Add,Sub,Min,Max,And,Or,Xor,Exchange}
(index, value)` and `.atomicCompareExchange(index, expected, desired)` are
device atomic read-modify-writes returning the old value — core
`OpAtomicI*`/`OpAtomicCompareExchange` (no extension), Device scope +
AcquireRelease on Vulkan. Device-verified bit-exact on CPU + RADV + gfx1151
(`XpuAtomicDeviceTests.intAtomicsRunOn{Cpu,VulkanDevice,AmdDevice}`) and
spirv-val-clean (`XpuVulkanEmitTests.lowersIntAtomicsToSpirv`). See
[`FloatAtomics.md`](FloatAtomics.md) for the float reduction family.
