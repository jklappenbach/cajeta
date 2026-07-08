# OptiX RT-Core Ray Query — M3: Multi-Impl AS + Launch-Time Selection Spec

## 1. Background

M0–M2 built the full NVIDIA OptiX RT-core path: the runtime AS provider
(`CAJ_AS_IMPL_OPTIX`, M1) and the verb codegen (M2) — four canonical ray-query shapes
(AABB candidate-count, triangle nearest-hit, triangle candidate getters, committed-
triangle per-launch) run on the 4090's RT cores via an `optixLaunch` pipeline, each
matching the software oracle. See `rayquery-optix-m2-codegen-{spec,plan}.md`.

M2 left one deliberate gap, recorded as the **AUTO-stays-software policy**
(`[[optix-auto-policy]]`, plan 4-C): on CUDA, AUTO resolves the AS impl to the portable
software BVH; OptiX is reachable only via the explicit `CAJETA_GPU_AS_IMPL=optix` opt-in.
The flip was deferred because it is **unsafe under the current single-impl model**, and a
per-compilation-unit "all kernels supported → flip" gate would be brittle (doesn't
compose across modules; all-or-nothing; hoists a per-launch property to a global flag).

M3 fixes the root cause so that AUTO can prefer RT cores **safely** — and generalizes the
noun-impl model across all backends in the process.

## 2. The root cause (the category error to fix)

Today an `AccelerationStructure` commits to **one** impl at build time. The noun provider
(`CajetaNounProvider.accel_build_*` in `cajeta_runtime.c`) calls
`caj_<backend>_resolve_as_impl(pref)` at `heap AccelerationStructure(...)` time, builds a
**single** representation, and records a single `out_impl` (read back as `implTag()`). The
software-BVH handle **is** the host BVH-blob pointer; the OptiX handle is an `OptixAs*`;
the Vulkan handle is a BLAS/TLAS descriptor — distinct, non-interchangeable
representations.

The launch then reads that one handle. The CUDA dispatch already checks
`rq && cajeta_xpu_optix_available() && caj_cuda_resolve_as_impl(AUTO)==CAJ_AS_IMPL_OPTIX`;
if it doesn't take the OptiX path it runs the kernel's software cubin over the handle.

The category error: **impl is modelled as a property the AS commits to at build, but
whether a kernel can traverse a given impl is a property of the *(kernel-shape, impl)*
pair — known only at launch.** At build the AS does not know its consumer kernels, so
AUTO would have to *guess* OptiX; if any consumer is an Unsupported shape, that kernel's
software cubin receives an `OptixAs*` it misreads as a BVH blob → fault. That single fault
mode is the entire reason OptiX is opt-in.

## 3. Goal

Make `AccelerationStructure` a **multi-impl noun** that always retains the portable
software BVH as a floor and additionally carries the device-native representation
(OptiX on CUDA, BLAS/TLAS on Vulkan) when the device supports it; and move impl
**selection to launch time**, where the runtime already knows the consuming kernel's
shape. A supported-shape kernel uses the native/OptiX representation; any other kernel
transparently falls back to the retained software BVH. With the fault mode gone, **AUTO
on CUDA can prefer OptiX RT cores by default** without opt-in and without miscompiling or
faulting any shape.

## 4. Requirements

- **R1 — Multi-impl noun.** An `AccelerationStructure` may hold more than one built
  representation: the software BVH (always, as the floor) plus, when the active device
  supports it, the native one (CUDA→OptiX `OptixAs`, Vulkan→BLAS/TLAS). The build no
  longer collapses to a single `out_impl`; the noun records the **set** of available
  impls. The `CajetaNounProvider` contract and the host-side AS object grow to carry the
  per-impl handles.
- **R2 — Launch-time selection (the verb picks).** At launch the runtime selects, per
  consuming kernel, the best impl that kernel can actually use: a registered OptiX
  program (or Vulkan native seam) + the AS having that representation → the native path;
  otherwise the software cubin over the retained software BVH. The kernel-arg marshalling
  hands the device side the representation matching the path taken, so the device-side
  per-impl contract is unchanged (a software kernel still receives the BVH blob pointer;
  an `optixLaunch` still receives the traversable + boxes). No kernel ever receives a
  representation it cannot traverse.
- **R3 — Safe AUTO on CUDA.** With R1+R2 the fault mode is gone, so `caj_cuda_resolve_as
  _impl`'s AUTO case may build the OptiX representation (in addition to the software
  floor) when OptiX is available. AUTO-consumed supported-shape kernels then run on RT
  cores; Unsupported-shape kernels run on the software floor — both correct, no env var.
  `CAJETA_GPU_AS_IMPL=software` still forces the floor only; `=optix` still forces the
  native build (and now errors loudly, not faults, if a consumer is Unsupported — see R6).
- **R4 — Lazy / elidable native build (cost control).** Building two representations costs
  memory + build time. The native (OptiX/Vulkan) representation SHOULD be built lazily —
  on the first native-capable launch against that AS — so an AS only ever consumed by
  software kernels never pays for it. A user hint (e.g. an `AsImpl`/builder option) MAY
  drop the software floor when the caller asserts all consumers are supported, trading
  the safety net for memory.
- **R5 — Compiler shape classification as an *optimization*, not a gate.** The M2 shape
  classifier (`classifyRayQueryShape`) becomes an input that lets the runtime/compiler
  *skip* building a representation that no consumer needs (don't build OptiX for an AS
  with no supported consumer; permit dropping the software floor when every consumer is
  supported) — never a correctness gate. Correctness holds even with no classification
  (the software floor always works).
- **R6 — Diagnostics, not faults, on forced mismatch.** Under `=optix`, a ray-query
  kernel whose shape is Unsupported must produce a clear runtime diagnostic (or fall back
  to the retained software floor) — never read an `OptixAs*` as a BVH blob. The current
  silent-fault path is eliminated.
- **R7 — `implTag()` / `Device.supports` model.** `AccelerationStructure.implTag()` grows
  from a single tag to expose the **set** of available impls (and/or the impl a given
  launch actually used), preserving backward-compatible reads where tests assert a single
  value. `Device.supports(Capability.{RayQueryNative,RayQueryRtCore})` is unchanged.
- **R8 — Cross-backend generality.** The multi-impl noun + launch-time selection model is
  not OptiX-specific: it subsumes the Vulkan native-vs-software choice (an AS carrying
  software + BLAS/TLAS, the verb picking) and reduces to today's behavior as the
  single-impl degenerate case. AMD (software-BVH-only) is unaffected.
- **R9 — On-device parity + no regression.** All four OptiX canonical shapes run under
  **AUTO** on the 4090 (no env) and match the software oracle (777); the software floor
  stays correct for Unsupported shapes under AUTO; the existing forced-`software`/`optix`,
  Vulkan-native, and `…OnNvptxSoftwareBvh` behaviors are preserved (the latter may now run
  on RT cores under AUTO — those tests are re-pointed to force software to keep the
  software-walk coverage they exist for).

## 5. Non-Goals

- New ray-query shapes (non-const-ray getters, AABB generate-intersection, etc.) — M3 is
  the selection/impl-model change, not codegen breadth. Those stay future M2 follow-ups.
- General CPS restructuring of arbitrary `proceed()` loops (unchanged from M2).
- Multi-level AS / instancing (TLAS), motion, SER, OptiX-IR input.
- Sharing one built AS across processes, or serializing built ASs.
- Changing the device-side per-impl traversal contract (R2 keeps it fixed).

## 6. Acceptance Criteria

**STATUS (2026-06-17): MET — M3 Phases 1–5 COMPLETE on the 4090, no open items** (see
`rayquery-optix-m3-multiimpl-plan.md`). 1: ✅ (`multiImplAs*`, `implSet()`). 2: ✅
(`lazyOptixBuiltOnFirstNativeLaunch`/`autoPrefersOptixForTriangleShape` → RT cores under
AUTO; `softwareOnlyConsumerSkipsOptixBuild` → floor for Unsupported, no fault). 3: ✅
(`sameAsTwoKernelsSelectsPerLaunch` — one AS, both impls per launch). 4: ✅ (lazy build —
3a `701`, 3b/4a `705`). 5: ✅ (`forcedOptixUnsupportedShapeFallsBackToSoftware`). 6: ✅
(re-pointed software-walk tests + OptiX device + Vulkan/AMD/CPU + capability + emit). 7: ✅
(docs + `[[optix-auto-policy]]`). The drop-software-floor hint (R4's *MAY*) is implemented
via `AsImpl.NativeNoFloor` (`dropSoftwareFloorHintHonored` → 704, OptiX-only impl set).

1. An `AccelerationStructure` built on a CUDA+OptiX device under **AUTO** carries both the
   software BVH and (lazily) the OptiX representation; `implTag()` reports the available
   set.
2. Under **AUTO** (no env), the four canonical OptiX shapes run on the RT cores and match
   the software oracle (777) on the 4090; a same-AS Unsupported-shape kernel under AUTO
   runs correctly on the software floor (no fault).
3. A single AS consumed by both a supported-shape kernel and an Unsupported-shape kernel
   resolves each to the correct impl at launch (RT cores vs software) with correct results
   — the test that proves launch-time selection.
4. Lazy native build: an AS consumed only by software kernels never builds the OptiX
   representation (observable via a build-count probe or `implTag()` set).
5. Forced `=optix` with an Unsupported shape produces a clear diagnostic or software
   fallback — never the silent `OptixAs*`-as-blob fault (R6).
6. No regression: forced-`software`, Vulkan-native (`autoRecordsNativeImplOnDevice` and
   the native ray-query suite), AMD software-BVH, and the M0–M2 OptiX device + emit + probe
   suites stay green; the `…OnNvptxSoftwareBvh` tests keep software-walk coverage (re-
   pointed to force software where AUTO would now choose OptiX).
7. Docs + memory updated (RayQuery.md §6 selection model + the AUTO-on-CUDA flip;
   `[[optix-auto-policy]]` superseded/updated; `[[optix-env]]`); spec/plan under
   `docs/specification/gpu/rayquery/`.
