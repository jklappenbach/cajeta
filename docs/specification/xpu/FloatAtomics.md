# Float atomics: lock-free parallel reductions

A reduction folds the values that many threads produce into one shared
accumulator. A sum, a max or a min over that data must not lose updates to a
race. The atomic read-modify-write is the primitive that guarantees this, and
`KernelBuffer<float32>` exposes it directly.

```
out.atomicAdd(i, v);   // *(out+i) += v   atomically; returns the old value
out.atomicMax(i, v);   // *(out+i) = max(*(out+i), v)
out.atomicMin(i, v);   // *(out+i) = min(*(out+i), v)
```

Each is one hardware instruction. A thousand threads can hit the same address at
once and the result is still correct.

## The canonical use: fold N threads into one slot

```
@Kernel
public static void reduce(KernelBuffer<float32> out, KernelBuffer<float32> in, uint32 n) {
    uint32 i = KernelThread.globalIdX();
    if (i < n) {
        out.atomicAdd(0, in[i]);   // parallel sum   -> out[0]
        out.atomicMax(1, in[i]);   // parallel max   -> out[1]
        out.atomicMin(2, in[i]);   // parallel min   -> out[2]
    }
}
```

Every thread races to `out[0..2]`. The atomics serialize just those updates, so
`out[0]` ends as the exact sum regardless of thread order. This is the histogram,
scatter-add and loss-accumulation pattern, and it is common in scientific
reductions and in ML for gradient accumulation and soft-assignment counts.

## How it lowers — same source, every backend

| Backend | Lowering |
|---|---|
| **Vulkan** (RADV, etc.) | `OpAtomicFAddEXT` / `OpAtomicFMinEXT` / `OpAtomicFMaxEXT` under `SPV_EXT_shader_atomic_float_add` / `_min_max`, with **Device scope + AcquireRelease** memory order. |
| **AMD / NVIDIA** | the native global FP atomic (`global_atomic_add_f32`, `red.add.f32`), relaxed system scope. |
| **CPU** | `atomicrmw` — a lock/`cmpxchg` loop where the hardware lacks a native FP atomic. |

The Vulkan memory model is stricter than the others. It rejects CrossDevice
scope and `SequentiallyConsistent` semantics on these ops, and it rejects
relaxed-with-storage-class semantics as well, so the Vulkan path emits
Device-scoped AcquireRelease atomics. Cajeta handles that in the lowering seam,
and you just write `atomicAdd`.

## Numerical note

Floating-point addition is not associative, so a concurrent `atomicAdd` sum can
differ in the last bits run-to-run when the addends are arbitrary floats. It is
exact and order-independent only when every partial sum stays integer-valued and
below `2^24`, the f32 integer-exact range. The Tour's reduction has inputs
`0..255`, so it is bit-exact. For large or fractional data, treat the sum as
correct to floating-point rounding rather than bit-reproducible.
`atomicMax`/`atomicMin` are always order-independent.

### Why not the alternatives?

| Alternative | What it forces |
|---|---|
| A plain `out[0] = out[0] + in[i]` from every thread | A data race — concurrent read-modify-writes lose updates; the sum comes out low and non-deterministic. |
| A tree reduction in shared memory + one writer | More code and a barrier; the right call for a *dense* reduction, but overkill when threads scatter into many bins (a histogram) — that is exactly what atomics are for. |
| Per-thread partial sums + a second pass | An extra buffer and kernel launch; atomics collapse it to one pass. |

---

**Rules.** Float atomics are methods on `KernelBuffer<float32>` (f32, v1): `atomicAdd`,
`atomicMin`, `atomicMax` of `(index, value)`, returning the old value. They are
device-only (inside an `@Kernel`) and require a writable buffer. On Vulkan
they need `SPV_EXT_shader_atomic_float_add` / `_min_max` (RADV exposes both for
f32). They are runnable end to end in `samples/tour/gpu` (the `float atomics` section).
See `CajetaXPU.md` for the kernel surface. Integer atomics
(`atomicAdd/Sub/Min/Max/And/Or/Xor/Exchange/CompareExchange`) and shared-memory
(LDS) atomics have since shipped and are described in [`IntegerAtomics.md`](IntegerAtomics.md).
f16/f64 float atomics remain follow-ons (`cajeta-xpu-plan.md` Stage 9).

**Memory order.** An optional compile-time `MemoryOrder` trailing arg applies to
float atomics too (`out.atomicAdd(0, v, MemoryOrder.Relaxed)`), as described in
[`IntegerAtomics.md`](IntegerAtomics.md). Vulkan's `OpAtomicF*EXT` rejects
relaxed-with-storage-class semantics (above), so Vulkan clamps `Relaxed`/`SeqCst` to
`AcqRel` for float atomics. CPU/AMD/NVPTX honour the requested order.
