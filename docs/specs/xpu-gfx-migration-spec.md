# Stdlib `gpu` → `xpu` / `gfx` Migration — Specification

> Status: draft for review (2026-06-24). A **refactoring** spec: it renames/splits the stdlib
> `cajeta.gpu` package into `cajeta.xpu` (compute) and `cajeta.gfx` (graphics). Behaviour is
> unchanged; only package identity and file layout move. Outline-numbered for addressability.
> Companion plan: `agents/cajeta/xpu-gfx-migration-plan.md`. Decision memory:
> `stdlib-xpu-gfx-split`.

## 1. Definition

### 1.1 Purpose
The stdlib `cajeta.gpu` package conflates two layers: a **multi-target compute/kernel
substrate** (which lowers via the `src/cajeta/xpu` backend to CPU / NVPTX / AMDGPU / SPIR-V)
and a **graphics/rendering pipeline**. This migration splits it into two packages and **retires
the `gpu` name**:
- **`cajeta.xpu`** — *all compute and cross-domain primitives*: the kernel execution substrate,
  every compute algorithm that runs as a kernel (cooperative matrix, ray/acceleration, sampling,
  mesh/SDF), and broadly-useful resources/encodings (compute storage images, unit-vector
  encoding, residency caches). It is the home for anything datascience / ML / scientific
  computing would also want — not just graphics.
- **`cajeta.gfx`** — *graphical-engine primitives only*: sampled textures & samplers and the
  render-pipeline state (swapchain, render graph, pipeline config, frame sync). `gfx` depends on
  `xpu` (it consumes xpu compute, mesh, and images), never the reverse.

### 1.2 Scope
- Rename/move the ~55 `.cajeta` classes under `runtime/src/cajeta/gpu/` into
  `runtime/src/cajeta/xpu/` and `runtime/src/cajeta/gfx/` per §3, collapsing the inverted
  `gpu.xpu` and `gpu.gfx` subpackages up to the new top-level packages.
- Update every consumer: C++ compiler package-name string literals, the prelude/auto-import
  list, runtime native comments, the test suites, the language tour, and docs/skills.
- Keep the build green and behaviour identical at every committed step (§4).

### 1.3 Non-goals
- **No behaviour, ABI, or codegen change.** This is a pure rename/relocation; kernels lower
  identically and tests assert the same results.
- **No new sub-package taxonomy.** `cajeta.xpu` and `cajeta.gfx` are **flat** (e.g.
  `cajeta.xpu.Bvh`, `cajeta.gfx.Swapchain`); optional sub-packaging (`xpu.ray`/`xpu.sample`/
  `xpu.mesh`) is deferred (§8).
- **No API additions or class removals.** Class names are unchanged; only their package moves.
- **No change to the C++ backend namespace `cajeta::xpu`** (`src/cajeta/xpu/`) — it already
  carries that name and does not collide with the new stdlib package `cajeta.xpu` (one is C++
  `::`, the other a `.cajeta` package).

### 1.4 The boundary principle (err toward `xpu`)
The classifying rule, decided 2026-06-24 (refined to **err toward `xpu`**):
> **Default to `cajeta.xpu`.** Put a class in `cajeta.gfx` **only** if it is a primitive whose
> distinctive use is a *graphical engine* — i.e. the **sampled-texture surface** (textures +
> samplers + texture formats) and the **render pipeline** (swapchain, render graph, pipeline
> config, frame sync). *Everything else is `xpu`*: the kernel substrate, cooperative matrix,
> ray/acceleration, sampling, **mesh/SDF**, and any broadly-useful resource or encoding (compute
> storage images, octahedral encoding, residency caches) — because datascience / ML / scientific
> computing consume those too, not only rendering.
>
> There is a **huge overlap** between compute and graphics concerns; the tie-breaker is *breadth
> of use*. `gfx` will *use* mesh, images, and accel — it imports them from `xpu`.

Consequence (§5.3): work authored as "cajeta-gfx §3" (accel / sampling / mesh / encoding) is
**compute** under this rule and moves to `cajeta.xpu` and `test/xpu/`; only the sampled-texture
surface and the render pipeline stay in `cajeta.gfx`.

## 2. Why now / context
- The decision is already partly wired: the prelude list (`Resolver.cpp:47`) **already contains
  `"cajeta.xpu"`** and lacks `"cajeta.gpu"`; the C++ backend dirs are already `src/cajeta/xpu/`.
- The `gpu` package is **actively growing** — the 2026-06-24 pull added 9 compute classes
  (`Bvh`, `Lbvh`, `BvhCodec`, `Strategy`, `Sobol`, `Reservoir`, `Ris`, `Octahedral`,
  `PageCache`) — so the change-surface enlarges with every landing. Splitting now caps it.
- núcleo and cajeta-robotica both want to import the **compute** layer (`cajeta.xpu`, esp.
  `CooperativeMatrix` for matmul) without dragging in the graphics pipeline; the split makes
  that DCE-clean.

## 3. Authoritative class → package classification

**Build target `cajeta.xpu` (compute + cross-domain)** — from `runtime/src/cajeta/gpu/` (+
promoted `gpu/xpu/`):

- **3.1 Kernel substrate:** `Device`, `Capabilities`, `Capability`, `KernelArg`,
  `KernelBuffer`, `KernelStream`, `KernelThread`, `KernelError`, `Barrier`, `Fence`, `Event`,
  `Workgroup`, `Wave`, `Quad`, `AddressSpace` (incl. the `Global`/`Shared`/`Constant`/`Private`/
  `Generic` markers), `MemoryKind`, `MemoryOrder`, `Bits`.
- **3.2 Cooperative matrix (promoted from `gpu.xpu`):** `CooperativeMatrix`, `CoopStage`.
  *(núcleo's matmul lowers onto these — the key reason the compute layer is importable without
  graphics.)*
- **3.3 Ray / acceleration:** `RayQuery`, `SoftwareRayQuery`, `SwRayCursor`,
  `AccelerationStructure`, `AsImpl`, `Bvh`, `Lbvh`, `BvhCodec`, `Strategy`.
- **3.4 Sampling (Monte-Carlo / low-discrepancy):** `Rng`, `Noise`, `LowDiscrepancy`, `Sobol`,
  `Reservoir`, `Ris`.
- **3.5 Mesh / SDF geometry:** `Sdf`, `Tsdf`, `Qem`, `MarchingTetrahedra`, `MeshSimplifier`.
  *(Cross-domain: consumed by `gfx` **and** datascience / ML / scientific computing; `xpu` is the
  home, `gfx` imports it.)*
- **3.6 Compute resources & generic primitives:** `Image2D` (compute storage image — read/write
  in kernels, distinct from a sampled texture), `Octahedral` (unit-vector ↔ 2-channel encoding —
  generic normal/direction compression), `PageCache` (generic feedback-driven LRU residency
  cache; reuses `cajeta.collection.Cache`).

**Build target `cajeta.gfx` (graphical-engine primitives only)** — from
`runtime/src/cajeta/gpu/` (+ promoted `gpu/gfx/`):

- **3.7 Sampled textures & samplers:** `Texture1D`, `Texture2D`, `Texture3D`, `Texture2DArray`,
  `TextureCube`, `TextureFormat`, `Sampler`.
- **3.8 Render pipeline (promoted from `gpu.gfx`):** `Swapchain`, `SwapchainConfig`,
  `RenderGraph`, `PipelineConfig`, `FrameSync`.

> **Resolved (err toward `xpu`, §1.4):** the 9 newly-landed classes are: accel `Bvh`/`Lbvh`/
> `BvhCodec`/`Strategy` → **xpu** (§3.3); sampling `Sobol`/`Reservoir`/`Ris` → **xpu** (§3.4);
> `Octahedral`/`PageCache` → **xpu** (§3.6, generic/broadly-useful). Only sampled textures and
> the render pipeline remain `gfx`. So `gfx` is **13 classes** (§3.7–3.8) and `xpu` is the
> remaining **~42**.
>
> **Judgment calls to confirm:** `Image2D` is placed in `xpu` as a *compute storage image*; if it
> is in fact a sampled/presentation surface it belongs in `gfx` (§8). `Sampler`/`TextureFormat`
> ride with textures in `gfx`; if compute texture-sampling needs them independently they could
> move to `xpu`.

## 4. Migration invariants (use cases)

- **4.1** As the maintainer, when each unit (§plan) is committed, then `cmake` configures and the
  toolchain builds (the stdlib is glob-discovered — `CMakeLists.txt:607` `GLOB_RECURSE …
  CONFIGURE_DEPENDS` — so a directory move auto-rebuilds; no manifest edit needed).
- **4.2** As the maintainer, when a unit lands, then the affected test suites pass with the
  **same assertions** (rename-only; no result changes).
- **4.3** As a developer, when I reference a moved class, then `cajeta.gpu.*` no longer resolves
  and `cajeta.xpu.*` / `cajeta.gfx.*` does; there is **no compatibility alias** kept (fail-loud
  on stale imports rather than a silent shim).
- **4.4** As the prelude, when the toolchain initializes, then `"cajeta.gfx"` is in the
  stdlib-roots list (`Resolver.cpp`) and `"cajeta.xpu"` remains; `"cajeta.gpu"` is absent.
- **4.5** As `cajeta.gfx`, when it imports compute primitives (e.g. `Device`, `KernelBuffer`),
  then it imports them from `cajeta.xpu` — the dependency runs gfx → xpu, never the reverse.

## 5. Mechanical change surface

- **5.1 Build / prelude.** Glob-driven build (no manifest); add `"cajeta.gfx"` to
  `src/cajeta/buildtool/Resolver.cpp:47` (kStdlibRoots).
- **5.2 C++ compiler string literals (~60 + ~28).** Package-qualified type-lookup strings keyed
  on `"cajeta.gpu.*"`: `Method.cpp`, `asn/expression/CallExpression.cpp`,
  `asn/expression/MethodCallExpression.cpp`, `xpu/lowering/KernelLowering.cpp`,
  `xpu/core/KernelArgTrait.cpp` (the `kPrefix` constants for `KernelBuffer`/`Texture2D`/…),
  `xpu/core/AddressSpace.h` (`if (pkg != "cajeta.gpu")` → `"cajeta.xpu"`), `type/CajetaType.*`
  (comments). Each string routes to `cajeta.xpu.*` or `cajeta.gfx.*` per §3.
  > Note: backend error labels already read `"cajeta.xpu.{vulkan,cpu,amd,nvidia}"` — **no change**.
- **5.3 Tests.** Compute test files currently under `test/gfx/` move to `test/xpu/` and take the
  `Xpu*` name: `Gfx{BvhCodec,Lbvh,AccelContract,Sobol,Reservoir,LowDiscrepancy,Noise,Rng,Sdf,
  Octahedral,PageCache}Tests` → `Xpu*`; `test/gpu/` tests (`Gpu{MeshSimplifier,Qem,Tsdf,
  MarchingTetrahedra,RayQueryProbe}Tests`) move to `test/xpu/`.
  Graphics tests stay under `test/gfx/` (`Gfx{FrameSync,PipelineConfig,RenderGraph,
  SwapchainConfig,PushConstant,MultiOutput,Spirv*,Graphics*}Tests`).
  Pure-math tests (`Gfx{Camera,Color,Frustum,Geometry,Rotation,Transform}Tests`) are
  out of scope (they test `cajeta.math`, not `cajeta.gpu`) — confirm by their imports.
  All affected tests' `import cajeta.gpu.*` strings update per §3 (~1160 import lines/68 files).
- **5.4 Runtime native.** Comment-only `cajeta.gpu` refs in `runtime/native/cajeta_runtime.c`
  and `cajeta_bvh.c`; one string `"cajeta.gpu.gfx.FrameSync"` → `"cajeta.gfx.FrameSync"`.
- **5.5 Tour.** Split `samples/tour/gpu/` (package `tour.gpu`) into compute (`tour.xpu`:
  saxpy/vecAdd/wave/bits/quad) and graphics (`tour.gfx`: textures/sampler), updating imports to
  `cajeta.xpu.*` / `cajeta.gfx.*`; update `run-gpu.sh`/README accordingly. Done atomically so the
  tour stays buildable.
- **5.6 Docs / skills.** Prose `cajeta.gpu` refs in `docs/*.md` and the `runtime/src/cajeta/gpu/
  skills/*.md` (14 files) move with their package; consider `gpu-*.md` → `xpu-*.md`/`gfx-*.md`.

## 6. Acceptance criteria (spec-level)
- `cajeta.gpu` resolves nowhere; every former member resolves under `cajeta.xpu` or `cajeta.gfx`
  per §3, and the prelude lists both (not `gpu`).
- The full test sweep passes with unchanged assertions; the GPU tour builds and runs on the CPU
  backend; a GPU-backend build still lowers kernels identically.
- `cajeta.gfx` imports compute from `cajeta.xpu`; no `cajeta.xpu` file imports `cajeta.gfx`.
- No `cajeta.gpu` string literals remain in C++/runtime/tests/docs (a repo-wide grep is clean).

## 7. Deliverables
- `runtime/src/cajeta/xpu/` and `runtime/src/cajeta/gfx/` populated per §3; `runtime/src/cajeta/
  gpu/` removed.
- Updated compiler/runtime/prelude/tests/tour/docs per §5.
- A green full test sweep + a built CPU-backend tour as evidence.

## 8. Open questions (resolve at plan time)
- **`Image2D` / `Sampler` / `TextureFormat` placement** (the §3 judgment calls). `Image2D` is in
  `xpu` as a compute storage image; `Sampler`/`TextureFormat` are in `gfx` with textures. Confirm
  against the actual class semantics before the move — these are the only genuinely-straddling
  members under the err-toward-`xpu` rule.
- **Flat vs sub-packaged `cajeta.xpu`.** v1 is flat (§1.3). Whether to later group
  `xpu.ray` / `xpu.sample` / `xpu.mesh` is deferred — it adds import churn for marginal
  organization; revisit once the flat package settles.
- **`gpu-*.md` skill renames.** Whether to rename the 14 skill docs' filenames (`gpu-buffer.md`
  → `xpu-buffer.md`) or only their content — cosmetic; lean rename for consistency.
- **One commit per target vs finer.** The rename is largely atomic per package; the plan
  proposes xpu-first, gfx-second, tour+docs-third (§ plan), each green.
