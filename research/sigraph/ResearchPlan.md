# Computer Graphics (SIGGRAPH) — Research Plan

> Initial research index for Cajeta. Status: initial pass (2026-05-28); virtual-geometry + Lumen deep-dive added 2026-05-28.

## Goals

Cajeta aims to give engine builders a native, borrow-checked systems language with first-class GPU integration (NVIDIA + AMD) compiled through LLVM 22. A graphics library that ships modern SIGGRAPH/HPG/EGSR-grade primitives — real-time path tracing with ReSTIR, BVH build/traversal, neural and analytic denoisers, virtualized geometry, virtual texturing, physically based BRDF/BSDF models, and radiance-field representations (NeRF / 3D Gaussian Splatting) — would let developers build new engines on Cajeta instead of bolting onto C++. The strategic value is offering these as a safe, composable standard-library surface where the memory model (deterministic ownership of large GPU buffers, acceleration structures, and tile caches) and the LLVM/GPU codegen path are the differentiators. This document is the seed index of primary sources and concrete algorithms to implement.

## Research Index

### Real-time Path Tracing & ReSTIR / ReSTIR GI

- **What:** Reservoir-based spatiotemporal importance resampling reuses light/path samples across pixels and frames so that a handful of rays per pixel yields converged-looking direct and indirect illumination.
- **Why for Cajeta:** This is the centerpiece real-time rendering algorithm of the last five years. A Cajeta `restir` module (reservoir buffers, RIS weights, spatial/temporal reuse passes) would be the headline feature for an RT-capable engine library.
- **Key papers / sources:**
  - [Spatiotemporal reservoir resampling for real-time ray tracing with dynamic direct lighting](https://cs.dartmouth.edu/~wjarosz/publications/bitterli20spatiotemporal.html) — Bitterli, Wyman, Pharr, Shirley, Lefohn, Jarosz; ACM TOG (SIGGRAPH) 39(4), 2020. The original ReSTIR DI; 6×–60× equal-error vs prior dynamic many-light methods. (URL resolves; landing page verified.)
  - [ReSTIR GI: Path Resampling for Real-Time Path Tracing](https://onlinelibrary.wiley.com/doi/abs/10.1111/cgf.14378) — Ouyang, Liu, Kettunen, Pharr, Pantaleoni; Computer Graphics Forum 40(8), 2021. Extends reservoir reuse to indirect/global illumination paths. (Wiley landing page resolves but is paywalled — 402.)
  - [Generalized Resampled Importance Sampling: Foundations of ReSTIR](https://dl.acm.org/doi/10.1145/3528223.3530158) — Lin, Kettunen, Bitterli, Pantaleoni, Yuksel, et al.; ACM TOG (SIGGRAPH) 41(4), 2022. The GRIS theory that makes biased/unbiased reuse rigorous. (ACM DOI resolves; returned 403 to the fetch bot — paywalled but valid.)
  - [A Gentle Introduction to ReSTIR Path Reuse in Real-Time](https://dl.acm.org/doi/10.1145/3587423.3595511) — ACM SIGGRAPH 2023 Courses. The best implementation-oriented tutorial. (ACM DOI; course notes.)
  - [ReSTIR PG: Path Guiding with Spatiotemporally Resampled Paths](https://research.nvidia.com/labs/rtr/publication/zeng2025restirpg/) — Zeng, Kettunen, Wyman, Wu, Ramamoorthi, Yan, Lin; SIGGRAPH Asia 2025. Derives path-guiding distributions from ReSTIR reservoirs. (NVIDIA landing page verified.)
  - [NVIDIA RTXDI (reference implementation)](https://github.com/NVIDIA-RTX/RTXDI) — production HLSL/Vulkan reference for ReSTIR DI/GI.
- **Algorithms to capture:** RIS (Resampled Importance Sampling); WRS (Weighted Reservoir Sampling, one-pass streaming); temporal reuse with motion-vector reprojection; spatial reuse with neighbor reservoir merging; GRIS reweighting + MIS for unbiased combination; permutation sampling for decorrelation; ReSTIR GI sample point reconnection.
- **Implementation notes:** Reservoir is a small POD struct (`sample`, `wSum`, `M`, `W`) — ideal as a Cajeta value type laid out in SoA GPU buffers. Borrow checker should own the double-buffered reservoir arrays (read-from-previous / write-to-current) to statically prevent the classic read/write aliasing bug. Expose passes as kernels generated to PTX/GCN via the LLVM backend; the RNG (PCG/xoshiro) must be a deterministic, per-pixel-seeded library type. API shape: a `ReservoirPass` trait with `initialSampling`, `temporalResample`, `spatialResample`, `shade` stages.

### Denoising (SVGF, ReBLUR/ReLAX, neural denoisers)

- **What:** Reconstruct a stable image from 1 spp noisy path-traced input using edge-aware spatial filtering, temporal accumulation, and variance estimation — or a trained neural network.
- **Why for Cajeta:** Real-time path tracing is unusable without denoising. A `denoise` module with both analytic (SVGF-class) and neural backends covers the full range from indie to AAA.
- **Key papers / sources:**
  - [Spatiotemporal Variance-Guided Filtering: Real-Time Reconstruction for Path-Traced Global Illumination](https://research.nvidia.com/labs/rtr/publication/schied2017spatiotemporal/) — Schied, Kaplanyan, Wyman, Patney, Chaitanya, Burgess, Liu, Dachsbacher, Lefohn, Salvi; HPG 2017 (Best Paper). ~10 ms 1080p reconstruction from 1 spp. (Landing page verified; preprint PDF also at cg.ivd.kit.edu.)
  - [Interactive Reconstruction of Monte Carlo Image Sequences using a Recurrent Denoising Autoencoder](https://research.nvidia.com/sites/default/files/publications/dnn_denoise_author.pdf) — Chaitanya, Kaplanyan, Schied, Salvi, Lefohn, Nowrouzezahrai, Aila; ACM TOG (SIGGRAPH) 36(4), 2017. Foundation of the OptiX neural denoiser. (PDF verified — valid 8.5 MB PDF.)
  - [ReBLUR: A Hierarchical Recurrent Denoiser](https://link.springer.com/content/pdf/10.1007/978-1-4842-7185-8_49.pdf) — Dmitry Zhdan (NVIDIA); in *Ray Tracing Gems II* (eds. Marrs, Shirley, Wald), ch. 49, Apress, 2021. Self-stabilizing recurrent blurring for low-ray-budget diffuse/specular. (Springer chapter PDF verified — valid 5.1 MB PDF.)
  - [NVIDIA Real-Time Denoisers (NRD) — ReBLUR, ReLAX, SIGMA](https://github.com/NVIDIA-RTX/NRD) — production reference; ReLAX is an SVGF derivative with fast-history clamping.
- **Algorithms to capture:** Temporal accumulation with disocclusion rejection (depth/normal/mesh-id tests); à-trous edge-stopping wavelet filter; per-pixel luminance variance estimation driving filter bandwidth; ReLAX fast/slow history clamping; recurrent U-Net autoencoder with G-buffer auxiliary inputs (albedo demodulation, normals, depth).
- **Implementation notes:** SVGF is a fixed pipeline of small image-space kernels — straightforward LLVM-generated compute shaders over G-buffer textures. Albedo demodulation/remodulation should be library helpers. Neural backend needs a tensor/inference interface; reuse the GPU offload path and consider interop with the project's ML stack (ONND-style). Borrow checker manages ping-pong history buffers and ensures G-buffer lifetimes outlive the denoise graph.

### Global Illumination (probe / irradiance-field methods)

- **What:** Ray-traced irradiance probes encode the dynamic diffuse light field on a grid, queried per-shading-point with visibility-aware interpolation, decoupling GI cost from screen resolution.
- **Why for Cajeta:** A robust, scalable GI option that complements ReSTIR GI; widely shipped (DDGI in commercial engines). Good "batteries-included" default for engine authors.
- **Key papers / sources:**
  - [Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields](https://research.nvidia.com/publication/2019-05_dynamic-diffuse-global-illumination-ray-traced-irradiance-fields) — Majercik, Guertin, Nowrouzezahrai, McGuire; Journal of Computer Graphics Techniques (JCGT) 8(2), 2019. Compact irradiance-field encoding with moment-based visibility. (NVIDIA landing page verified.)
  - [Importance-Based Ray Strategies for Dynamic Diffuse Global Illumination](https://dl.acm.org/doi/10.1145/3585500) — Liu, Huang, Rocha, Malmros, Zhang (Huawei); Proc. ACM Computer Graphics and Interactive Techniques (i3D) 6(1), 2023. Adaptive ray budgeting for DDGI. (Verified via author page allenliuzihao.github.io/IS-DDGI.)
  - [Dynamic Diffuse Global Illumination Resampling](https://dl.acm.org/doi/10.1145/3450623.3464635) — Majercik, Müller, Keller, Nowrouzezahrai, McGuire; ACM SIGGRAPH 2021 Talks. Brings ReSTIR-style resampling to probe updates. (Verified; open-access arXiv mirror at arxiv.org/abs/2108.05263.)
- **Algorithms to capture:** Octahedral probe parameterization; irradiance + depth-moment (mean / mean-of-squares) probe textures; Chebyshev visibility test for leak-free interpolation; hysteresis-blended temporal probe update; probe relocation/classification.
- **Implementation notes:** Probe grid is a 3D array of fixed-size octahedral tiles — a natural Cajeta strided buffer type with compile-time tile dimensions. Probe update is one ray-trace + atomic-blend kernel; query is a sampler helper. Ownership of the probe atlas and per-frame update scratch handled by the borrow model.

### BVH Construction & Traversal

- **What:** Bounding Volume Hierarchies are the acceleration structure for ray queries; modern builders trade build speed vs. Surface Area Heuristic (SAH) quality, and wide (BVH4/BVH8) layouts cut traversal cost.
- **Why for Cajeta:** Even with hardware RT, software BVH is needed for custom primitives, offline/baking, AMD parity, and CPU fallback. A first-class `bvh` module is foundational.
- **Key papers / sources:**
  - [HLBVH: Hierarchical LBVH Construction for Real-Time Ray Tracing of Dynamic Geometry](https://research.nvidia.com/sites/default/files/pubs/2010-06_HLBVH-Hierarchical-LBVH/HLBVH-final.pdf) — Pantaleoni, Luebke; HPG 2010. Morton-code spatial sort + SAH; basis for GPU builders. (NVIDIA PDF verified — valid 924 KB PDF.)
  - [Software Rasterization of 2 Billion Points in Real Time](https://arxiv.org/pdf/2204.01287) — Schütz, Kerbl, Wimmer; Proc. ACM CGIT (HPG) 2022. Atomic min/max compute rasterization (relevant to both BVH-free point rendering and software raster). (arXiv PDF verified — valid 2.2 MB PDF.)
  - [DOBB-BVH: Efficient Ray Traversal by Transforming Wide BVHs into Oriented Bounding Box Trees using Discrete Rotations](https://arxiv.org/abs/2506.22849) — Kern, Galvan, Oldcorn, Skinner, Mehalwal, Reyes Lozano, Chajdas (AMD); arXiv, 2025. Discrete-rotation OBB conversion of wide BVHs for tighter bounds. (arXiv verified.)
  - [Minimizing Ray Tracing Memory Traffic through Quantized Structures and Ray Stream Tracing](https://arxiv.org/abs/2505.24653) — Grauer, Hanika, Dachsbacher; arXiv, 2025. Compressed wide-BVH + stream traversal. (arXiv verified.)
- **Algorithms to capture:** LBVH via Morton codes + radix sort; HLBVH hierarchical emit with SAH on upper levels; PLOC / H-PLOC (parallel locally-ordered clustering, bottom-up); BVH2→BVH8 SAH-optimal widening; stack-based vs. stackless (restart/parent-pointer) traversal; compressed/quantized node layouts; ray stream / packet traversal for SIMD.
- **Implementation notes:** Node arrays are tight POD structs; Cajeta value types with explicit alignment map cleanly to GPU and SIMD. Radix sort and PLOC are excellent showcases for the GPU offload + LLVM codegen path. Borrow checker owns the build scratch (Morton arrays, cluster lists) separately from the immutable, shareable final BVH. Traversal stack should be a fixed-capacity stack type chosen at compile time.

### Hardware Ray Tracing (RTX / DXR / Vulkan RT) Integration

- **What:** Driver/GPU-accelerated BVH build + ray traversal exposed through DXR, Vulkan KHR ray tracing, and OptiX, with the shader-binding-table programming model.
- **Why for Cajeta:** First-class GPU RT integration is an explicit Cajeta ambition. The library must wrap acceleration-structure build, TLAS/BLAS, and the ray-gen / hit / miss shader model on both NVIDIA and AMD.
- **Key papers / sources:**
  - [RTXDI / RTX reference (NVIDIA-RTX org)](https://github.com/NVIDIA-RTX/RTXDI) — canonical DXR/Vulkan RT usage with ReSTIR.
  - [A Gentle Introduction to ReSTIR (SIGGRAPH 2023 Courses)](https://dl.acm.org/doi/10.1145/3587423.3595511) — also the best end-to-end "how the HW RT pipeline is driven" walkthrough. (ACM DOI.)
  - Vulkan `VK_KHR_ray_tracing_pipeline` / `VK_KHR_acceleration_structure` and Microsoft DXR specs (project documentation; (unverified) — to be pinned in backlog).
- **Algorithms to capture:** TLAS/BLAS build + refit; instance transforms; shader binding table layout; any-hit/closest-hit/miss/intersection program model; inline ray queries (RayQuery) vs. pipeline tracing; opacity micromaps and displaced micro-meshes (capture as future).
- **Implementation notes:** This is the GPU-offload abstraction layer. Cajeta should present a backend-neutral RT API that lowers to DXR/Vulkan/OptiX; the SBT and acceleration-structure handles are owned resources with explicit lifetimes (borrow model prevents use-after-free of a freed BLAS while a TLAS references it). Cross-vendor parity (AMD RDNA RT vs NVIDIA) is a key library responsibility.

### Virtualized Geometry (Nanite-style) & Software Rasterization

- **What:** Cluster-based, LOD-streamed micropolygon geometry rendered with a hybrid hardware/software rasterizer (compute rasterization wins for pixel-sized triangles). A precomputed DAG of triangle clusters lets the renderer pick, per view, a "cut" through the hierarchy giving ~1 triangle/pixel everywhere — **eliminating discrete LOD meshes and their popping**.
- **Why for Cajeta:** Virtualized geometry is the state of the art for massive scenes; offering it as a library (not just an engine feature) is a strong differentiator.
- **Key papers / sources:**
  - **Foundation (the multiresolution DAG that Nanite is built on):**
    - [Batched Multi-Triangulation](https://vcg.isti.cnr.it/Publications/2005/CGGMPS05/BatchedMT_Vis05.pdf) — Cignoni, Ganovelli, Gobbetti, Marton, Ponchio, Scopigno; IEEE Visualization 2005, pp. 207–214. A DAG of pre-batched mesh patches with error-bounded view-dependent cuts — the direct academic ancestor of Nanite's cluster DAG (see also the same group's *Adaptive TetraPuzzles*, SIGGRAPH 2004, for out-of-core construction). (PDF verified — 1.3 MB.)
  - **The technique:**
    - [A Deep Dive into Nanite Virtualized Geometry](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf) — Karis, Stubbe, Wihlidal (Epic); SIGGRAPH 2021 Advances in Real-Time Rendering course. The definitive reference. (Verified — 50 MB+ PDF.)
    - [Software Rasterization of 2 Billion Points in Real Time](https://arxiv.org/pdf/2204.01287) — Schütz, Kerbl, Wimmer; HPG 2022. Atomic-min compute rasterization for pixel-sized primitives. (arXiv PDF verified.)
    - [Real-Time Ray Tracing of Micro-Poly Geometry with Hierarchical Level of Detail](https://onlinelibrary.wiley.com/doi/10.1111/cgf.14868) — Benthin, Peters (Intel); Computer Graphics Forum (HPG) 42(8), 2023. RT-side analogue: LOD-aware micropolygon tracing. (Open-access PDF at momentsingraphics.de.)
  - **New optimizations on the approach:**
    - [Multiresolution Mesh Rendering Engine — Practicalities and Performance](https://www.cl.cam.ac.uk/~rkm38/pdfs/pettett2024_multiresolution_mesh_rendering.pdf) — Pettett, Mantiuk (Cambridge); CESCG 2024. A from-scratch mesh-shader Nanite-style engine in Vulkan; compares the Nanite-style intermediate-buffer pipeline against a no-intermediate-buffer variant and classic LOD chains. (PDF verified — 1.4 MB.)
    - [End-to-End Compressed Meshlet Rendering](https://diglib.eg.org/bitstream/handle/10.1111/cgf15002/v43i1_12_cgf15002.pdf) — Mlakar et al.; Computer Graphics Forum 43(1), 2024. Keeps meshlet geometry compressed all the way to the mesh shader, decompressing on-chip — directly attacks virtualized geometry's memory/bandwidth ceiling. (Open-access PDF verified — 1.4 MB.)
    - [NVIDIA RTX Mega Geometry / Cluster Acceleration Structures (CLAS)](https://developer.nvidia.com/blog/fast-ray-tracing-of-dynamic-scenes-using-nvidia-optix-9-and-nvidia-rtx-mega-geometry/) + [RTXMG SDK](https://github.com/NVIDIA-RTX/RTXMG) — NVIDIA, 2025. The major new optimization: a CLAS primitive (≤256-triangle clusters) + partitioned TLAS make BVH builds cheap enough to **ray trace full-quality Nanite geometry** (Blackwell adds HW cluster-intersection/compression engines). (Industry docs/SDK — html, not downloaded.)
  - **Cross-listed:** [Virtualized 3D Gaussians](https://arxiv.org/abs/2505.06523) — Yang et al., 2025. Nanite-style cluster LOD applied to Gaussian splats (see radiance-field section).
- **Algorithms to capture:** METIS/graph-partition meshlet & cluster-group generation; DAG-based cluster LOD with locked shared edges and per-group error bounds; per-view DAG cut by projected error; hierarchical + cluster culling (frustum, HZB occlusion, backface cone); persistent-thread software rasterizer with 64-bit atomic visibility buffer (depth|id min); material/shading pass over the visibility buffer; geometry streaming + page residency; compressed-meshlet on-chip decode; cluster acceleration structures (CLAS) for RT.
- **Implementation notes:** The visibility buffer (`atomicMin` on packed depth|triangle-id) is a Cajeta atomic-buffer primitive. The cluster DAG is an immutable shared structure (read-only borrow across all draw threads); the borrow model cleanly separates the read-only DAG from the per-frame mutable cut/cull scratch. Streaming page cache shares the virtual-texture residency model below. Compile-time meshlet size constants (e.g. 128 verts / 256 tris to match CLAS) enable tight codegen. Mesh-shader vs. software-raster path selection is a runtime capability query; RT of virtualized geometry (CLAS) ties into the Hardware Ray Tracing section.

### Alternative / Competing Geometry Representations (vs. Nanite)

- **What:** Other answers to "unlimited geometric detail without LOD popping" that do **not** use Nanite's triangle-cluster DAG — voxels/SVOs, displaced micro-meshes, point/splat clouds, and pure GPU-driven software geometry pipelines.
- **Why for Cajeta:** The "right" virtual-geometry primitive is unsettled. A Cajeta graphics library should understand the trade-offs (authoring, RT compatibility, memory, animation) and may offer more than one backend behind a common scene API.
- **Key papers / sources:**
  - [Efficient Sparse Voxel Octrees](https://research.nvidia.com/sites/default/files/pubs/2010-02_Efficient-Sparse-Voxel/laine2010i3d_paper.pdf) — Laine, Karras (NVIDIA); I3D 2010. The canonical SVO: compact voxel encoding + contours + fast ray casts, geometric detail competitive with triangles. Root of the voxel-engine lineage (Euclideon "Unlimited Detail", Atomontage, NanoTech octree virtual geometry). (PDF verified — 7.6 MB.)
  - [Micro-Mesh Construction](https://d1qx31qr3h6wln.cloudfront.net/publications/MicroMesh_generation.pdf) — Maggiordomo (Univ. Milan), Moreton (NVIDIA), Tarini (Univ. Milan); SIGGRAPH 2023. NVIDIA Displaced Micro-Meshes (DMM): a base mesh + barycentric displacement grid — a HW-RT-native micro-geometry primitive, the hardware-centric competitor to Nanite's software clusters. (PDF verified — 122 MB.)
  - [On-the-fly Vertex Reuse for Massively-Parallel Software Geometry Processing](https://arxiv.org/pdf/1805.08893) — Kenzel, Kerbl, Schmalstieg, Steinberger; HPG 2018. A fully software, GPU-driven geometry/raster pipeline — the programmable-pipeline angle of which Nanite's compute rasterizer is one instance. (arXiv PDF verified — 8.2 MB.)
  - **Point / splat clouds as the geometry primitive:** [Software Rasterization of 2 Billion Points](https://arxiv.org/pdf/2204.01287) (Schütz, above) and [3D Gaussian Splatting](https://arxiv.org/abs/2308.04079) / [Virtualized 3D Gaussians](https://arxiv.org/abs/2505.06523) (radiance-field section) — a non-triangle route to massive detail.
  - **SDF / hybrid renderers:** Media Molecule *Dreams* — Alex Evans, "Learning from Failure: a survey of promising, unconventional and mostly abandoned renderers for *Dreams*", SIGGRAPH 2015 Advances. Signed-distance + point-splat rendering shipped in a game. (Talk/slides — html, backlog.)
- **Algorithms to capture:** SVO ray-cast with contour test + beam optimization; ESVO normal/colour compression; DMM barycentric displacement encode/decode + micro-triangle RT; software vertex-reuse cache + programmable geometry stages; point/splat atomic compositing (cross-ref software raster + 3DGS).
- **Implementation notes:** These are alternative backends behind one scene/geometry abstraction, not language-level competitors. SVO and DMM are tight bit-packed structures — good Cajeta value-type + bitfield tests and natural GPU-buffer citizens. DMM should lower onto the same HW-RT abstraction as the BVH section (it *is* an RT primitive). Decide which primitive(s) the v1 library commits to; keep the door open with a `Geometry` trait (triangle-cluster | voxel | micro-mesh | splat).

### Dynamic Global Illumination — Lumen & Competing Next-Gen Lighting

- **What:** Fully dynamic, real-time GI + reflections with no baked lightmaps. Lumen (UE5) is the reference system; several fundamentally different schemes compete for the same job.
- **Why for Cajeta:** GI is the other half of a modern engine (Nanite handles geometry, Lumen handles light). An engine-builder library needs a dynamic-GI subsystem; understanding Lumen's structure and its competitors decides what Cajeta should expose.
- **How Lumen works (capture for implementation):** Lumen is a *hybrid, multi-representation* GI system:
  1. **Scene representation:** per-mesh **Signed Distance Fields** merged into a global SDF for software ray tracing, plus optional **hardware ray tracing** (DXR/RTX) where available.
  2. **Surface Cache:** materials captured into per-mesh "cards" (a few orthographic captures) giving cheap albedo/normal/emissive lookups at any ray hit — decouples shading from geometry complexity.
  3. **Ray tracing fallback chain:** short **screen-space traces** → **mesh-SDF software traces** (or HW-RT) → coarse **global SDF / voxel** trace for distant light.
  4. **Radiance caching & final gather:** world-space **radiance cache** + screen-space **radiance probes** + spatial/temporal filtering (and ReSTIR-style reuse) to denoise a tiny ray budget; reflections reuse the same hit-lighting path.
- **Key papers / sources:**
  - [Lumen: Real-time Global Illumination in Unreal Engine 5](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Lumen-Wright%20et%20al.pdf) — Wright, Narkowicz, Kelly (Epic); SIGGRAPH 2022 Advances in Real-Time Rendering. The definitive Lumen reference (SDF tracing, surface cache, screen/world radiance probes, final gather). (PDF verified — 25.8 MB.)
- **Competing next-gen lighting algorithms:**
  - [Radiance Cascades: A Novel Approach to Calculating Global Illumination](https://github.com/Raikiri/RadianceCascadesPaper) — Alexander Sannikov (Grinding Gear Games), 2023 (shipped in *Path of Exile 2*). Exploits the "penumbra hypothesis" (near light needs high spatial / low angular resolution, far light the reverse): a cascade hierarchy where each level doubles ray count and halves probe density. A radically different GI structure. (PDF verified — 57.8 MB.)
  - [Real-Time Neural Radiance Caching for Path Tracing](https://arxiv.org/pdf/2106.12372) — Müller, Rousselle, Novák, Keller (NVIDIA); ACM TOG (SIGGRAPH) 40(4), 2021. A tiny self-training MLP caches radiance, queried after a few path bounces — the neural competitor to analytic radiance caches. (arXiv PDF verified — 19.7 MB.)
  - **Cross-reference (already indexed):** **ReSTIR GI** (reservoir path reuse) and **DDGI** (ray-traced irradiance probes) are the other two mainstream dynamic-GI families; Lumen itself borrows screen-probe + reservoir ideas.
- **Algorithms to capture:** per-mesh SDF generation + global SDF merge; SDF sphere-trace; surface-cache card capture + atlas; screen → SDF → global-SDF trace fallback chain; world-space radiance cache + screen radiance probes; radiance-cascade construction + merging (penumbra hypothesis); neural radiance cache (tiny MLP, self-training, streaming update).
- **Implementation notes:** Lumen is essentially a *composition* of subsystems Cajeta already needs — SDF gen, a probe/cache buffer model, screen-space tracing, and the BVH/HW-RT layer — so it's a strong integration test of the library rather than one monolithic feature. The surface/radiance caches are owned GPU atlases with feedback-driven residency (share the `PageCache` infra). The fallback trace chain is a clean place for a capability-gated trait (screen | SDF | HW-RT backends). Radiance Cascades and NRC are alternative `GiBackend` implementations behind one final-gather interface; NRC needs the shared `inference` abstraction (cross-ref denoiser/NeRF).

### Virtual Texturing

- **What:** Treat a huge texture as a sparse set of tiles, stream only resident pages to GPU, and indirect UV lookups through a page table.
- **Why for Cajeta:** Texture memory is a hard ceiling for large worlds; a `virtualtexture` module (and shared residency/streaming infra with virtualized geometry) is high value.
- **Key papers / sources:**
  - [Sparse Virtual Textures (Sean Barrett)](https://silverspaceship.com/src/svt/) — foundational SVT writeup + reference. (Primary author resource.)
  - [Accelerating Virtual Texturing Using CUDA](https://www.researchgate.net/publication/265202211_Accelerating_Virtual_Texturing_Using_CUDA) — Hollemeersch, Pieters, Lambert, Van de Walle; in *GPU Pro: Advanced Rendering Techniques*, ch. 5.2, 2010. GPU feedback + page management. (Verified; open-access GTC poster mirror at nvidia.com.)
  - [High-performance adaptive texture streaming and rendering of large 3D cities](https://link.springer.com/article/10.1007/s00371-021-02152-z) — Zhang, Chen, Johan, Erdt; The Visual Computer 38, pp. 1245–1262, 2022 (online 2021). Modern hardware-VT streaming, stutter mitigation. (Verified; open-access PDF at fraunhofer.sg.)
  - Hardware sparse-residency APIs: Vulkan sparse partially-resident images, D3D12 tiled resources, OpenGL ARB_sparse_texture (project docs; (unverified)).
- **Algorithms to capture:** Page-table indirection texture; GPU feedback buffer (which pages were requested this frame); LRU page cache eviction; mip/clipmap residency; anisotropic-safe page borders; async tile upload/transcode.
- **Implementation notes:** Page table + physical tile atlas are owned GPU resources; the feedback→upload loop is a per-frame CPU/GPU pipeline whose buffer lifetimes the borrow checker manages. Unify the residency manager with virtualized geometry's page cache as a generic `PageCache<TileKey, TileData>` standard-library type. Prefer hardware sparse residency where available, software indirection as fallback.

### Physically Based Rendering (BRDF / BSDF models)

- **What:** Energy-conserving microfacet reflectance models (GGX/Cook-Torrance) plus artist-friendly layered/principled parameterizations.
- **Why for Cajeta:** Every shading path needs a correct, shared BRDF/BSDF library. This is the most reused, lowest-level graphics module.
- **Key papers / sources:**
  - [Physically Based Shading at Disney](https://media.disneyanimation.com/uploads/production/publication_asset/48/asset/s2012_pbs_disney_brdf_notes_v3.pdf) — Brent Burley; SIGGRAPH 2012 Physically Based Shading course. The "principled" BRDF. (PDF verified — valid 4.4 MB PDF.)
  - [Extending the Disney BRDF to a BSDF with Integrated Subsurface Scattering](https://blog.selfshadow.com/publications/s2015-shading-course/burley/s2015_pbs_disney_bsdf_notes.pdf) — Brent Burley; SIGGRAPH 2015 Physically Based Shading course. Principled BSDF + thin-surface transmission/SSS. (selfshadow PDF verified — valid 14 MB PDF.)
  - [Crash Course in BRDF Implementation](https://boksajak.github.io/files/CrashCourseBRDF.pdf) — Jakub Boksansky, 2021. Practical importance-sampling code for GGX/Disney. (PDF verified — valid 1.7 MB PDF.)
- **Algorithms to capture:** Cook-Torrance microfacet specular; GGX/GTR normal distribution + Smith masking-shadowing (height-correlated); Schlick Fresnel; multiscatter energy compensation; VNDF (visible normal distribution) importance sampling; Disney diffuse + sheen/clearcoat lobes; layered/stack BSDF evaluation.
- **Implementation notes:** BRDF lobes are pure, side-effect-free Cajeta functions over `(wo, wi, params)` — trivially inlinable and a great test of LLVM math codegen (vectorization, FMA). Provide both `evaluate` and `sample`/`pdf` for MIS. A `Bsdf` trait with associated lobe composition lets engines build layered materials safely. Keep these `const`/value-semantics so they run identically on CPU and GPU.

### 3D Gaussian Splatting & Neural Radiance Fields

- **What:** Radiance-field scene representations — NeRF (MLP + ray-marched volume rendering, accelerated by hash encodings) and 3D Gaussian Splatting (explicit anisotropic Gaussians, tile-based differentiable rasterization).
- **Why for Cajeta:** Captured/learned content and novel-view synthesis are now mainstream; a `radiancefield` module (splat rasterizer + optional NeRF inference) future-proofs the library.
- **Key papers / sources:**
  - [3D Gaussian Splatting for Real-Time Radiance Field Rendering](https://arxiv.org/abs/2308.04079) — Kerbl, Kopanas, Leimkühler, Drettakis; ACM TOG (SIGGRAPH) 42(4), 2023. Real-time (≥30 fps) 1080p radiance fields via tile-based splat rasterization. (arXiv verified.)
  - [Instant Neural Graphics Primitives with a Multiresolution Hash Encoding](https://arxiv.org/abs/2201.05989) — Müller, Evans, Schied, Keller; ACM TOG (SIGGRAPH) 41(4), 2022. ~1000× NeRF training speedup via multiresolution hash grid. (arXiv verified.)
  - [Virtualized 3D Gaussians: Flexible Cluster-based Level-of-Detail System for Real-Time Rendering of Composed Scenes](https://arxiv.org/abs/2505.06523) — Yang, Xu, Jiang, Lin, Dai; arXiv, 2025. Nanite-style LOD for splats. (arXiv verified.)
  - [A Survey on 3D Gaussian Splatting](https://arxiv.org/abs/2401.03890) — Chen, Wang; arXiv, 2024 (accepted by ACM Computing Surveys). Field overview / taxonomy. (arXiv verified.)
- **Algorithms to capture:** 3DGS tile-based differentiable rasterizer (frustum cull → per-tile depth sort → front-to-back alpha-blend of projected 2D Gaussians); adaptive densification/pruning during optimization; spherical-harmonic view-dependent color; multiresolution hash-grid feature encoding + tiny MLP; ray-marched volume rendering with occupancy/density grid skipping.
- **Implementation notes:** The splat rasterizer is a GPU-offload kernel: per-tile sort + atomic alpha compositing — pairs well with Cajeta's GPU codegen and atomic-buffer types. Gaussian parameters (position, covariance/quaternion+scale, SH coeffs, opacity) form a SoA value-type buffer owned by a `GaussianCloud`. Hash-grid NeRF needs the same tensor/inference interop as the neural denoiser; share an `inference` abstraction. Differentiable training is out of scope for v1 (inference/rendering first); note borrow-checking interplay with autodiff buffers as an open question.

## PDF / paper backlog

- [x] Spatiotemporal Reservoir Resampling (ReSTIR DI) — https://cs.dartmouth.edu/~wjarosz/publications/bitterli20spatiotemporal.html — papers/bitterli-2020-restir-di.pdf
- [x] ReSTIR GI: Path Resampling for Real-Time Path Tracing — https://onlinelibrary.wiley.com/doi/abs/10.1111/cgf.14378 — papers/ouyang-2021-restir-gi.pdf
- [x] Generalized Resampled Importance Sampling: Foundations of ReSTIR — https://dl.acm.org/doi/10.1145/3528223.3530158 — papers/lin-2022-gris-foundations-restir.pdf
- [x] A Gentle Introduction to ReSTIR Path Reuse (SIGGRAPH 2023 Course) — https://dl.acm.org/doi/10.1145/3587423.3595511 — papers/wyman-2023-gentle-intro-restir-course.pdf
- [x] ReSTIR PG: Path Guiding with Spatiotemporally Resampled Paths — https://research.nvidia.com/labs/rtr/publication/zeng2025restirpg/ — papers/zeng-2025-restir-pg.pdf
- [x] Spatiotemporal Variance-Guided Filtering (SVGF) — https://research.nvidia.com/labs/rtr/publication/schied2017spatiotemporal/ — papers/schied-2017-svgf.pdf
- [x] Recurrent Denoising Autoencoder (OptiX neural denoiser basis) — https://research.nvidia.com/sites/default/files/publications/dnn_denoise_author.pdf — papers/chaitanya-2017-recurrent-denoising-autoencoder.pdf
- [x] ReBLUR: A Hierarchical Recurrent Denoiser (Ray Tracing Gems II ch.49) — https://link.springer.com/content/pdf/10.1007/978-1-4842-7185-8_49.pdf — papers/zhdan-2021-reblur.pdf
- [x] Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields (DDGI) — https://research.nvidia.com/publication/2019-05_dynamic-diffuse-global-illumination-ray-traced-irradiance-fields — papers/majercik-2019-ddgi.pdf
- [x] HLBVH: Hierarchical LBVH Construction — https://research.nvidia.com/sites/default/files/pubs/2010-06_HLBVH-Hierarchical-LBVH/HLBVH-final.pdf — papers/pantaleoni-2010-hlbvh.pdf
- [x] Software Rasterization of 2 Billion Points in Real Time — https://arxiv.org/pdf/2204.01287 — papers/schutz-2022-software-rasterization-2b-points.pdf
- [x] A Deep Dive into Nanite Virtualized Geometry (SIGGRAPH 2021 Advances) — https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf — papers/karis-2021-nanite.pdf
- [x] Real-Time Ray Tracing of Micro-Poly Geometry with Hierarchical LOD — https://onlinelibrary.wiley.com/doi/abs/10.1111/cgf.14868 — papers/benthin-2023-micropoly-rt-hlod.pdf
- [x] Batched Multi-Triangulation (Nanite DAG foundation) — https://vcg.isti.cnr.it/Publications/2005/CGGMPS05/BatchedMT_Vis05.pdf — papers/cignoni-2005-batched-multi-triangulation.pdf
- [ ] Adaptive TetraPuzzles (out-of-core multiresolution, SIGGRAPH 2004) — ACM TOG 23(3):796–803 — (paywalled, not downloaded)
- [x] Multiresolution Mesh Rendering Engine — Practicalities & Performance (Pettett 2024) — https://www.cl.cam.ac.uk/~rkm38/pdfs/pettett2024_multiresolution_mesh_rendering.pdf — papers/pettett-2024-multiresolution-mesh-rendering.pdf
- [x] End-to-End Compressed Meshlet Rendering (Mlakar 2024) — https://diglib.eg.org/bitstream/handle/10.1111/cgf15002/v43i1_12_cgf15002.pdf — papers/mlakar-2024-compressed-meshlet-rendering.pdf
- [ ] NVIDIA RTX Mega Geometry / CLAS (OptiX 9, 2025) — https://developer.nvidia.com/blog/fast-ray-tracing-of-dynamic-scenes-using-nvidia-optix-9-and-nvidia-rtx-mega-geometry/ — (industry docs/SDK, html-only)
- [x] Efficient Sparse Voxel Octrees (Laine & Karras 2010) — https://research.nvidia.com/sites/default/files/pubs/2010-02_Efficient-Sparse-Voxel/laine2010i3d_paper.pdf — papers/laine-karras-2010-efficient-sparse-voxel-octrees.pdf
- [x] Micro-Mesh Construction / Displaced Micro-Meshes (Maggiordomo 2023) — https://d1qx31qr3h6wln.cloudfront.net/publications/MicroMesh_generation.pdf — papers/maggiordomo-2023-micro-mesh-construction.pdf
- [x] On-the-fly Vertex Reuse for Massively-Parallel Software Geometry Processing (Kenzel 2018) — https://arxiv.org/pdf/1805.08893 — papers/kenzel-2018-on-the-fly-vertex-reuse-software-geometry.pdf
- [x] Lumen: Real-time Global Illumination in UE5 (Wright 2022) — https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Lumen-Wright%20et%20al.pdf — papers/wright-2022-lumen-ue5-gi.pdf
- [x] Radiance Cascades (Sannikov 2023) — https://github.com/Raikiri/RadianceCascadesPaper — papers/sannikov-2023-radiance-cascades.pdf
- [x] Real-Time Neural Radiance Caching for Path Tracing (Müller 2021) — https://arxiv.org/pdf/2106.12372 — papers/muller-2021-neural-radiance-caching.pdf
- [ ] Sparse Virtual Textures (Sean Barrett) — https://silverspaceship.com/src/svt/ — (html-only, not downloaded)
- [x] Physically Based Shading at Disney (2012 BRDF) — https://media.disneyanimation.com/uploads/production/publication_asset/48/asset/s2012_pbs_disney_brdf_notes_v3.pdf — papers/burley-2012-disney-brdf.pdf
- [x] Extending the Disney BRDF to a BSDF (2015) — https://blog.selfshadow.com/publications/s2015-shading-course/burley/s2015_pbs_disney_bsdf_notes.pdf — papers/burley-2015-disney-bsdf.pdf
- [x] Crash Course in BRDF Implementation (Boksansky) — https://boksajak.github.io/files/CrashCourseBRDF.pdf — papers/boksansky-2021-crash-course-brdf.pdf
- [x] 3D Gaussian Splatting for Real-Time Radiance Field Rendering — https://arxiv.org/abs/2308.04079 — papers/kerbl-2023-3d-gaussian-splatting.pdf
- [x] Instant Neural Graphics Primitives (multiresolution hash encoding) — https://arxiv.org/abs/2201.05989 — papers/muller-2022-instant-ngp.pdf
- [x] A Survey on 3D Gaussian Splatting — https://arxiv.org/pdf/2401.03890 — papers/chen-2024-survey-3dgs.pdf

## Open questions

- **GPU codegen target:** Does Cajeta lower compute kernels to SPIR-V (Vulkan), PTX (NVIDIA), and GCN/RDNA (AMD) directly via LLVM 22 backends, or via an intermediate (e.g., wrap existing HLSL/DXC)? This decides how much of ReSTIR/SVGF/BVH can be authored once in Cajeta vs. wrapped.
- **Hardware RT abstraction:** What is the backend-neutral surface for DXR / Vulkan KHR RT / OptiX, and how do opacity micromaps / displaced micro-meshes fit? Cross-vendor (AMD vs NVIDIA) feature parity needs a capability-query design.
- **Borrow model vs. ping-pong/atomic buffers:** Reservoirs, denoiser history, and visibility buffers all rely on previous-frame reads + current-frame writes and on GPU atomics. How does the borrow checker model GPU-side atomic mutation and frame-to-frame double buffering without forcing `unsafe`?
- **Inference interop:** Neural denoisers and NeRF need a tensor/inference runtime. Reuse the workspace ML stack or build a minimal in-library inference path? Define a single `inference` abstraction shared by denoise + radiancefield.
- **Shared residency/streaming infra:** Can virtualized geometry and virtual texturing share one generic `PageCache` + feedback-driven streaming subsystem in the standard library?
- **Differentiable rendering scope:** 3DGS/NeRF training implies autodiff over GPU buffers. Is training in scope for v1, and how does autodiff interact with ownership/borrowing of gradient buffers? (Lean toward inference/rendering only for v1.)
- **Virtual-geometry primitive choice:** Does v1 commit to a single primitive (triangle-cluster DAG, à la Nanite) or expose a `Geometry` trait spanning clusters / SVO / displaced micro-meshes / splats? RT compatibility (CLAS, DMM are RT-native; software clusters need a CLAS-style bridge) and animation/skinning support should drive the decision.
- **Lumen as composition vs. monolith:** Lumen is built from SDF gen + surface/radiance caches + a screen→SDF→HW-RT fallback chain. Should Cajeta ship a single "Lumen-like" GI module, or a set of composable pieces (SDF, `PageCache`, trace-backend trait, final-gather trait) that *also* let users assemble Radiance Cascades / NRC / DDGI / ReSTIR GI backends? Lean composable.
- **Source pinning:** Several entries are course notes / book chapters / paywalled (ACM, Wiley) or marked (unverified); pin canonical DOIs and mirror open-access PDFs where license permits before deep study. Outstanding: Adaptive TetraPuzzles PDF; RTX Mega Geometry has no formal paper (industry docs/SDK only); the *Dreams* talk is slides-only.
