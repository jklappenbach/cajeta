# Cajeta GFX — the graphics layer

`cajeta-gfx` is the **graphics / rendering layer** — the Vulkan **graphics** pipeline
(vertex → rasterizer → fragment), render passes, swapchain, and the game engine built
on top. It is a **sibling of `cajeta-xpu`**, not a child: both depend on the shared
`cajeta-gpu` foundation, and **gfx does not depend on xpu** (graphics carries none of
the compute kernel/dispatch/wave specifics).

```
cajeta-gpu        (foundation — value types, math, textures, device/codegen/memory)
   ▲                       ▲
   │ depends on            │ depends on
cajeta-xpu             cajeta-gfx   ← this layer
(compute)              (graphics + engine)
   ✗ gfx does NOT depend on xpu ✗
```

This doc is the **map** of the graphics layer. The detailed `cajeta.render` design
spec is [`CajetaRender.md`](CajetaRender.md); the authoritative forward plan is
[`plans/gpu/gfx/cajeta-gfx-plan.md`](../../../plans/gpu/gfx/cajeta-gfx-plan.md).

---

## Status: bring-up started, mostly forward work

Today the Vulkan backend is **compute-only** (`GLCompute` entry points,
`vkCmdDispatch`); there is **no graphics pipeline yet**. The gating question is
**answered**: the existing in-tree LLVM SPIR-V backend can emit valid graphics shaders
— no `glslang`/SPIRV-Tools assembler needed. Proven by
`test/gfx/GfxSpirvEmitProbeTests.cpp` (Vertex + Fragment modules, `spirv-val`-clean
against Vulkan 1.3).

`cajeta-gfx` reuses the `cajeta-gpu` foundation wholesale —
`Vector`/`Matrix`/`Quaternion`, math intrinsics, textures, the SPIR-V emitter, the
device/driver layer, the memory/buffer model — and adds the graphics-specific
execution model the foundation doesn't have.

### What the graphics seam adds over the compute path

1. **Execution model** — `hlsl.shader="vertex"|"pixel"` **plus a per-stage triple
   environment** (`spirv-unknown-vulkan1.3-{vertex,pixel}`) → `OpEntryPoint
   Vertex`/`Fragment`. Each shader stage is its own module / `.spv` (which is how
   Vulkan binds stages anyway).
2. **Shader I/O** — interface variables as module globals in **addrspace 7 (Input) /
   addrspace 8 (Output)** carrying `!spirv.Decorations` (BuiltIn Position/FragCoord,
   Location N). Compute has *no* interface vars — this is the genuinely new lowering
   surface.
3. **Emit path is reusable as-is** — graphics modules flow through `emitSpirvText`
   unchanged.

---

## The shared bridge from the foundation

One foundation capability sits directly under graphics and is worth calling out: the
writable / storage image. **`Image2D` `imageStore`** (the writable twin of the
foundation `Texture2D`) is owned by `cajeta-gpu` but is the bridge a renderer writes
through — see [`../WritableImages.md`](../WritableImages.md). Where graphics wants a
*compute* pass (particles, skinning, culling) it uses the `cajeta-gpu`
device/dispatch primitives directly — it does **not** take a dependency on the
`cajeta-xpu` compute spec.

---

## Roadmap (from the plan)

Forward work, sequenced after the foundation's value-type + math + texture stages:

- **G1 — Graphics device:** shader stages → SPIR-V (per-stage TargetMachine knob,
  `@Vertex`/`@Fragment` annotation surface, interface-variable lowering, uniform/storage
  buffers + push constants); then pipeline & presentation (swapchain, render passes,
  pipeline state, command buffers, `vkCmdDraw`).
- **G2 — Windowing, input, platform** (Linux/Hyprland first).
- **G3 — Renderer:** render graph, forward/deferred + PBR, lighting/shadows,
  post-processing, a 2-D/UI renderer, and the **RT rendering pipeline** (distinct from
  the foundation-level ray-query primitive — deferred behind it).
- **G4 — Scene, content & assets:** ECS, scene graph, glTF/FBX import, particles.
- **G5 — Physics & audio.** **G6 — Tooling & editor.**

**Apple/macOS:** Tier 1 = **MoltenVK** (Vulkan→Metal) reuses this Vulkan rendering path
as-is; Tier 2 = native Metal. The `metal` backend seam is gpu-foundation.

---

## See also

- [`CajetaRender.md`](CajetaRender.md) — the detailed `cajeta.render` design spec.
- [`../CajetaGPU.md`](../CajetaGPU.md) — the foundation this builds on.
- [`../xpu/CajetaXPU.md`](../xpu/CajetaXPU.md) — the compute sibling.
- [`plans/gpu/gfx/cajeta-gfx-plan.md`](../../../plans/gpu/gfx/cajeta-gfx-plan.md) — the
  forward plan.
