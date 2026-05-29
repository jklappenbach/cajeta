# Computer Graphics (SIGGRAPH) — Research Plan

> Initial research index for Cajeta. Status: initial pass (2026-05-28).

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

- **What:** Cluster-based, LOD-streamed micropolygon geometry rendered with a hybrid hardware/software rasterizer (compute rasterization wins for pixel-sized triangles).
- **Why for Cajeta:** Virtualized geometry is the state of the art for massive scenes; offering it as a library (not just an engine feature) is a strong differentiator.
- **Key papers / sources:**
  - [A Deep Dive into Nanite Virtualized Geometry](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf) — Karis, Stubbe, Wihlidal; SIGGRAPH 2021 Advances in Real-Time Rendering course. The definitive reference. (URL resolves; PDF too large to fetch inline but link valid.)
  - [Software Rasterization of 2 Billion Points in Real Time](https://arxiv.org/pdf/2204.01287) — Schütz et al.; HPG 2022. Atomic-based compute rasterization techniques. (arXiv PDF verified.)
  - [Real-Time Ray Tracing of Micro-Poly Geometry with Hierarchical Level of Detail](https://onlinelibrary.wiley.com/doi/10.1111/cgf.14868) — Benthin, Peters (Intel); Computer Graphics Forum (HPG) 42(8), 2023. RT-side analogue: LOD-aware micropolygon tracing. (Verified; open-access PDF at momentsingraphics.de.)
  - [Virtualized 3D Gaussians: Flexible Cluster-based Level-of-Detail System for Real-Time Rendering of Composed Scenes](https://arxiv.org/abs/2505.06523) — Yang, Xu, Jiang, Lin, Dai; arXiv, 2025. Nanite-style LOD applied to Gaussian splats. (arXiv verified.)
- **Algorithms to capture:** Meshlet/cluster generation; DAG-based cluster LOD with locked shared edges; hierarchical + cluster culling (frustum, occlusion via HZB, backface cone); persistent-thread software rasterizer with 64-bit atomic visibility buffer (depth+id min); material/shading pass over visibility buffer; geometry streaming + page residency.
- **Implementation notes:** The visibility buffer (`atomicMin` on packed depth|triangle-id) is a Cajeta atomic-buffer primitive. Cluster DAG is an immutable shared structure (read-only borrow across all draw threads). Streaming page cache mirrors the virtual-texture residency model below — share that machinery. Compile-time meshlet size constants enable tight codegen.

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
- **Source pinning:** Several entries are course notes / book chapters / paywalled (ACM, Wiley) or marked (unverified); pin canonical DOIs and mirror open-access PDFs where license permits before deep study.
