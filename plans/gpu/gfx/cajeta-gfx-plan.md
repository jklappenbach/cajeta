# Cajeta GFX — graphics spec

**`cajeta-gfx` is the graphics/rendering layer** — the Vulkan **graphics** pipeline
(vertex → rasterizer → fragment), render passes, swapchain, and the game engine built
on top. It is a **sibling of `cajeta-xpu`**, not a child: both depend on the shared
`cajeta-gpu` foundation, and **gfx does not depend on xpu** (graphics carries none of
the compute kernel/dispatch/wave specifics).

```
cajeta-gpu        (foundation — value types, math, textures, device/codegen/memory)
   ▲                       ▲
   │ depends on            │ depends on
cajeta-xpu             cajeta-gfx   ← this spec
(compute)              (graphics + engine)
   ✗ gfx does NOT depend on xpu ✗
```

**Status: bring-up started (G1.0 increment 0 — emitter feasibility — done).**
Today the Vulkan backend is **compute-only** (`GLCompute` entry points,
`vkCmdDispatch`); there is no graphics pipeline. This spec is the forward plan to
add one and build an engine on it. It reuses the `cajeta-gpu` foundation wholesale
— `Vector`/`Matrix`/`Quaternion`, math intrinsics, textures, the SPIR-V emitter,
the device/driver layer, the memory/buffer model — and adds the graphics-specific
execution model the foundation doesn't have.

**The gating question is answered: the existing in-tree LLVM SPIR-V backend can
emit valid graphics shaders — no glslang / SPIRV-Tools assembler needed.** Proven
by `test/gfx/GfxSpirvEmitProbeTests.cpp` (3 tests, green; binaries `spirv-val`'d
against Vulkan 1.3). The graphics seam vs the compute path is exactly three things:
1. **Execution model** — `hlsl.shader="vertex"|"pixel"` **plus a per-stage triple
   environment** (`spirv-unknown-vulkan1.3-{vertex,pixel}`) → `OpEntryPoint
   Vertex`/`Fragment`. Because the stage rides the *triple*, **each shader stage is
   its own module / `.spv`** — which is how Vulkan binds stages anyway.
   (`createSpirvTargetMachine` hardcodes the `-compute` triple today; the graphics
   path needs a per-stage triple knob.)
2. **Shader I/O** — interface variables are module globals in **addrspace 7
   (Input) / addrspace 8 (Output)** carrying `!spirv.Decorations` metadata:
   `{i32 11, i32 0}`=BuiltIn Position, `{i32 11, i32 15}`=BuiltIn FragCoord,
   `{i32 30, i32 N}`=Location N. (Compute has *no* interface vars — args arrive via
   descriptors. This is the real new lowering surface.)
3. **Emit path is reusable as-is** — `emitSpirvText` runs no compute-specific
   post-passes; the binary's LocalSize/barrier fixups are documented no-ops when
   absent. Graphics modules flow through unchanged.

Checkbox legend: `[x]` landed+tested · `[~]` partial · `[ ]` not started.
**Working agreement:** one increment at a time, tests + docs + commit checkpoint;
golden-image tests for the renderer; never ship a broken frame path silently; commit
only when asked; **no attribution trailer**; stage files explicitly.

---

## Part G1 — Graphics device (Vulkan rendering pipeline)

The core gap vs the compute substrate: a *graphics* pipeline alongside the compute one,
reusing the same SPIR-V backend and device layer.

### Stage G1.0 — Shader stages → SPIR-V
- [x] **Increment 0 — emitter feasibility probe** (`test/gfx/GfxSpirvEmitProbeTests.cpp`): the in-tree SPIR-V backend emits `spirv-val`-clean Vertex + Fragment modules (execution model + Input/Output interface vars via `spirv.Decorations`). Confirms the whole stage is buildable on the existing emitter — no external assembler. Findings recorded in the status section above.
- [ ] **Increment 1 — per-stage TargetMachine knob**: parameterize `createSpirvTargetMachine` / `configureDeviceModule` (or add a graphics sibling) on the shader stage so the triple env is `-vertex`/`-pixel` and the function gets the right `hlsl.shader` value. One module/`.spv` per stage.
- [ ] **Increment 2 — `@Vertex` / `@Fragment` annotation surface**: parallel to `@Kernel` (extend `XpuAttr` or add a `gfx` attribute set); recognition + validation that a graphics shader has the right shape (entry returns/writes the stage's required builtins).
- [ ] **Increment 3 — interface-variable lowering**: a `SpirvGraphicsTarget` (sibling of `SpirvTarget`) that lowers shader inputs/outputs to addrspace 7/8 globals with `Location`/`BuiltIn` decorations — vertex attributes, `gl_Position`, varyings, fragment color out. This is the genuinely new lowering surface (compute has none).
- [ ] **Increment 4 — uniform/storage buffers + push constants** for shader params (reuse the descriptor-set model from `cajeta-gpu`)
- [ ] (later) geometry / tessellation stages; the triple/attr knob already generalizes

### Stage G1.1 — Pipeline & presentation
- [ ] Swapchain creation + present; surface/format/colorspace selection
- [ ] Render passes / framebuffers (or dynamic rendering); attachments
- [ ] Graphics pipeline state: vertex input, input assembly, raster, blend, depth/stencil, viewport/scissor
- [ ] Command-buffer recording for draws; frames-in-flight; sync (semaphores/fences)
- [ ] `vkCmdDraw`/`vkCmdDrawIndexed`; vertex/index buffer binding

### Stage G1.2 — Other backends (optional, later)
- [ ] DX12 / Metal / WebGPU via the same `LoweringTarget`-style seam pattern that compute uses
- [ ] **Apple/macOS graphics:** Tier 1 = **MoltenVK** (Vulkan→Metal; mature for graphics — how most macOS games ship) reuses this Vulkan rendering path as-is; Tier 2 = native **Metal** (mesh shaders, Metal RT pipeline, MetalFX). Shared strategy/sequencing lives in `cajeta-gpu-plan.md` § Platforms — Apple/macOS (the `metal` backend seam is gpu-foundation).

---

## Part G2 — Windowing, input, platform

### Stage G2.0 — Platform surface
- [ ] Window/surface creation — Linux/Hyprland first (matches the dev box)
- [ ] Input events — keyboard/mouse/gamepad; event loop
- [ ] Display/monitor config, vsync, fullscreen

---

## Part G3 — Renderer

### Stage G3.0 — Render graph
- [ ] Pass graph with auto-derived resource barriers (can reuse the XPU-launch lowering machinery for compute passes that participate in rendering, but the graph itself is graphics)
- [ ] Transient resource allocation / aliasing

### Stage G3.1 — Render paths & shading
- [ ] Forward + deferred paths; PBR material model
- [ ] Lighting (directional/point/spot), shadows, ambient/IBL
- [ ] Post-processing (tonemap, bloom, FXAA/TAA, SSAO)
- [ ] Camera, frustum culling, LOD; instancing & batching

### Stage G3.2 — 2-D renderer
- [ ] Sprites, text, UI (also serves data-viz consumers — e.g. the ETE tree renderer from the ML ports)

### Stage G3.3 — Hardware ray tracing (rendering pipeline)
The **full RT pipeline** — distinct from the **ray query** primitive, which is
foundation-level and lives in `cajeta-gpu-plan.md` Part C (compute-callable, also used
here for inline RT: ray-traced shadows / AO / reflections inside fragment & compute
passes). This stage is the *rendering* shading model only.
- [ ] RT pipeline shader stages: raygen / closest-hit / miss / any-hit / intersection / callable execution models (extends the G1.0 per-stage triple + `hlsl.shader` knob)
- [ ] Ray-payload & hit-attribute storage classes; `OpTraceRayKHR`; shader binding table (SBT) runtime
- [ ] Path tracing / RT reflections / RT GI render paths built on the above
- **LLVM backend dependency:** these opcodes/execution-models are **Tier-3 absent in LLVM today** (only NV enum stubs) — they ride the same downstream-fork + prebuilt-artifact pipeline as `cajeta-gpu` Part C, but the lowering itself is the *bigger* patch (new execution models + payload storage classes + `OpTraceRayKHR` + SBT), and is **deferred behind ray query** (the smaller, shared, science-first primitive).

---

## Part G4 — Scene, content & assets

### Stage G4.0 — Asset pipeline
- [ ] Asset import (textures, meshes, shaders, audio) → engine-native formats
- [ ] Virtual filesystem / package mounting; streaming; serialization/versioning
- [ ] Content build step integrated with `build.sh`

### Stage G4.1 — Scene model
- [ ] ECS (entity-component-system) — data-oriented, parallel-friendly over a job system
- [ ] Scene graph / transform hierarchy; spatial partitioning (BVH/octree — reuse `gpu-utils` collision)
- [ ] glTF/FBX import; skeletal animation + skinning (GPU)
- [ ] Particle systems (GPU compute → render — one place gfx *uses* an xpu-style compute pass, via `cajeta-gpu`, without depending on the xpu spec)

---

## Part G5 — Physics & audio

### Stage G5.0 — Physics
- [ ] Rigid-body (broad/narrow phase — reuse `gpu-utils` collision-detection)
- [ ] Collision queries, raycasts, character controller; constraint solver

### Stage G5.1 — Audio
- [ ] Mixing, spatial/3-D, streaming

---

## Part G6 — Tooling & editor

- [ ] Hot-reload of assets + Cajeta gameplay code
- [ ] In-engine profiler / frame debugger (GPU timings via the render graph)
- [ ] Scene editor (built on the engine's own 2-D/UI renderer)
- [ ] Build/packaging for distributable game binaries (leverages the runtime dispatcher's "run anywhere")

---

## Dependency notes & sequencing

- **Blocked on `cajeta-gpu`.** G1.0 needs the SPIR-V emitter, value types (Matrix/
  Quaternion for transforms), and textures — all `cajeta-gpu`. Don't start the graphics
  pipeline before the foundation's value-type + texture + math stages (gpu B1–B3) land.
- **The gpu/xpu code split should be sequenced with G1.0.** Building the *second*
  consumer of the foundation (graphics) is what validates the shared seam — so the
  physical `cajeta.gpu.core.*` → `cajeta.gpu.core.*` package + `__cajeta_xpu_*` ABI
  rename (see the refactor-strategy note in `cajeta-gpu-plan.md`) is best done here,
  as the first graphics work proves which symbols are truly shared.
- **gfx never imports xpu.** Where graphics wants a compute pass (particles, skinning,
  culling), it uses the `cajeta-gpu` device/dispatch primitives directly — it does not
  take a dependency on the `cajeta-xpu` compute spec. This keeps the engine from
  hauling around the numerical-compute execution model.

---

*Graphics is a sibling of compute over a shared foundation, not a layer on top of it.
This spec is entirely forward work; it begins once `cajeta-gpu`'s value-type, math, and
texture stages are in place.*
