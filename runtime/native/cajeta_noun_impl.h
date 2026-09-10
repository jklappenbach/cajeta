// The noun seam's shared impl contract: the recorded identity of an
// AccelerationStructure's built representation. Ordinals MUST match `enum class
// NounImpl` in src/cajeta/xpu/lowering/LoweringTarget.h. C/C++ compatible.
#ifndef CAJETA_NOUN_IMPL_H
#define CAJETA_NOUN_IMPL_H

typedef enum CajetaAsImpl {
    CAJ_AS_IMPL_SOFTWARE_BVH  = 0,  // portable software BVH (a plain Buffer<float32>)
    CAJ_AS_IMPL_VULKAN_NATIVE = 1,  // VK_KHR_acceleration_structure native BLAS
    CAJ_AS_IMPL_OPTIX         = 2   // NVIDIA OptiX RT-core AS; traversed by optixTrace
} CajetaAsImpl;

// The app's per-AS impl preference, composed with the `CAJETA_GPU_AS_IMPL` env
// override. Ordinals MUST match `AsImpl` in gpu/core/AsImpl.cajeta.
typedef enum CajetaAsPref {
    CAJ_AS_PREF_AUTO           = 0,  // heuristic default (native if supported, else software)
    CAJ_AS_PREF_SOFTWARE       = 1,  // force the portable software BVH
    CAJ_AS_PREF_NATIVE         = 2,  // prefer native (falls back to software if unsupported)
    CAJ_AS_PREF_NATIVE_NO_FLOOR = 3  // as NATIVE, but the build omits the floor rep
} CajetaAsPref;

// The impl an AUTO build picks, given the runtime's "active backend advertises
// inline ray query". Pure policy core: caj_resolve_as_impl layers the env
// override and the explicit preference on top of it.
static inline CajetaAsImpl caj_default_as_impl(int native_available) {
    return native_available ? CAJ_AS_IMPL_VULKAN_NATIVE : CAJ_AS_IMPL_SOFTWARE_BVH;
}

#endif  // CAJETA_NOUN_IMPL_H
