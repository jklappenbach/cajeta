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

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
