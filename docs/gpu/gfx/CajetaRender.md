# CajetaRender — Graphics Pipelines

> **Status: forward design spec — not yet implemented.** This is the detailed
> design for `cajeta.render`; none of the `cajeta.render.*` packages, the
> `@vertex`/`@fragment`/`@mesh`/`@ray_*` stage attributes, or the pipeline/scene/
> material APIs below exist in the codebase yet. The graphics layer is at bring-up:
> the gating proof (the in-tree LLVM SPIR-V backend can emit valid Vertex/Fragment
> modules) is done — see [`CajetaGFX.md`](CajetaGFX.md) for the current status and
> the [`cajeta-gfx-plan.md`](../../../plans/gpu/gfx/cajeta-gfx-plan.md) roadmap (G1–G6,
> all forward work). The code blocks here use illustrative pseudo-syntax (`fn`, `let`,
> `var`, lowercase `@vertex`) and are design intent, not the shipped surface; the
> implemented compute surface it builds on is described in
> [`CajetaXPU.md`](../xpu/CajetaXPU.md) and [`CajetaXPU-Matrix.md`](../xpu/CajetaXPU-Matrix.md).

This document specifies `cajeta.render`, Cajeta's graphics layer:
rasterization, ray tracing, mesh shading, and the scene-graph + asset
shape that feeds them. The library sits on top of
[`cajeta.xpu`](../xpu/CajetaXPU.md) — every dispatch ultimately lands in
`cajeta.xpu.vulkan` (portable cross-vendor path) or in vendor-native
backends (`cajeta.xpu.nvidia` for RT cores, `cajeta.xpu.amd` for RDNA
ray accelerators) when the project opts into hardware-specific
acceleration.

Render is a separate library from XPU on purpose. XPU is a substrate
— compute kernels, memory, dispatch. Render is the *application* of
that substrate to drawing pixels: vertex / fragment / mesh / ray
pipelines, materials, samplers, framebuffers, render passes, swap
chains. Mixing the two muddies both. Anyone writing a compute kernel
for ML doesn't need the render surface; anyone writing a renderer
doesn't need the BLAS bindings under `cajeta.xpu.nvidia.cublas`.

---

## 1. Goals and non-goals

### 1.1 Goals

1. **One renderer, every vendor.** The same `cajeta.render` code
   produces a binary that runs on NVIDIA, AMD, Intel Arc, Apple (via
   MoltenVK), Android, and SteamDeck. Vulkan is the load-bearing path.
2. **Raster and ray as peers.** Modern engines mix rasterization and
   ray tracing in one frame (raster G-buffer + ray-traced shadows /
   reflections / GI). `cajeta.render` exposes both as first-class
   pipelines, not raster-then-bolted-on-ray.
3. **Hardware ray-tracing acceleration where available.** RT cores
   (NVIDIA Turing+), RDNA hardware RT (AMD RDNA2+), Apple Metal RT —
   reached through the appropriate XPU backend. Software fallback for
   targets without dedicated RT silicon.
4. **A scene graph that doesn't force a paradigm.** Cajeta apps build
   scenes the way they like: hand-rolled draw loops, ECS-driven,
   render-graph-declarative. The render library offers all three
   styles; users pick.
5. **Game-engine-grade asset path.** glTF in, mesh / texture / material
   / animation out. EXR for HDR. KTX2 for compressed textures.
   Skeletal animation via dual-quaternion skinning. Animation
   compression via cubic-spline curves.
6. **Borrow-checker correctness across frames.** A texture in use by
   the GPU through the end of frame N cannot be freed in the middle
   of frame N. The borrow checker treats render-pass scopes as
   deferred borrows the same way XPU does (per
   [`CajetaXPU.md`](../xpu/CajetaXPU.md#11-borrow-checker-interaction-notable-cases)).

### 1.2 Non-goals

- **No proprietary-API native backend.** No DirectX 12, no Metal-
  direct, no PS5 GNM. Vulkan is the lingua franca — including MoltenVK
  on Apple. A Metal-direct backend could land later but is out of v1.
- **No game-engine systems.** Animation, physics, ECS, scripting,
  input — all of those are out of scope for `cajeta.render`. The
  renderer is the rendering surface; an engine sits one layer up.
- **No managed-scene scripting language.** Materials are written in
  Cajeta. Behavior is written in Cajeta. There is no embedded shading
  language or material graph DSL in v1 — the language is the language.
- **No editor / authoring tools.** The asset path imports; the editor
  is an independent project.

---

## 2. Architecture at a glance

```
                Cajeta source (.cajeta)
                          |
                          v
        +--------------------------------+
        |  Frontend: lex / parse / type  |
        |  check / borrow check / mono   |
        +--------------------------------+
                          |
                          v
        +--------------------------------+
        |  cajeta.render.pipeline        |
        |  - graphics-pipeline MIR       |
        |  - vertex/fragment/mesh/ray    |
        |  - descriptor / binding layout |
        |  - render-pass DAG             |
        +--------------------------------+
                          |
                          v
        +--------------------------------+
        |  cajeta.xpu.vulkan             |
        |  (SPIR-V emit, dispatch)       |
        +--------------------------------+
                |              |
        portable             accelerated
        Vulkan path          (RT cores /
                             RDNA RT) via
                             cajeta.xpu.nvidia /
                             cajeta.xpu.amd
```

The compiler emits one SPIR-V module per pipeline stage. The render
runtime records command buffers, manages descriptor pools, presents
to swap chains, and coordinates synchronization. Hardware-accelerated
ray tracing reaches in through the vendor namespaces under XPU when
the build targets them; software ray tracing falls back to Vulkan
compute kernels on devices without RT hardware.

---

## 3. Package layout

```
cajeta.render                  ── top-level: Device, Surface, Swapchain,
                                  CommandBuffer, RenderPass, RenderGraph

cajeta.render.raster           ── rasterization pipeline:
                                    Vertex / Fragment / Geometry /
                                    Tessellation stages, MeshPipeline
                                    (mesh + task shaders).

cajeta.render.ray              ── ray-tracing pipeline:
                                    RayGen / Miss / ClosestHit /
                                    AnyHit / Intersection / Callable
                                    stages, BVH building, ray queries.

cajeta.render.scene            ── scene graph: Node, Transform,
                                    Camera, Light, BoundingVolume.
                                    Frustum + occlusion culling.

cajeta.render.material         ── PBR material model (metallic-
                                    roughness), shader-graph-shaped
                                    material composition, texture
                                    sampling, BRDFs.

cajeta.render.mesh             ── Mesh, MeshLOD, MeshletCluster,
                                    skeletal skinning (dual-quat),
                                    morph targets, blend shapes.

cajeta.render.texture          ── Texture, RenderTarget,
                                    Framebuffer, KTX2 / BCn / ASTC
                                    decompression, mipmap generation.

cajeta.render.lighting         ── directional / point / spot / area
                                    lights, shadow maps (PCF, PCSS,
                                    cascaded), GI (light probes, ray-
                                    traced GI), volumetric fog.

cajeta.render.postfx           ── tone mapping (ACES, AgX), bloom,
                                    SSAO / GTAO, SSR, TAA, FSR /
                                    DLSS bindings, motion blur,
                                    chromatic aberration.

cajeta.render.io               ── glTF 2.0 import, EXR / HDR, KTX2
                                    texture container, animation
                                    clip import + compression.

cajeta.render.debug            ── wireframe, normal visualization,
                                    overdraw heatmap, GPU profiler
                                    overlay, RenderDoc capture hooks.
```

---

## 4. Top-level surface — `cajeta.render`

The objects every renderer touches, regardless of paradigm.

### 4.1 Device, Surface, Swapchain

```cajeta
public class Device {
    public static Device create(DeviceOptions opts);
    public static Device fromHandle(VkDevice handle, uint32 queueFamilyIndex);

    public PhysicalDeviceProperties properties();
    public DeviceFeatures           features();
    public Limits                   limits();
}

public class Surface {
    public static Surface forWindow(Window w);
    public static Surface offscreen(uint32 width, uint32 height, ColorFormat fmt);

    public Vec2<uint32> extent();
    public ColorFormat  format();
    public PresentMode  presentMode();
}

public class Swapchain {
    public Swapchain(Device d, Surface s, SwapchainOptions opts);

    public AcquireResult acquireNext(Timeout t);
    public void          present(SwapchainImage img, Event signalEvent);
    public void          resize(Vec2<uint32> newExtent);
}
```

`Device` wraps a `VkDevice` that the render library either created
itself (stand-alone mode) or accepted from a host application
(integration mode — typical for Cajeta in an existing Vulkan engine).
The XPU substrate underneath is the same object; the render-side
`Device` just adds the graphics-queue handle and the descriptor pools
the graphics pipelines need.

### 4.2 CommandBuffer and RenderPass

```cajeta
public class CommandBuffer {
    public CommandBuffer(Device d, CommandPoolKind kind = CommandPoolKind.GRAPHICS);

    public void record(RecordingScope scope);
    public void submit(Queue q, Event signal = null);
}

public class RenderPass {
    public RenderPass(Device d, RenderPassDescription desc);

    public void begin(CommandBuffer cmd, Framebuffer fb);
    public void end(CommandBuffer cmd);
    public void nextSubpass(CommandBuffer cmd);
}
```

`RenderPass` is the explicit-attachment-and-subpass model. It's
verbose but it matches Vulkan's actual cost model — implicit barriers
in a less-explicit API hurt mobile and integrated GPUs the most. For
projects that want a less-verbose surface, see §4.4.

### 4.3 Pipelines

```cajeta
public class GraphicsPipeline {
    public GraphicsPipeline(Device d, GraphicsPipelineDescription desc);
}

public class ComputePipeline {
    public ComputePipeline(Device d, ComputePipelineDescription desc);
}

public class RayTracingPipeline {
    public RayTracingPipeline(Device d, RayTracingPipelineDescription desc);
}

public class MeshPipeline {
    public MeshPipeline(Device d, MeshPipelineDescription desc);
}
```

Each pipeline kind is a different SPIR-V layout. `GraphicsPipeline`
binds vertex / fragment / tessellation / geometry stages;
`MeshPipeline` binds mesh + task; `RayTracingPipeline` binds the
hit-group set; `ComputePipeline` binds a single compute stage.

### 4.4 RenderGraph

For projects that prefer declarative graph composition over explicit
command-buffer recording:

```cajeta
public class RenderGraph {
    public RenderGraph(Device d);

    public RenderGraphHandle addPass(PassDescription desc);
    public RenderGraphHandle addResource(ResourceDescription desc);

    public void connect(RenderGraphHandle from, RenderGraphHandle to);

    public CompiledRenderGraph compile();
}
```

The compiled graph automatically inserts synchronization, schedules
parallel-eligible passes, and reuses transient resources across the
frame. Inspired by Frostbite's FrameGraph and Granite's
RenderGraph — a known-good shape for modern engines.

Render-graph use is opt-in. Hand-rolled command buffers stay
available; nothing in `cajeta.render` *forces* graph composition.

---

## 5. Rasterization — `cajeta.render.raster`

The classical pipeline: vertex assembly → primitive assembly →
rasterization → fragment shading → output merger.

### 5.1 Pipeline stages

```cajeta
@vertex
fn vs(in: VertexInput) -> VertexOut {
    let world = pc.model * vec4(in.position, 1.0);
    let clip  = pc.viewProj * world;
    return VertexOut {
        position: clip,
        normalWS: normalize((pc.normalMatrix * vec4(in.normal, 0.0)).xyz),
        uv:       in.uv,
    };
}

@fragment
fn fs(in: VertexOut) -> FragmentOut {
    let albedo = texture(material.albedo, sampler, in.uv).rgb;
    let normal = normalize(in.normalWS);
    let lit    = lighting.evaluate(albedo, normal, pc.cameraPos);
    return FragmentOut { color: vec4(lit, 1.0) };
}
```

A graphics pipeline is declared as a struct of stages:

```cajeta
class TerrainPipeline : GraphicsPipeline {
    @vertex   fn vs(in: VertexInput) -> VertexOut { ... }
    @fragment fn fs(in: VertexOut) -> FragmentOut { ... }

    @descriptor(set=0, binding=0) var heightMap: Texture2D<f16>;
    @descriptor(set=0, binding=1) var albedo:    Texture2D<rgba8>;
    @descriptor(set=0, binding=2) var sampler:   Sampler;
    @push_constant struct PC { camera: Mat4; lod: f32; }
}
```

The compiler emits one SPIR-V module per stage, plus the
`VkPipelineLayout` description in metadata.

### 5.2 Mesh shading

For GPU-driven rendering of meshlet-decomposed geometry:

```cajeta
@task
fn taskShader(in: TaskInput) -> TaskPayload {
    // Cull meshlets against frustum and HiZ, emit visible ones to mesh stage.
    if (!frustumTest(in.meshletBounds) || !hiZTest(in.meshletBounds)) {
        return TaskPayload { meshletCount: 0 };
    }
    return TaskPayload { meshletCount: 1, visibleMeshlets: [in.meshletId] };
}

@mesh(maxVertices=64, maxPrimitives=124)
fn meshShader(in: MeshInput) -> MeshOut {
    // Emit a meshlet's vertices and indices.
    ...
}
```

`@task` and `@mesh` lower to `VK_EXT_mesh_shader` SPIR-V opcodes.
Targets without mesh-shader support fall back to a vertex-shader-only
path that processes meshlets through indirect draws (slower; works
everywhere).

### 5.3 Tessellation and geometry stages

`@tess_control`, `@tess_eval`, and `@geometry` are present for legacy
content (terrain LOD via hardware tessellation, particle billboards
via geometry shaders), but new code is encouraged to use mesh
shading. The legacy stages are not deprecated — they remain useful
for specific cases — but the tutorials and examples land first on
mesh shading + compute.

---

## 6. Ray tracing — `cajeta.render.ray`

Hardware ray tracing where available, software fallback elsewhere.
Reaches into `cajeta.xpu.nvidia` (Turing+, OptiX-free path —
`VK_KHR_ray_tracing_pipeline` direct) and `cajeta.xpu.amd` (RDNA2+,
HIP-RT-free path) for the acceleration-structure builders; the
pipeline itself is portable Vulkan ray tracing.

### 6.1 Pipeline stages

```cajeta
@ray_gen
fn rayGen() {
    let pixel  = vec2(gl_LaunchID.xy);
    let center = pixel + 0.5;
    let inUV   = center / vec2(gl_LaunchSize.xy);
    let target = vec4(inUV * 2.0 - 1.0, 1.0, 1.0);

    let origin    = camera.position;
    let direction = normalize((camera.invView * target).xyz - origin);

    var payload : Payload;
    traceRay(tlas, RayFlags.OPAQUE, 0xff, 0, 0, 0, origin, 0.001, direction, 10000.0, payload);

    imageStore(outImage, gl_LaunchID.xy, vec4(payload.color, 1.0));
}

@closest_hit
fn closestHit(in: HitInput) -> Payload {
    let baryCoord = vec3(1.0 - in.attrib.x - in.attrib.y, in.attrib.x, in.attrib.y);
    let normal    = interpolate(in.triangle.normals, baryCoord);
    return Payload { color: shade(normal, in.worldPos, in.materialId) };
}

@miss
fn miss() -> Payload {
    return Payload { color: skybox.sample(direction) };
}
```

### 6.2 Acceleration structures

```cajeta
public class BLAS {                    // Bottom-level: geometry
    public BLAS(Device d, MeshGeometry geo, BuildHint hint);
    public void rebuild(BuildHint hint);
    public void refit();                          // for animated meshes
}

public class TLAS {                    // Top-level: instances
    public TLAS(Device d);
    public void addInstance(BLAS blas, Mat4 transform, uint32 instanceId);
    public void build(BuildHint hint);
    public void update();                         // for moved instances
}
```

`BLAS` build uses `VK_KHR_acceleration_structure`. On hardware-RT
devices, the driver issues native commands (RT cores on NVIDIA, RA
units on AMD). On software fallback, the build runs as a compute
kernel emitting a BVH8 layout.

### 6.3 Ray queries inside non-RT shaders

```cajeta
@fragment
fn fsWithRayQuery(in: VertexOut) -> FragmentOut {
    let q = rayQuery(tlas, RayFlags.TERMINATE_ON_FIRST_HIT, 0xff, in.worldPos, 0.01, lightDir, 1000.0);
    q.proceed();
    let occluded = q.hasHit();
    let shadow   = occluded ? 0.0 : 1.0;
    ...
}
```

Lowers to `SPV_KHR_ray_query`. Lets fragment / compute shaders fire
rays without going through a dedicated ray-tracing pipeline — the
usual path for ray-traced shadows / AO / GI integrated into a raster
G-buffer pass.

### 6.4 When to choose pipeline vs query

| Use case                          | Recommended       |
|-----------------------------------|-------------------|
| Path tracing, full RT             | Ray-tracing pipeline |
| Ray-traced shadows in a raster frame | Ray query in fragment |
| Ray-traced AO / GI in a compute pass | Ray query in compute |
| Cone tracing, sparse sampling     | Ray query         |

The pipeline form is heavier (separate hit-group binding, separate
shader stages) but lets the driver schedule rays optimally across
the SIMT. The query form is lighter (just an opcode in a normal
shader) but reuses the calling shader's invocation, which doesn't
suit primary rays.

---

## 7. Scene graph — `cajeta.render.scene`

Independent of the rendering pipelines — a scene graph is just a
tree of transforms and renderables. The render library provides one;
projects that need something different (DOD-flat, ECS-stored) skip it.

```cajeta
public class Node {
    public Transform transform;
    public Array<Node> children;
    public Node parent;
    public BoundingVolume bounds;

    public void addChild(#Node child);
    public Mat4 worldMatrix();              // cached, dirty-tracked
}

public class Transform {
    public Vec3<f32> position;
    public Quaternion<f32> rotation;
    public Vec3<f32> scale;

    public Mat4 toMatrix();
    public static Transform fromMatrix(Mat4 m);
}

public class Camera : Node {
    public Mat4 projection;                  // perspective or ortho
    public f32  aspect;
    public f32  fovY;                        // radians
    public f32  near;
    public f32  far;

    public Mat4 viewMatrix();
    public Frustum frustum();
}

public class Light : Node {
    public LightKind kind;                   // DIRECTIONAL / POINT / SPOT / AREA
    public Vec3<f32> color;
    public f32  intensity;                    // lumens or W/sr depending on unit
    public f32  range;                        // for point/spot
    public f32  innerCone;                    // for spot, radians
    public f32  outerCone;
}
```

### 7.1 Culling

```cajeta
public class FrustumCuller {
    public Array<Node> visibleIn(Frustum f, Node root);
}

public class HierarchicalZBuffer {
    public boolean isOccluded(BoundingVolume bv);
}
```

GPU-driven culling lives as compute passes that emit indirect draw
commands; the CPU-side `FrustumCuller` is for low-instance-count
scenes (UI overlays, sparse scenes) where the CPU is cheaper than
round-tripping to the GPU.

---

## 8. Materials — `cajeta.render.material`

PBR (metallic-roughness) is the default. Other models — Disney /
Burley, Anisotropic, Cloth, ClearCoat, Subsurface — are available as
material variants.

```cajeta
public class Material {
    public MaterialModel    model;            // PBR_METAL_ROUGH (default)
    public Texture2D<rgba8> albedo;
    public Texture2D<r8>    metallic;
    public Texture2D<r8>    roughness;
    public Texture2D<rgb8>  normal;
    public Texture2D<r8>    ao;
    public Texture2D<rgb16f> emissive;
    public Vec4<f32>        albedoFactor;
    public f32              metallicFactor;
    public f32              roughnessFactor;
}

public class MaterialInstance {
    public MaterialTemplate template;
    public PerInstanceParameters params;
}
```

Material *templates* are compiled SPIR-V — the actual fragment-shader
program. Instances reuse a template and supply per-instance parameters
(uniform-buffer slot). The split lets a scene render thousands of
unique instances of a few materials at descriptor-binding cost rather
than pipeline-rebuild cost.

A material that needs custom shading writes a Cajeta function in the
`@fragment` style; the compiler turns it into a template.

---

## 9. Mesh and asset pipeline — `cajeta.render.mesh`, `cajeta.render.io`

### 9.1 Mesh

```cajeta
public class Mesh {
    public KernelBuffer<Vertex> vertices;
    public KernelBuffer<uint32> indices;
    public Array<Submesh> submeshes;          // grouped by material
    public BoundingBox    bounds;
}

public class MeshLOD {
    public Array<Mesh> lods;                  // sorted by detail
    public f32         lodBias;
}

public class MeshletCluster {
    public KernelBuffer<Meshlet>  meshlets;         // 64 verts / 124 prims each
    public KernelBuffer<uint32>   meshletVertices;
    public KernelBuffer<uint8>    meshletTriangles;
    public BoundingSphere   bounds;
}
```

`MeshletCluster` is the input format for mesh-shader rendering (§5.2).
Static meshes ship to disk in meshlet form; runtime conversion from
classic vertex/index buffers is available for legacy content.

### 9.2 Skeletal animation

```cajeta
public class Skeleton {
    public Array<Bone>           bones;
    public Mat4                  bindPose;
    public Array<Mat4>           inverseBindMatrices;
}

public class AnimationClip {
    public f32                   duration;
    public Array<AnimationTrack> tracks;
}
```

Skinning uses dual-quaternion blend skinning by default (better
silhouette than linear-blend skinning for large rotations) — the
math lives in [`cajeta.math.linalg.DualQuaternion`](../../CajetaMath.md#geometry).
LBS remains available as a fallback for bones-heavy meshes where
dual-quat cost matters.

Animation clips compress via cubic-spline curves (default) or
quantized log-step keyframes. The format is stable across Cajeta
versions.

### 9.3 Asset import — `cajeta.render.io`

```cajeta
public Scene  loadGLTF(String path);
public Mesh   loadOBJ(String path);
public Image  loadEXR(String path);
public Image  loadKTX2(String path);

public void   saveScene(Scene s, String path, SceneFormat fmt = SceneFormat.GLTF);
```

glTF 2.0 is the canonical interchange format. Extensions supported in
v1: `KHR_materials_pbrSpecularGlossiness`, `KHR_materials_unlit`,
`KHR_lights_punctual`, `KHR_texture_basisu` (KTX2 textures),
`KHR_mesh_quantization`, `KHR_draco_mesh_compression`.

EXR for HDR textures and skyboxes. KTX2 for compressed runtime
textures (BCn on desktop, ASTC on mobile). DDS is read-only legacy.

---

## 10. Lighting and shadows — `cajeta.render.lighting`

```cajeta
public class DirectionalLight {
    public Vec3<f32> direction;
    public Vec3<f32> color;
    public f32       intensity;
    public ShadowConfig shadow;
}

public class PointLight   { /* position, color, intensity, range, shadow */ }
public class SpotLight    { /* position, direction, color, intensity, cone, shadow */ }
public class AreaLight    { /* rectangular / disk / cylinder, color, intensity, shadow */ }

public class ShadowMap {
    public ShadowKind         kind;       // PCF / PCSS / VSM / CASCADED
    public uint32             resolution;
    public f32                bias;
    public f32                slopeBias;
    public Array<f32>         cascadeSplits;
}
```

Cascaded shadow maps for directional lights, cube shadow maps for
point lights, single shadow maps for spot lights. Ray-traced shadows
via `cajeta.render.ray.RayQuery` (§6.3) are the high-quality
alternative on hardware-RT devices.

### 10.1 Global illumination

| Technique           | Quality | Cost   | RT required |
|---------------------|---------|--------|-------------|
| Light probes (SH-9) | low     | low    | no          |
| Lightmaps (baked)   | high    | author | no (runtime)|
| Voxel cone tracing  | medium  | medium | no          |
| Ray-traced GI       | high    | high   | yes         |
| Restir              | high    | medium | yes         |

`cajeta.render.lighting` provides all five. Projects pick at scene-
configuration time.

### 10.2 Volumetrics

`cajeta.render.lighting.volumetric` — froxel-based volumetric fog,
light scattering, atmosphere. Software-traced; RT versions land as
opt-in upgrades on hardware-RT devices.

---

## 11. Post-processing — `cajeta.render.postfx`

```cajeta
public class PostProcessChain {
    public PostProcessChain(Device d);

    public void addPass(PostFx fx);
    public void execute(CommandBuffer cmd, Texture2D input, Texture2D output);
}

public class ACESToneMap     : PostFx { /* tone mapping */ }
public class AgXToneMap      : PostFx { /* Blender-style */ }
public class Bloom           : PostFx { /* threshold + downsample + composite */ }
public class GTAO            : PostFx { /* Ground-truth AO */ }
public class SSR             : PostFx { /* Screen-space reflections */ }
public class TAA             : PostFx { /* Temporal AA */ }
public class MotionBlur      : PostFx { /* per-pixel velocity */ }
public class ChromaticAberration : PostFx { /* lens sim */ }
```

Vendor upscalers — DLSS, FSR, XeSS — live in
`cajeta.render.postfx.upscale` and bind through the appropriate XPU
vendor namespace (DLSS via `cajeta.xpu.nvidia.dlss`, FSR via portable
Vulkan compute, XeSS via `cajeta.xpu.intel.xess` once that backend
lands). The same `PostFx` interface; the binding is what changes.

---

## 12. Borrow checker interaction

The render library extends XPU's borrow-checker contract (per
[`CajetaXPU.md` §11](../xpu/CajetaXPU.md#11-borrow-checker-interaction-notable-cases))
with three render-specific cases:

1. **Texture freed while bound to an in-flight pipeline.**

   ```cajeta
   let tex = Texture2D<rgba8>.alloc(...);
   pipeline.bind(0, tex);
   cmd.draw(...);
   cmd.submit(queue);
   tex.free();                       // ERROR: live borrow until queue.wait()
   ```

2. **Render pass that outlives its framebuffer.**

   ```cajeta
   let fb = Framebuffer(...);
   {
       let pass = RenderPass.begin(cmd, fb);
       // ... record ...
   }
   fb.free();                        // ERROR: pass borrowed fb; pass not ended yet
   ```

3. **Swapchain image used after `present()`.**

   The acquired swapchain image is borrowed by the command buffer
   until the present completes (which is until the next
   `acquireNext()` returns it). Using it after `present()` is rejected
   at compile time.

These are checked uniformly across raster, ray, mesh, and compute
pipelines.

---

## 13. Toolchain integration

### 13.1 Compiler flags

```
--render-targets=<list>            # vulkan-1.2, vulkan-1.3, ...
--render-features=<list>           # mesh_shader, ray_tracing, descriptor_indexing, ...
--render-debug                     # enables shader-validation layer + RenderDoc hooks
```

### 13.2 Build artifacts

- One `*.spv` per shader stage, bundled per pipeline.
- Pipeline-layout metadata in a sidecar `*.cajeta-pipeline` (binding
  shapes, push-constant ranges, vertex input layout).
- Hot-reloadable sidecar files: `*.cajeta-spv` per stage, watched by
  the runtime (per [`CajetaXPU.md` §6.5](../xpu/CajetaXPU.md#65-hot-reload)).

### 13.3 Diagnostics

The `--diag-hints` "did you mean" pass extends with render-specific
suggestions:

- "`@geometry` stage is being compiled but the project's render
  targets include mobile devices that don't support it; consider
  `@mesh` + `@task` (works on more targets) or guard with
  `@feature(geometry_shader)`."
- "`RayQuery` requires `--render-features=ray_query`; nearest match
  is the closest-hit pipeline form. See `cajeta.render.ray` §6.4."
- "Push-constant block exceeds the 128-byte spec-minimum; targets
  without `maxPushConstantsSize >= 256` will reject the pipeline."

### 13.4 Debuggers and profilers

RenderDoc, NSight Graphics, Radeon GPU Profiler. SPIR-V carries
`OpLine` mapping back to the `.cajeta` source.

---

## 14. Phasing

A reasonable order of implementation. The substrate phases
([`CajetaXPU.md` §12](../xpu/CajetaXPU.md#12-phasing)) gate everything here.

1. **`cajeta.render.Device` / `Surface` / `Swapchain` / `CommandBuffer`.**
   The minimum to draw a triangle. Depends on XPU phase 5 (Vulkan
   backend, compute only — we ride the compute machinery for graphics
   setup).
2. **`cajeta.render.raster` — vertex + fragment.** A textured triangle,
   then a textured model loaded from glTF. The classic onboarding
   step.
3. **`cajeta.render.io` — glTF + KTX2.** Material-textured meshes
   loading from disk.
4. **`cajeta.render.material` — PBR metallic-roughness.** Standard
   material model, sun + skybox lighting, no shadows.
5. **`cajeta.render.scene` — node + camera + frustum culling.**
   Enough scene graph to render a scene of a few hundred objects.
6. **`cajeta.render.lighting` — directional + point + spot, shadow
   maps.** The classical raster lighting story.
7. **`cajeta.render.postfx` — tone mapping + bloom + TAA.** Enough
   post chain to make screenshots look modern.
8. **`cajeta.render.raster.MeshPipeline` — mesh + task shaders.**
   GPU-driven culling and meshlet rendering.
9. **`cajeta.render.ray` — pipeline + acceleration structures.**
   Hardware-RT path on NVIDIA + AMD, software fallback elsewhere.
10. **`cajeta.render.lighting` — ray-traced shadows / AO / GI.**
    Where ray tracing pays for itself in real engines.
11. **`cajeta.render.mesh.Skeleton` + `AnimationClip` — skeletal
    animation.** Dual-quat skinning, cubic-spline clips.
12. **`cajeta.render.RenderGraph` — declarative composition.** The
    convenience layer on top of explicit command-buffer recording.

---

## 15. Open questions

- **Where do compute-only render passes live?** Post-processing is
  almost entirely compute kernels — they don't need
  `cajeta.render.raster`. Two options: bake compute integration into
  `cajeta.render` (current sketch), or keep the post-fx chain as a
  thin wrapper around `cajeta.gpu` kernels. The current sketch
  is more ergonomic; the alternative would shave a few hundred lines
  off the render library. Defer until the post-fx surface is real.
- **Material authoring.** Cajeta-as-shading-language is the v1
  position, but a visual material-graph DSL is a known win for
  artist-facing workflows. Out of scope for v1; flagged as a likely
  v2 addition.
- **Mobile-specific paths.** Tiled rasterization, framebuffer fetch,
  PLS (pixel local storage) — Vulkan exposes these via extensions but
  they require careful authoring to be useful. Worth a dedicated
  mobile sub-doc once a target platform is identified.
- **Metal-native backend.** MoltenVK is the v1 Apple path. A direct
  Metal backend would bypass MoltenVK overhead at the cost of a
  separate codegen path. Worth quantifying the overhead before
  committing.
- **Web targets.** WebGPU is a different shape than Vulkan (subset of
  features, different binding model). Likely lands as a fourth XPU
  backend (`cajeta.xpu.webgpu`) and a parallel render backend
  (`cajeta.render.web`?). Out of v1.

---

## 16. Summary

`cajeta.render` is the graphics layer that sits on top of
`cajeta.xpu`. Raster, ray, mesh, and compute pipelines as peers.
Vulkan-via-`cajeta.xpu.vulkan` is the portable substrate; vendor
extensions reach into `cajeta.xpu.nvidia` and `cajeta.xpu.amd` for
hardware ray-tracing acceleration. The package layout splits the
surface into `raster`, `ray`, `scene`, `material`, `mesh`, `texture`,
`lighting`, `postfx`, `io`, `debug` — each a coherent block. The
borrow checker covers texture lifetimes and render-pass scopes the
same way it covers compute-kernel buffers. Asset path is glTF in,
meshlet + dual-quat-skinned mesh out. Build configuration selects
which Vulkan version and extensions to target; the same Cajeta source
runs on NVIDIA, AMD, Intel Arc, Apple, Android, and SteamDeck.
