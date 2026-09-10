// NVPTX → OptiX RT-core ray-query program emission: a @Kernel using RayQuery on
// an OptiX AccelerationStructure becomes an OptiX pipeline in a separate PTX
// module (never through ptxas), launched by optixLaunch, not cuLaunchKernel.

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

    // True iff `method` is a ray-query kernel: it has an AccelerationStructure
    // parameter, the same test the Vulkan dual-variant path uses.
    bool nvptxKernelUsesRayQuery(const MethodPtr& method);

    // The OptiX program-emission shapes. Each ordinal is shared with the runtime's
    // __cajeta_xpu_register_optix_rayquery `shape` argument and with the launch
    // dispatch in cajeta_runtime.c, so the three must be kept in sync.
    enum class OptixRqShape {
        Unsupported  = -1,
        CountAabb    = 0,   // AABB candidate count: anyhit increments + ignores
        NearestTri   = 1,   // triangle nearest-hit: closesthit commits T/prim
        BaryCandidate = 2,  // triangle candidate getters: anyhit reads T/bary + ignores
        CommittedTri  = 3,  // triangle committed per-launch: closesthit writes hit/front-face
    };

    // Classifies a ray-query kernel from its proceed-loop body plus signature.
    // Anything outside the four canonical shapes is Unsupported, which leaves the
    // kernel registered on its ordinary software-BVH cubin.
    OptixRqShape classifyRayQueryShape(const MethodPtr& method);

    // Count-shape `params` global, packed in this order and matched field for field
    // by the runtime struct: handle u64, originX/originY/originZ u64, out u64, n u32,
    // boxes u64 — `boxes` is no kernel arg, the OptiX glue retains and passes it.

    // Emits the count-shape program set for `method` into `optixModule` (already
    // configured for NVPTX): __raygen__/__intersection__/__anyhit__/__miss__ plus
    // the `params` global. Returns the raygen name; throws XPU-N04 off the shape.
    std::string emitOptixCountModule(const MethodPtr& method,
                                     llvm::Module& optixModule);

    // Nearest-hit `params` global: handle u64, outT u64, outI u64; ray baked in.

    // Emits the triangle nearest-hit program set for `method`: __raygen__ (one
    // baked ray, built-in triangle traversal), __closesthit__ (T / type / primitive
    // index), __miss__. Returns the raygen name; throws XPU-N04 off the shape.
    std::string emitOptixNearestModule(const MethodPtr& method,
                                       llvm::Module& optixModule);

    // Candidate-getter `params` global: handle u64, out u64; ray baked into raygen.

    // Emits the triangle candidate-getter program set for `method`: __anyhit__
    // writes out[0..2] = t,u,v then ignores the intersection so enumeration
    // continues; no closesthit. Returns the raygen name; throws XPU-N04 off shape.
    std::string emitOptixBaryModule(const MethodPtr& method,
                                    llvm::Module& optixModule);

    // Committed-triangle `params` global, packed in this order: handle u64, b0 u64,
    // b1 u64, out u64, n u32. Each ray component resolves from initialize() as
    // either a compile-time constant or a `b0[i]`/`b1[i]` load on the launch index.

    // Emits the committed-triangle per-launch program set for `method`: __raygen__
    // traces one ray per index i<n, __closesthit__ writes out[i] (front-face 1/2
    // when the body reads committedFrontFace, else 1), __miss__ writes 0.
    std::string emitOptixCommittedTriModule(const MethodPtr& method,
                                            llvm::Module& optixModule);

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
