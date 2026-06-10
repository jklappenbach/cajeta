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

// The app's per-AS impl *preference* (inc-4 brick #3) — the in-code override that
// composes with the `CAJETA_GPU_AS_IMPL` env override. Ordinals MUST match the
// `AsImpl` enum in runtime/src/cajeta/gpu/core/AsImpl.cajeta (comment-synced).
typedef enum CajetaAsPref {
    CAJ_AS_PREF_AUTO     = 0,  // heuristic default (native if supported, else software)
    CAJ_AS_PREF_SOFTWARE = 1,  // force the portable software BVH
    CAJ_AS_PREF_NATIVE   = 2   // prefer native (falls back to software if unsupported)
} CajetaAsPref;

// The default impl an AUTO build picks: native iff the active backend offers
// native inline ray query, else the portable software BVH (the floor).
// `native_available` is the runtime's "active == Vulkan && that device advertises
// ray query." `caj_resolve_as_impl` (cajeta_runtime.c) layers the env override +
// the explicit preference on top of this; this stays the pure policy core.
static inline CajetaAsImpl caj_default_as_impl(int native_available) {
    return native_available ? CAJ_AS_IMPL_VULKAN_NATIVE : CAJ_AS_IMPL_SOFTWARE_BVH;
}

#endif  // CAJETA_NOUN_IMPL_H
