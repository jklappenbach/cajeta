# Cajeta GPU Vendor Extension SDK — seed

**Status: SEED.** This is a placeholder for the SPI that lets a third party, such
as a driver vendor or an interest group, ship a GPU extension library for cajeta.
It records the decisions already made and is not a buildable spec. None of it is
built, and it cannot be specced concretely yet (see "Why a seed" below). The
concrete API lands when the work does.

**Audience:** external extension authors (NVIDIA, AMD, Apple, a research group).
This is deliberately a separate document from [`CajetaGPU.md`](CajetaGPU.md).
That one is cajeta's internal foundation contract, and this one is the
third-party SPI. They have a different reader and a different stability promise.

---

## Why a seed, not a spec

The governing principle (from [`CajetaGPU.md §1`](CajetaGPU.md)) is:

> **Core is just the in-tree "vendor library" that has a fallback for everything.**

So this SDK is the seam machinery core already uses internally, exposed,
stabilized, and documented for third parties. You cannot honestly spec the
external SPI until core has dogfooded that machinery, which means until the noun
seam and `Device.supports(...)` actually exist. Ray-query-to-core is what forces
the noun seam into being (see the foundation plan §3.3). Speccing it before core
uses it would be designing in a vacuum, which is the exact "fiction-as-contract"
failure this project is trying to avoid. The decided shape is recorded here now,
and the concrete SPI follows when core dogfoods the seams.

---

## What an extension library is

An extension library is a signed dependency, distributed from `olla.cajeta.dev`,
that a developer adds to a project explicitly. Importing it is the lock-in
declaration (`import cajeta.xpu.nvidia.*` = "this code may require NVIDIA"). It
is no part of stdlib, because stdlib ships core only.

An extension contributes two kinds of thing, matching core's two seams:

### 1. Verb extensions (the easy half)
A verb extension is a vendor-exclusive operation. The author declares:
- the **verb signature** (the cajeta-facing method),
- its **native lowering** on the target backend (the driver op / intrinsic),
- a **vendor-authored SPIR-V degrade**, which is the portable fallback algorithm.

The degrade is the load-bearing idea. SPIR-V is the vehicle, and it is no magic.
A vendor-exclusive op (TMA, an NV-shape `mma`) has no SPIR-V equivalent, so its
degrade is the slower portable algorithm the author writes and emits through
SPIR-V. That lets the extension run on a competing GPU or fall to CPU instead of
hard-failing. Authors that supply no degrade get the hard-requirement behavior
(§ Degradation).

### 2. Noun extensions (the hard half)
A noun extension is a vendor-specific datastructure representation, behind the
noun seam. Per [`CajetaGPU.md §1.4`](CajetaGPU.md), the rule is
build-from-description rather than convert-between-builts. The author provides a
representation, a builder that consumes core's build description (e.g. the
ray-query geometry inputs), and the verbs that consume that representation. The
built artifact is opaque, and core never transcodes between representations.

> The noun's chosen implementation determines the verb's lowering, selected once at build time
> by the capability heuristic. An extension that adds a noun representation also owns its verbs.

---

## Degradation & selection (decided)

- **Core always runs** (floors to CPU). An extension never silently emulates.
  Absent silicon yields a diagnostic / no-device unless the author supplied a
  degrade.
- An app composes graceful fallback explicitly, as a guarded vendor branch beside
  a core branch.
  ```cajeta
  if (Device.supports(Capability.X)) { /* extension fast path */ }
  else { /* cajeta.xpu — floors to CPU */ }
  ```
- **Selection** is a capability heuristic with an explicit override (default ≠ law).

---

## Distribution & trust (decided shape; the real work)

- **`olla.cajeta.dev`** is the package registry (the maven-for-cajeta), with
  signed libraries as the primary distribution.
- **AOT** links the chosen impl layers, and the app selects among the compiled-in
  set at runtime. **JIT (`.cja`)** uses whatever is on the classpath, with
  `core` + `cpu` always built in as the floor.
- **Trust/security model** is the open cost, and it is not yet designed. An
  extension emits LLVM IR / SPIR-V into a user's module. That is a real attack
  and stability surface. It needs signed-only install, a sandbox / capability
  boundary for what an extension may emit, a versioned and stable SPI contract,
  and a stated threat model. This is the part that makes the SDK a project
  rather than a paragraph.

---

## Dependencies (what must exist before this crystallizes)

From the foundation plan §1:
- ✅ the **noun seam** as a first-class SPI (ray-query-to-core forced it). It is
  built as `CajetaNounProvider` and dogfooded on `AccelerationStructure`
  (recorded impl tag; the verb follows the noun). This is the noun half of the
  machinery this SDK will expose.
- ✅ **`Device.supports(...)`** plus the explicit impl override and execution
  mechanism (an `AsImpl` preference / `CAJETA_GPU_AS_IMPL` env override, and a
  per-impl kernel variant so a forced choice actually runs). These are the "impl
  layers" the degrade framework will generalize. The automatic density/extent
  heuristic is still to come,
- ✅ the **internal impl-layer + degrade seam** (the verb half, dogfooded). Core
  now has one named degrade concept, `LoweringTarget::ImplTier { Native,
  Portable }`, that both core degrade features answer through (the coop-matrix
  verb `coopMatrixTier` and the ray-query verb `rayQueryTier`, derived from the
  AS noun's `NounImpl`), plus a generic `CAJETA_GPU_<FEATURE>_IMPL` override
  (`resolveImplTier`) proven on a second consumer beyond the AS noun
  (`CAJETA_GPU_COOPMATRIX_IMPL=software` forces the portable tile on a
  native-capable device, device-verified). This is the in-tree machinery this
  SDK will expose. The external declaration syntax (verb + lowering + degrade),
  AOT/JIT impl-layer packaging, and signing/sandbox remain the seed (below). The
  honest boundary is that Native and Portable are different realizations, so an
  arithmetic feature's tiers are each validated against the reference and never
  against each other,
- ✅ the `cajeta.xpu.core → cajeta.xpu` rename.

When the remaining pieces land and core has used them, this seed becomes the
SDK's real spec.

---

## Open questions (carry forward)

- The exact **declaration syntax** for a verb's native-lowering + degrade, and a
  noun's representation + builder. Do not invent it here; let core's own use
  settle it.
- The **SPIR-V degrade boundary**, meaning what an author can assume the vehicle
  provides and what they must author themselves.
- The **sandbox model** for third-party IR/SPIR-V emission, and the
  signing/verification chain.
- Versioning: how an extension declares the SPI version it targets and how
  breakage is handled.

---

## See also

- Foundation contract — [`CajetaGPU.md`](CajetaGPU.md) (esp. §1 the model, §4 the noun seam).
- Foundation plan — `../../plans/gpu/cajeta-gpu-plan.md`
  (§1 the framework task).
