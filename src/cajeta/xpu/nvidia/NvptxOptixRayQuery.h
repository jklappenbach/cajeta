//
// NVPTX → OptiX RT-core ray-query program emission (cajeta-gpu, NVIDIA-CUDA
// native RT path, Milestone 2 Phase 3).
//
// A cajeta @Kernel using RayQuery against an OptiX-impl AccelerationStructure can
// not run as a single cuLaunchKernel kernel — OptiX has no inline ray query. The
// RT cores are reached only through an OptiX PIPELINE: a set of raygen / anyhit /
// closesthit / intersection / miss programs + a Shader Binding Table, launched via
// optixLaunch. This module emits that program set for the canonical AABB
// candidate-COUNT shape (the kRqMinDriver pattern) as a SEPARATE PTX module.
//
// Why separate: the programs use the `_optix_*` inline-asm calls (optixTrace etc.)
// that only the OptiX module compiler (nvoptix.dll) understands — ptxas REJECTS
// them. So the OptiX programs never go through assembleCubin; their PTX text is fed
// straight to optixModuleCreate at launch. The kernel's ordinary software-BVH
// variant is still emitted + assembled as usual; the launch picks between
// cuLaunchKernel (software) and optixLaunch (OptiX) by the recorded AS impl.
//
// v1 scope: the canonical count shape, recognized by the @Kernel SIGNATURE (one
// AccelerationStructure, three Buffer origins, one Buffer output, one count scalar).
// A ray-query kernel whose signature is not the canonical count shape throws
// XPU-N04 — never a silent miscompile. Nearest-hit / getters are Phase 4.
//

#pragma once

#include <memory>
#include <string>

namespace llvm {
    class Module;
}

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {
namespace nvidia {

    // True iff `method` is a ray-query kernel — it has an AccelerationStructure
    // parameter (the same definition the Vulkan dual-variant path uses).
    bool nvptxKernelUsesRayQuery(const MethodPtr& method);

    // The canonical OptiX program-emission shapes (v1). A ray-query kernel is
    // classified by its body + signature; an unrecognized one is Unsupported
    // (the registration leaves it on the software cubin). The ordinal is shared
    // with the runtime's __cajeta_xpu_register_optix_rayquery `shape` arg + the
    // launch dispatch (cajeta_runtime.c), so keep them in sync.
    enum class OptixRqShape {
        Unsupported  = -1,
        CountAabb    = 0,   // AABB candidate count: anyhit increments + ignores
        NearestTri   = 1,   // triangle nearest-hit: closesthit commits T/prim
        BaryCandidate = 2,  // triangle candidate getters: anyhit reads T/bary + ignores
        CommittedTri  = 3,  // triangle committed per-launch: closesthit writes hit/front-face
    };

    // Classify a ray-query kernel by its proceed-loop body + signature:
    //   - calls confirmIntersection() / a committed* getter -> NearestTri
    //     (signature 1 AS, 2 Buffer, 0 scalar) else Unsupported
    //   - calls a candidate* getter (candidateDistance/BarycentricU/BarycentricV)
    //     (signature 1 AS, 1 Buffer, 0 scalar) -> BaryCandidate else Unsupported
    //   - canonical count signature (1 AS, 4 Buffer, 1 scalar) + candidateType
    //     -> CountAabb
    //   - otherwise -> Unsupported.
    OptixRqShape classifyRayQueryShape(const MethodPtr& method);

    // The compiler ↔ runtime launch-params layout for the OptiX count shape. The
    // emitted `params` const global is a packed struct in THIS order; the runtime
    // (M2 Phase 3-C) fills the matching struct before optixLaunch. Fields:
    //   handle  : OptixTraversableHandle (u64)         — the AS traversable
    //   originX : device ptr (u64) to Buffer<float32>  — kernel buffer arg 0
    //   originY : device ptr (u64) to Buffer<float32>  — kernel buffer arg 1
    //   originZ : device ptr (u64) to Buffer<float32>  — kernel buffer arg 2
    //   out     : device ptr (u64) to Buffer<uint32>   — kernel buffer arg 3
    //   n       : u32                                  — the count scalar
    //   boxes   : device ptr (u64) to 6*np floats      — the AS's AABB data
    // (`boxes` is NOT a kernel arg — the kernel only receives the AS — so the OptiX
    // glue must retain the AS's boxes on device and the launch passes them here.)

    // Emit the OptiX count-shape program set for `method` into `optixModule` (a
    // fresh module the caller has already configured for NVPTX, see
    // configureDeviceModule). Builds __raygen__<k> / __intersection__<k> /
    // __anyhit__<k> / __miss__<k> + the `params` const global. Returns the raygen
    // entry-function name (e.g. "__raygen__countHits"). Throws XPU-N04 if the
    // kernel's signature is not the canonical AABB-count shape.
    std::string emitOptixCountModule(const MethodPtr& method,
                                     llvm::Module& optixModule);

    // The compiler ↔ runtime launch-params layout for the triangle nearest-hit
    // shape. The emitted `params` const global is a packed struct in THIS order:
    //   handle : OptixTraversableHandle (u64)        — the triangle AS traversable
    //   outT   : device ptr (u64) to Buffer<float32> — kernel buffer arg 0 (outT)
    //   outI   : device ptr (u64) to Buffer<uint32>  — kernel buffer arg 1 (outI)
    // The ray (origin/dir/tmin/tmax) is BAKED into __raygen__ from the kernel's
    // initialize() literals (the nearest shape hardcodes its single ray).

    // Emit the OptiX triangle nearest-hit program set for `method`: __raygen__<k>
    // (single ray from the initialize() literals, built-in triangle traversal) /
    // __closesthit__<k> (writes committed T / type=1 / primitive index to outT,
    // outI via params) / __miss__<k> (writes committed type 0). No intersection /
    // anyhit (built-in triangle). Returns the raygen entry name. Throws XPU-N04 if
    // the signature/body is not the canonical nearest-hit shape or the ray args are
    // not compile-time constants.
    std::string emitOptixNearestModule(const MethodPtr& method,
                                       llvm::Module& optixModule);

    // The compiler ↔ runtime launch-params layout for the triangle candidate-getter
    // (barycentric) shape. The emitted `params` const global is a packed struct:
    //   handle : OptixTraversableHandle (u64)        — the triangle AS traversable
    //   out    : device ptr (u64) to Buffer<float32> — kernel buffer arg 0 (out)
    // The single ray is BAKED into __raygen__ from the initialize() literals.

    // Emit the OptiX triangle candidate-getter program set for `method`:
    // __raygen__<k> (single baked ray, built-in triangle traversal) / __anyhit__<k>
    // (reads the candidate's optixGetRayTmax + optixGetTriangleBarycentrics, writes
    // out[0]=t, out[1]=u, out[2]=v via params, then optixIgnoreIntersection to keep
    // enumerating — faithful candidate semantics) / __miss__<k> (no-op). No
    // closesthit (the kernel never commits). Returns the raygen entry name. Throws
    // XPU-N04 if the signature/body is not the canonical candidate-getter shape or
    // the ray args are not compile-time constants.
    std::string emitOptixBaryModule(const MethodPtr& method,
                                    llvm::Module& optixModule);

    // The compiler ↔ runtime launch-params layout for the committed-triangle
    // per-launch shape (the kTri-count / kFront front-face kernels). The emitted
    // `params` const global is a packed struct:
    //   handle : OptixTraversableHandle (u64)        — the triangle AS traversable
    //   b0     : device ptr (u64)                    — kernel Buffer arg 0 (ray-fed)
    //   b1     : device ptr (u64)                    — kernel Buffer arg 1 (ray-fed)
    //   out    : device ptr (u64)                    — kernel Buffer arg 2 (output)
    //   n      : u32                                 — the count scalar (launch width)
    // Each ray component is resolved per-arg from initialize(): a compile-time
    // constant, or a `b0[i]`/`b1[i]` load (the launch-index-fed buffers).

    // Emit the OptiX committed-triangle per-launch program set for `method`:
    // __raygen__<k> (per-launch index i<n; ray from the resolved const/buffer
    // components; built-in triangle traversal) / __closesthit__<k> (writes out[i] —
    // front-face mode `front?1:2` if the body reads committedFrontFace, else hit-flag
    // `1`) / __miss__<k> (writes out[i]=0). No anyhit/intersection. Returns the raygen
    // entry name. Throws XPU-N04 if the signature/body is not the canonical
    // committed-triangle shape or a ray arg is neither constant nor a buffer[i] load.
    std::string emitOptixCommittedTriModule(const MethodPtr& method,
                                            llvm::Module& optixModule);

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
