// cajeta_noun_impl.h — the noun seam's shared impl contract.
//
// CajetaAsImpl is the explicit, recorded identity of an AccelerationStructure's
// built representation (the "noun impl"). It is the single source of truth the
// three formerly-independent coupling points derive from, instead of each
// re-inferring the active backend:
//   1. compile-time verb body     — LoweringTarget::accelImpl / softwareRayQuery
//   2. runtime build + free        — CajetaNounProvider (cajeta_runtime.c)
//   3. runtime launch marshalling  — the impl/kind consistency check
//
// The C++ mirror is `enum class NounImpl` in
// src/cajeta/xpu/lowering/LoweringTarget.h; its ordinals MUST match these
// (comment-synced, like the CAJETA_KP_* constants — see KernelLowering.h). Today
// impl == backend (CPU -> software BVH, Vulkan -> native BLAS); the capability-
// heuristic brick is what lets a single backend pick either, at which point this
// recorded tag — not the active backend — drives the verb. C/C++ compatible.
#ifndef CAJETA_NOUN_IMPL_H
#define CAJETA_NOUN_IMPL_H

typedef enum CajetaAsImpl {
    CAJ_AS_IMPL_SOFTWARE_BVH  = 0,  // portable software BVH (a plain Buffer<float32>)
    CAJ_AS_IMPL_VULKAN_NATIVE = 1   // VK_KHR_acceleration_structure native BLAS
} CajetaAsImpl;

// The default impl a build picks: native iff the active backend offers native
// inline ray query, else the portable software BVH (the floor). `native_available`
// is the runtime's "active == Vulkan && that device advertises ray query." The
// heuristic brick generalizes this (e.g. force software at large radius / density).
static inline CajetaAsImpl caj_default_as_impl(int native_available) {
    return native_available ? CAJ_AS_IMPL_VULKAN_NATIVE : CAJ_AS_IMPL_SOFTWARE_BVH;
}

#endif  // CAJETA_NOUN_IMPL_H
