# In-kernel timing: the shader clock

`Thread.clock()` reads a free-running hardware counter from **inside a running
kernel** — a 64-bit tick value. You read it before and after a region and diff
the two to measure how long that region took, *on the device, per invocation*,
without a CPU round-trip.

```
uint64 t0 = Thread.clock();
// ... work to measure ...
uint64 t1 = Thread.clock();
uint64 elapsed = t1 - t0;   // device ticks
```

**It is ticks, not seconds.** The value is a raw counter for *relative*
measurement — comparing two regions, finding the hot branch, spotting lane
divergence. Don't convert it to wall-clock time.

## What it's for

- **In-kernel profiling** — which loop or branch dominates, measured where it
  runs rather than inferred from a host-side dispatch timer.
- **Divergence / tail measurement** — diff per lane to see how unevenly work is
  distributed across a subgroup.
- **Memory-latency probing** — time a dependent load chain on-device.

It is a tuning/research instrument (e.g. timing SPELA layers or Toffee kernels
on-device), not something an end-user app calls.

## How it lowers — every backend, native clock

| Backend | Lowering |
|---|---|
| **Vulkan** (RADV, etc.) | `OpReadClockKHR` at **Subgroup** scope (`SPV_KHR_shader_clock`, `ShaderClockKHR` capability). |
| **AMD** | `s_getreg HW_REG_SHADER_CYCLES` (the RDNA shader-cycles register; lowered from `llvm.readcyclecounter`). |
| **NVIDIA** | `clock64` (the SM clock register). |
| **CPU** | `rdtsc` (`llvm.readcyclecounter`). |

The Vulkan path needed a **fork addition** to the SPIR-V backend: the
`OpReadClockKHR` op exists upstream but is only reachable through the OpenCL
builtin path, which is gated off for the Vulkan/Shader flavor. cajeta's fork adds
the `llvm.spv.read.clock` intrinsic + selection so the Shader flavor can reach it
(the same pattern used for cooperative matrix and ray query). Unlike a backend
bug fix, this is **fork-carried, not an upstream PR** — upstream has no in-tree
frontend that would emit such an intrinsic.

## Caveats

- **Non-deterministic.** Two runs give different tick values; the clock counts
  real hardware cycles. Tests assert the clock is *live* (non-zero, monotonic
  within a thread), never an exact number.
- **Don't let the optimizer hoist your reads.** The two `clock()` calls model a
  side effect so they aren't merged, but a measured region with no observable
  result can still be dead-code-eliminated — make the region's output feed the
  store (or `out[i]`) so it survives.
- **Scope.** The Vulkan path reads the Subgroup clock (per-wave). That's the
  right scope for timing a region within a thread; it is not a global wall clock
  shared across workgroups.

---

**Rules.** `Thread.clock()` returns a `uint64` device tick, device-only (inside
an `@Kernel`). Diff two reads for an elapsed-cycle count; the value is for
relative measurement, not seconds, and is non-deterministic. Runnable in
`samples/tour/gpu` (the `shader clock` section). See `CajetaXPU.md` for the kernel
surface. (Implementation note: the Vulkan reach is a `cajeta-spirv` fork
intrinsic, `llvm.spv.read.clock` — fork-carried, not upstreamed.)
