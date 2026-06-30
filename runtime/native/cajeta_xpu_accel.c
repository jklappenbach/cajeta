// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// --- Noun seam: the resource-provider SPI (cajeta-gpu inc-4 brick #2) --------
//
// The first-class mirror of the verb seam (LoweringTarget): a struct of build/
// free hooks, one instance per backend, through which a core noun is built from
// its description. This is the machinery a vendor extension implements for a
// noun (VendorExtensionSDK.md §2) — build-from-description, not convert-between-
// builts. Dogfooded on AccelerationStructure: the seam-defining noun
// (CajetaGPU.md §4) and the only noun with impl divergence (software BVH vs
// native BLAS). Buffer/Texture/Image have one impl per backend, so their slots
// are reserved here but stay on their existing switch dispatch (no tag would be
// meaningful). Each build reports the CajetaAsImpl it used; the noun records it;
// free follows the RECORDED impl, not the active backend.
#include "cajeta_noun_impl.h"

// OptiX AS runtime glue (src/cajeta/xpu/nvidia/OptixAccel.cpp). Resolved at JIT
// link time via the process-symbol generator (the TLS-engine pattern); stubs
// returning 0 when cajeta was built without the OptiX SDK, so this always links.
extern int     cajeta_xpu_optix_available(void);
extern int64_t cajeta_xpu_optix_accel_build_aabbs(const float* boxes, uint32_t count);
extern int64_t cajeta_xpu_optix_accel_build_triangles(const float* verts,
                                                      uint32_t triCount, uint32_t stride);
extern void    cajeta_xpu_optix_accel_free(int64_t handle);
extern uint64_t cajeta_xpu_optix_traversable(int64_t handle);
extern uint64_t cajeta_xpu_optix_accel_boxes(int64_t handle);
extern int      cajeta_xpu_optix_launch(const char* ptx, uint64_t ptxLen,
                                        const char* raygenName, const char* isName,
                                        const char* anyhitName, const char* missName,
                                        const void* paramsHost, uint64_t paramsLen,
                                        uint32_t width);
extern int      cajeta_xpu_optix_launch_tri(const char* ptx, uint64_t ptxLen,
                                            const char* raygenName,
                                            const char* closesthitName,
                                            const char* anyhitName,
                                            const char* missName,
                                            const void* paramsHost, uint64_t paramsLen,
                                            uint32_t width);

// --- OptiX ray-query program registry (M2 Phase 3-C-ii) ---------------------
// A ray-query @Kernel whose AccelerationStructure resolves to the OptiX impl can
// NOT run as a single cuLaunchKernel kernel — OptiX has no inline ray query; the
// RT cores are reached only through a program PIPELINE (raygen / intersection /
// anyhit / miss) launched via optixLaunch. NvptxRegistration emits that program
// set as a SEPARATE PTX module (the `_optix_*` asm ptxas rejects) and registers it
// here via __cajeta_xpu_register_optix_rayquery, keyed by the same name as the
// kernel's ordinary software-BVH cubin. The CUDA launch path (cajeta_xpu_launch_cuda)
// dispatches here when the active AS impl is OptiX; otherwise the software cubin runs.
// shape: 0 = AABB candidate count       (prog1=intersection, prog2=anyhit, prog3=miss);
//        1 = triangle nearest-hit        (prog1=closesthit, prog2=miss, prog3 unused);
//        2 = triangle candidate bary     (prog1=anyhit, prog2=miss, prog3 unused);
//        3 = committed-triangle per-launch (prog1=closesthit, prog2=miss, prog3 unused).
// Keep in sync with cajeta::xpu::nvidia::OptixRqShape.
struct cajeta_optix_rq {
    char name[256];
    const void* ptx;     // OptiX program PTX text (an embedded host constant)
    uint64_t ptxLen;
    int32_t shape;
    char raygen[256];
    char prog1[256];
    char prog2[256];
    char prog3[256];
};
#define CAJETA_XPU_MAX_OPTIX_RQ 32
static struct cajeta_optix_rq g_optix_rq[CAJETA_XPU_MAX_OPTIX_RQ];
static int g_optix_rq_count;

// Registration ctor entry point (NvptxRegistration emits the call at module init).
void __cajeta_xpu_register_optix_rayquery(const char* name, const void* ptx,
                                          uint64_t ptxLen, int32_t shape,
                                          const char* raygen, const char* prog1,
                                          const char* prog2, const char* prog3) {
    if (!name || !ptx || ptxLen == 0) return;
    struct cajeta_optix_rq* e = NULL;
    for (int i = 0; i < g_optix_rq_count; ++i)
        if (strcmp(g_optix_rq[i].name, name) == 0) { e = &g_optix_rq[i]; break; }
    if (!e) {
        if (g_optix_rq_count >= CAJETA_XPU_MAX_OPTIX_RQ) return;  // last-writer-wins on overflow drop
        e = &g_optix_rq[g_optix_rq_count++];
    }
    snprintf(e->name,   sizeof(e->name),   "%s", name);
    e->ptx = ptx; e->ptxLen = ptxLen; e->shape = shape;
    snprintf(e->raygen, sizeof(e->raygen), "%s", raygen ? raygen : "");
    snprintf(e->prog1,  sizeof(e->prog1),  "%s", prog1 ? prog1 : "");
    snprintf(e->prog2,  sizeof(e->prog2),  "%s", prog2 ? prog2 : "");
    snprintf(e->prog3,  sizeof(e->prog3),  "%s", prog3 ? prog3 : "");
}

static struct cajeta_optix_rq* cajeta_xpu_find_optix_rq(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < g_optix_rq_count; ++i)
        if (strcmp(g_optix_rq[i].name, name) == 0) return &g_optix_rq[i];
    return NULL;
}

// Native inline ray query available on the active device? (Same condition as
// __cajeta_xpu_device_supports(RayQueryNative).) The native-vs-software input.
static int caj_native_rayquery_available(void) {
#if defined(CAJETA_RT_HAS_VULKAN)
    // No Win32 gate: native ray query is available wherever the Vulkan device
    // advertises it (the RTX 4090's Windows Vulkan driver does). This MUST agree
    // with __cajeta_xpu_device_supports(RayQueryNative) — same condition — so the
    // capability query and the AS-impl resolver never disagree on one device.
    return (cajeta_xpu_active_backend() == CAJ_XPU_VULKAN && g_xpu_vk.rayQuery) ? 1 : 0;
#else
    return 0;
#endif
}

// Resolve an AS impl preference (CajetaAsPref) to a concrete impl (inc-4 brick #3).
// Precedence: the CAJETA_GPU_AS_IMPL env override wins, then the explicit
// preference, then the AUTO default policy. Read once per call (constant within a
// run), so the build's choice and the recorded impl always agree. A NATIVE request
// with no native support falls back to the software floor (core always runs).
// This is the RUNTIME-NOUN instance of the CAJETA_GPU_<FEATURE>_IMPL degrade-
// override convention (inc-4 brick #4); the compile-time-feature instance is
// resolveImplTier() in src/cajeta/xpu/lowering/KernelLowering.cpp (e.g.
// CAJETA_GPU_COOPMATRIX_IMPL). Same precedence + case-sensitive string match.
// CUDA's native AS tier is OptiX (RT cores), resolved separately from Vulkan's
// because the "native impl" ordinal differs (CAJ_AS_IMPL_OPTIX vs _VULKAN_NATIVE)
// and the availability probe is different (cajeta_xpu_optix_available, not the
// Vulkan ray-query flag). M1 NOTE: AUTO on CUDA stays SOFTWARE — the OptiX *verb*
// (a kernel traversing the AS via optixTrace) lands in M2; until then an OptiX AS
// is opt-in (AsImpl.Native / CAJETA_GPU_AS_IMPL=optix|native) and not yet consumed
// by a lowered kernel. Flipping AUTO to OPTIX before M2 would hand the software
// walk an OptiX handle it would misread as a buffer.
static CajetaAsImpl caj_cuda_resolve_as_impl(int pref) {
    int optix = cajeta_xpu_optix_available();
    const char* env = getenv("CAJETA_GPU_AS_IMPL");
    if (env && *env) {
        if (strcmp(env, "software") == 0) return CAJ_AS_IMPL_SOFTWARE_BVH;
        if (strcmp(env, "optix") == 0 || strcmp(env, "native") == 0)
            return optix ? CAJ_AS_IMPL_OPTIX : CAJ_AS_IMPL_SOFTWARE_BVH;
        // unknown value: ignore; fall through to the explicit preference.
    }
    if (pref == CAJ_AS_PREF_SOFTWARE) return CAJ_AS_IMPL_SOFTWARE_BVH;
    // NATIVE and NATIVE_NO_FLOOR both prefer OptiX; they differ only in whether the
    // build keeps the software FLOOR (see caj_cuda_accel_build_aabbs), not the impl tag.
    if (pref == CAJ_AS_PREF_NATIVE || pref == CAJ_AS_PREF_NATIVE_NO_FLOOR)
        return optix ? CAJ_AS_IMPL_OPTIX : CAJ_AS_IMPL_SOFTWARE_BVH;
    return CAJ_AS_IMPL_SOFTWARE_BVH;   // AUTO — software floor is the build-time primary
}

static CajetaAsImpl caj_resolve_as_impl(int pref) {
    // Backend-aware: CUDA resolves to the OptiX tier, Vulkan to its native BLAS,
    // everything else to the portable floor. Keyed on the active backend so the
    // recorded impl (implTag) and the build path always agree on one device.
    if (cajeta_xpu_active_backend() == CAJ_XPU_CUDA)
        return caj_cuda_resolve_as_impl(pref);
    int native = caj_native_rayquery_available();
    const char* env = getenv("CAJETA_GPU_AS_IMPL");
    if (env && *env) {
        if (strcmp(env, "software") == 0) return CAJ_AS_IMPL_SOFTWARE_BVH;
        if (strcmp(env, "native") == 0)
            return native ? CAJ_AS_IMPL_VULKAN_NATIVE : CAJ_AS_IMPL_SOFTWARE_BVH;
        // unknown value: ignore; fall through to the explicit preference.
    }
    if (pref == CAJ_AS_PREF_SOFTWARE) return CAJ_AS_IMPL_SOFTWARE_BVH;
    // Vulkan native is a single rep (no separate software floor to drop), so
    // NATIVE_NO_FLOOR behaves exactly like NATIVE here — the floor-drop is CUDA-only.
    if (pref == CAJ_AS_PREF_NATIVE || pref == CAJ_AS_PREF_NATIVE_NO_FLOOR)
        return native ? CAJ_AS_IMPL_VULKAN_NATIVE : CAJ_AS_IMPL_SOFTWARE_BVH;
    return caj_default_as_impl(native);   // AUTO
}

typedef struct CajetaNounProvider {
    const char*  name;
    int          backend_id;
    // AccelerationStructure noun (wired). `pref` is the CajetaAsPref override;
    // out_impl reports the impl the build actually chose (after resolution).
    int64_t      (*accel_build_aabbs)(const float* boxes, uint32_t count,
                                      int32_t pref, CajetaAsImpl* out_impl);
    int64_t      (*accel_build_triangles)(const float* verts, uint32_t triCount,
                                          uint32_t stride, CajetaAsImpl* out_impl);
    void         (*accel_free)(int64_t handle, CajetaAsImpl impl);
    // Buffer / Texture / Image noun slots: reserved (the unified contract);
    // routed by their existing dispatchers until they gain impl divergence.
} CajetaNounProvider;

// CPU provider — the portable software BVH (the floor; handle == host blob ptr).
// The CPU backend has only the software impl, so `pref` is moot here.
static int64_t caj_cpu_accel_build_aabbs(const float* boxes, uint32_t count,
                                         int32_t pref, CajetaAsImpl* out_impl) {
    (void) pref;
    if (out_impl) *out_impl = CAJ_AS_IMPL_SOFTWARE_BVH;
    return cajeta_xpu_cpu_accel_build_aabbs(boxes, count);
}
static int64_t caj_cpu_accel_build_triangles(const float* verts, uint32_t triCount,
                                             uint32_t stride, CajetaAsImpl* out_impl) {
    if (out_impl) *out_impl = CAJ_AS_IMPL_SOFTWARE_BVH;
    return cajeta_xpu_cpu_accel_build_triangles(verts, triCount, stride);
}
static void caj_cpu_accel_free(int64_t handle, CajetaAsImpl impl) {
    (void) impl;
    free((void*) (intptr_t) handle);
}

static const CajetaNounProvider caj_cpu_noun_provider = {
    "cpu", CAJ_XPU_CPU,
    caj_cpu_accel_build_aabbs, caj_cpu_accel_build_triangles,
    caj_cpu_accel_free,
};

// Vulkan provider — native VK_KHR_acceleration_structure BLAS, or (forced/auto-
// software) the portable software BVH uploaded into a storage buffer the "<name>$sw"
// kernel variant reads as bvh[i]. The resolved impl drives both.
static int64_t caj_vk_accel_build_aabbs(const float* boxes, uint32_t count,
                                        int32_t pref, CajetaAsImpl* out_impl) {
    CajetaAsImpl impl = caj_resolve_as_impl(pref);
    if (out_impl) *out_impl = impl;
    if (impl == CAJ_AS_IMPL_SOFTWARE_BVH) {
        int64_t blob = cajeta_xpu_cpu_accel_build_aabbs(boxes, count);  // host blob
        if (!blob) return 0;
        const float* hdr = (const float*) (intptr_t) blob;
        uint64_t bytes = (uint64_t) caj_bvh_block_words(hdr) * 4u;
        int64_t buf = cajeta_xpu_vk_alloc(bytes);
        if (buf) {
            void* m = cajeta_xpu_vk_mapped(buf);
            if (m) memcpy(m, hdr, (size_t) bytes);
            else { cajeta_xpu_vk_free(buf); buf = 0; }
        }
        free((void*) (intptr_t) blob);
        return buf;
    }
    return cajeta_xpu_vk_accel_build_aabbs(boxes, count);  // native BLAS
}
static int64_t caj_vk_accel_build_triangles(const float* verts, uint32_t triCount,
                                            uint32_t stride, CajetaAsImpl* out_impl) {
    // Follow the resolved impl, exactly like the AABB path: software → build the
    // portable host BVH and upload it to a storage buffer the "$sw" kernel reads;
    // native → the VK_GEOMETRY_TYPE_TRIANGLES_KHR BLAS+TLAS traced via OpRayQuery.
    // AUTO resolves to native on any ray-query-capable Vulkan device (Windows
    // included — caj_native_rayquery_available is no longer Win32-gated), and to
    // software on a non-RT device. (The triangle ctor carries no pref override, so
    // AUTO — same default the noun records.)
    CajetaAsImpl impl = caj_resolve_as_impl(CAJ_AS_PREF_AUTO);
    if (out_impl) *out_impl = impl;
    if (impl == CAJ_AS_IMPL_SOFTWARE_BVH) {
        int64_t blob = cajeta_xpu_cpu_accel_build_triangles(verts, triCount, stride);
        if (!blob) return 0;
        const float* hdr = (const float*) (intptr_t) blob;
        uint64_t bytes = (uint64_t) caj_bvh_block_words(hdr) * 4u;
        int64_t buf = cajeta_xpu_vk_alloc(bytes);
        if (buf) {
            void* m = cajeta_xpu_vk_mapped(buf);
            if (m) memcpy(m, hdr, (size_t) bytes);
            else { cajeta_xpu_vk_free(buf); buf = 0; }
        }
        free((void*) (intptr_t) blob);
        return buf;
    }
    return cajeta_xpu_vk_accel_build_triangles(verts, triCount, stride);  // native
}
static void caj_vk_accel_free(int64_t handle, CajetaAsImpl impl) {
    // Free follows the recorded impl: a software BVH is a storage buffer; a native
    // BLAS is an accel-table entry.
    if (impl == CAJ_AS_IMPL_SOFTWARE_BVH) cajeta_xpu_vk_free(handle);
    else cajeta_xpu_vk_accel_free(handle);
}

static const CajetaNounProvider caj_vk_noun_provider = {
    "vulkan", CAJ_XPU_VULKAN,
    caj_vk_accel_build_aabbs, caj_vk_accel_build_triangles,
    caj_vk_accel_free,
};

// CUDA provider — NVIDIA has no cajeta native inline ray-query seam (RT cores are
// reached via OptiX, not the NVPTX device path), so the AS is ALWAYS the portable
// software BVH: build the host blob (the shared CPU builder) and upload it into a
// CUDA device buffer the kernel reads as bvh[i]. The NVPTX kernel is lowered with
// the SoftwareRayQuery walk under its base name (NvptxTarget.accelImpl() ==
// SoftwareBvh), so — unlike Vulkan's $sw twin — there is no separate variant to
// select; the AS POD's deviceHandle (offset 0) is the device pointer the launch
// passes through as the buffer arg. Mirrors caj_vk_accel_build_aabbs' software arm
// with cuMemAlloc/cuMemcpyHtoD. (HIP is the symmetric follow-up.)
static int64_t caj_cuda_accel_upload_blob(int64_t blob) {
    if (!blob) return 0;
    if (!g_xpu_cuda.cuMemAlloc || !g_xpu_cuda.cuMemcpyHtoD) {
        free((void*) (intptr_t) blob);
        return 0;
    }
    const float* hdr = (const float*) (intptr_t) blob;
    uint64_t bytes = (uint64_t) caj_bvh_block_words(hdr) * 4u;
    cajeta_cudeviceptr dev = 0;
    if (g_xpu_cuda.cuMemAlloc(&dev, (size_t) bytes) != 0 || !dev) {
        free((void*) (intptr_t) blob);
        return 0;
    }
    if (g_xpu_cuda.cuMemcpyHtoD(dev, hdr, (size_t) bytes) != 0) {
        if (g_xpu_cuda.cuMemFree) g_xpu_cuda.cuMemFree(dev);
        free((void*) (intptr_t) blob);
        return 0;
    }
    free((void*) (intptr_t) blob);
    return (int64_t) dev;
}
// --- M3 Phase 1: multi-impl AS secondary-representation registry ------------
// An AccelerationStructure may carry a SECONDARY representation alongside its
// primary (the POD's deviceHandle). Under CAJETA_GPU_AS_IMPL=optix the primary is
// the OptiX AS and the secondary is the portable software-BVH FLOOR (uploaded to a
// device buffer), so M3's launch-time selection can fall back to software for an
// Unsupported-shape kernel instead of faulting on the OptiX handle. Keyed by the
// primary handle; entries are removed on free. (CUDA-only for now; the model
// generalizes in M3 Phase 5.) Guarded by a dedicated lock — registration runs on
// the build thread, lookup on launch/free.
struct caj_as_secondary { int64_t primary; int32_t secImpl; int64_t secHandle; };
#define CAJ_AS_SEC_MAX 256
static struct caj_as_secondary g_as_sec[CAJ_AS_SEC_MAX];
static int g_as_sec_count;
static pthread_mutex_t g_as_sec_lock = PTHREAD_MUTEX_INITIALIZER;

static void caj_as_sec_register(int64_t primary, int32_t secImpl, int64_t secHandle) {
    if (!primary || !secHandle) return;
    pthread_mutex_lock(&g_as_sec_lock);
    if (g_as_sec_count < CAJ_AS_SEC_MAX) {
        g_as_sec[g_as_sec_count].primary   = primary;
        g_as_sec[g_as_sec_count].secImpl   = secImpl;
        g_as_sec[g_as_sec_count].secHandle = secHandle;
        g_as_sec_count++;
    }
    pthread_mutex_unlock(&g_as_sec_lock);
}
static int caj_as_sec_lookup(int64_t primary, int32_t* secImpl, int64_t* secHandle) {
    int found = 0;
    pthread_mutex_lock(&g_as_sec_lock);
    for (int i = 0; i < g_as_sec_count; i++)
        if (g_as_sec[i].primary == primary) {
            if (secImpl)   *secImpl   = g_as_sec[i].secImpl;
            if (secHandle) *secHandle = g_as_sec[i].secHandle;
            found = 1; break;
        }
    pthread_mutex_unlock(&g_as_sec_lock);
    return found;
}
static int caj_as_sec_remove(int64_t primary, int32_t* secImpl, int64_t* secHandle) {
    int found = 0;
    pthread_mutex_lock(&g_as_sec_lock);
    for (int i = 0; i < g_as_sec_count; i++)
        if (g_as_sec[i].primary == primary) {
            if (secImpl)   *secImpl   = g_as_sec[i].secImpl;
            if (secHandle) *secHandle = g_as_sec[i].secHandle;
            g_as_sec[i] = g_as_sec[--g_as_sec_count];   // swap-remove
            found = 1; break;
        }
    pthread_mutex_unlock(&g_as_sec_lock);
    return found;
}

// --- M3 Phase 3: lazy native build — retained geometry + lazy OptiX resolver ----
// Under AUTO the build records the portable software BVH as the PRIMARY and does NOT
// build the (expensive) OptiX rep — that is deferred to the first supported-shape
// launch against this AS (R4: only pay for OptiX if a native consumer actually runs).
// To rebuild OptiX on demand we must retain the source geometry; this registry holds a
// host COPY keyed by the primary (software) handle. kind 0 = AABBs (count*6 floats),
// 1 = triangle soup (triCount*3*stride floats). Freed when the OptiX rep is built (no
// longer needed) or at AS free. Forced =optix builds OptiX eagerly and never retains
// here; forced =software is ineligible and never retains (no lazy OptiX).
struct caj_as_geom { int64_t primary; int32_t kind; uint32_t count; uint32_t stride;
                     float* data; uint64_t nfloats; };
#define CAJ_AS_GEOM_MAX 256
static struct caj_as_geom g_as_geom[CAJ_AS_GEOM_MAX];
static int g_as_geom_count;
static pthread_mutex_t g_as_geom_lock = PTHREAD_MUTEX_INITIALIZER;
// Serializes lazy OptiX builds so concurrent launches against one AS build it once.
static pthread_mutex_t g_as_lazy_lock = PTHREAD_MUTEX_INITIALIZER;

static void caj_as_geom_register(int64_t primary, int32_t kind, const float* data,
                                 uint64_t nfloats, uint32_t count, uint32_t stride) {
    if (!primary || !data || !nfloats) return;
    float* copy = (float*) malloc((size_t) nfloats * sizeof(float));
    if (!copy) return;
    memcpy(copy, data, (size_t) nfloats * sizeof(float));
    pthread_mutex_lock(&g_as_geom_lock);
    if (g_as_geom_count < CAJ_AS_GEOM_MAX) {
        g_as_geom[g_as_geom_count].primary = primary;
        g_as_geom[g_as_geom_count].kind    = kind;
        g_as_geom[g_as_geom_count].count   = count;
        g_as_geom[g_as_geom_count].stride  = stride;
        g_as_geom[g_as_geom_count].data    = copy;
        g_as_geom[g_as_geom_count].nfloats = nfloats;
        g_as_geom_count++;
        copy = NULL;
    }
    pthread_mutex_unlock(&g_as_geom_lock);
    free(copy);   // registry full: drop the copy (lazy build just won't fire)
}
// Copy out the metadata + data pointer for `primary` (data still owned by the registry).
static int caj_as_geom_get(int64_t primary, struct caj_as_geom* out) {
    int found = 0;
    pthread_mutex_lock(&g_as_geom_lock);
    for (int i = 0; i < g_as_geom_count; i++)
        if (g_as_geom[i].primary == primary) { *out = g_as_geom[i]; found = 1; break; }
    pthread_mutex_unlock(&g_as_geom_lock);
    return found;
}
static void caj_as_geom_remove(int64_t primary) {
    float* data = NULL;
    pthread_mutex_lock(&g_as_geom_lock);
    for (int i = 0; i < g_as_geom_count; i++)
        if (g_as_geom[i].primary == primary) {
            data = g_as_geom[i].data;
            g_as_geom[i] = g_as_geom[--g_as_geom_count];   // swap-remove
            break;
        }
    pthread_mutex_unlock(&g_as_geom_lock);
    free(data);
}

// Lazy OptiX is eligible when the runtime has OptiX AND the policy is not forced
// software (AUTO, or anything other than =software). Forced =optix takes the eager
// build branch and never reaches the retention path; forced =software returns 0 here.
static int caj_cuda_lazy_optix_eligible(void) {
    if (!cajeta_xpu_optix_available()) return 0;
    const char* env = getenv("CAJETA_GPU_AS_IMPL");
    if (env && strcmp(env, "software") == 0) return 0;
    return 1;
}

// Resolve (build-once) the OptiX representation for an AUTO AS whose primary is the
// software BVH `primary`. Returns the OptiX handle, or 0 if none can be built (no
// retained geometry → forced software, or the OptiX build failed). Registers the built
// rep as the secondary so implSet() reports it and free releases it. Thread-safe.
static int64_t caj_cuda_as_resolve_optix(int64_t primary) {
    int32_t sImpl = 0; int64_t sH = 0;
    if (caj_as_sec_lookup(primary, &sImpl, &sH) && sImpl == CAJ_AS_IMPL_OPTIX && sH)
        return sH;                                   // already built
    pthread_mutex_lock(&g_as_lazy_lock);
    if (caj_as_sec_lookup(primary, &sImpl, &sH) && sImpl == CAJ_AS_IMPL_OPTIX && sH) {
        pthread_mutex_unlock(&g_as_lazy_lock);       // built by a racing launch
        return sH;
    }
    struct caj_as_geom g;
    int64_t h = 0;
    if (caj_as_geom_get(primary, &g) && g.data) {
        h = (g.kind == 0) ? cajeta_xpu_optix_accel_build_aabbs(g.data, g.count)
                          : cajeta_xpu_optix_accel_build_triangles(g.data, g.count,
                                                                   g.stride);
        if (h) {
            caj_as_sec_register(primary, CAJ_AS_IMPL_OPTIX, h);
            caj_as_geom_remove(primary);             // geometry no longer needed
        }
    }
    pthread_mutex_unlock(&g_as_lazy_lock);
    return h;
}

static int64_t caj_cuda_accel_build_aabbs(const float* boxes, uint32_t count,
                                          int32_t pref, CajetaAsImpl* out_impl) {
    CajetaAsImpl impl = caj_cuda_resolve_as_impl(pref);
    if (impl == CAJ_AS_IMPL_OPTIX) {
        int64_t h = cajeta_xpu_optix_accel_build_aabbs(boxes, count);
        if (h) {
            if (out_impl) *out_impl = CAJ_AS_IMPL_OPTIX;
            // M3: build the portable software FLOOR as a secondary rep (uploaded to
            // device) so the verb can fall back for an Unsupported-shape kernel — UNLESS
            // the caller dropped it via AsImpl.NativeNoFloor (Phase 3c: asserts all
            // consumers are supported native shapes, trading the safety net for memory;
            // implSet() then reports OptiX-only = 4, and an Unsupported-shape launch is
            // diagnosed + skipped rather than handed the OptixAs* handle).
            if (pref != CAJ_AS_PREF_NATIVE_NO_FLOOR) {
                int64_t floor = caj_cuda_accel_upload_blob(
                    cajeta_xpu_cpu_accel_build_aabbs(boxes, count));
                if (floor) caj_as_sec_register(h, CAJ_AS_IMPL_SOFTWARE_BVH, floor);
            }
            return h;
        }
        // OptiX build failed (or SDK absent at runtime) -> the software floor.
    }
    if (out_impl) *out_impl = CAJ_AS_IMPL_SOFTWARE_BVH;
    int64_t h = caj_cuda_accel_upload_blob(cajeta_xpu_cpu_accel_build_aabbs(boxes, count));
    // M3 Phase 3: AUTO lazy path — retain geometry so a supported-shape launch can build
    // the OptiX rep on demand (the primary stays the software floor until then).
    if (h && caj_cuda_lazy_optix_eligible())
        caj_as_geom_register(h, /*kind=aabbs*/0, boxes, (uint64_t) count * 6u, count, 0);
    return h;
}
static int64_t caj_cuda_accel_build_triangles(const float* verts, uint32_t triCount,
                                              uint32_t stride, CajetaAsImpl* out_impl) {
    CajetaAsImpl impl = caj_cuda_resolve_as_impl(CAJ_AS_PREF_AUTO);
    if (impl == CAJ_AS_IMPL_OPTIX) {
        int64_t h = cajeta_xpu_optix_accel_build_triangles(verts, triCount, stride);
        if (h) {
            if (out_impl) *out_impl = CAJ_AS_IMPL_OPTIX;
            int64_t floor = caj_cuda_accel_upload_blob(
                cajeta_xpu_cpu_accel_build_triangles(verts, triCount, stride));
            if (floor) caj_as_sec_register(h, CAJ_AS_IMPL_SOFTWARE_BVH, floor);
            return h;
        }
    }
    if (out_impl) *out_impl = CAJ_AS_IMPL_SOFTWARE_BVH;
    int64_t h = caj_cuda_accel_upload_blob(
        cajeta_xpu_cpu_accel_build_triangles(verts, triCount, stride));
    // M3 Phase 3: AUTO lazy path — retain the vertex soup (triCount*3 verts, `stride`
    // floats each) so a supported-shape launch can build the OptiX rep on demand.
    if (h && caj_cuda_lazy_optix_eligible())
        caj_as_geom_register(h, /*kind=triangles*/1, verts,
                             (uint64_t) triCount * 3u * stride, triCount, stride);
    return h;
}
static void caj_cuda_accel_free(int64_t handle, CajetaAsImpl impl) {
    if (!handle) return;
    // M3: release any registered secondary (the software floor under =optix, or the
    // lazily-built OptiX rep under AUTO) first, then any retained lazy geometry.
    int32_t secImpl; int64_t secHandle;
    if (caj_as_sec_remove(handle, &secImpl, &secHandle) && secHandle) {
        if (secImpl == CAJ_AS_IMPL_OPTIX) cajeta_xpu_optix_accel_free(secHandle);
        else if (g_xpu_cuda.cuMemFree)
            g_xpu_cuda.cuMemFree((cajeta_cudeviceptr) secHandle);
    }
    caj_as_geom_remove(handle);   // no-op if the lazy OptiX rep was already built/freed
    if (impl == CAJ_AS_IMPL_OPTIX) { cajeta_xpu_optix_accel_free(handle); return; }
    if (g_xpu_cuda.cuMemFree)
        g_xpu_cuda.cuMemFree((cajeta_cudeviceptr) handle);
}

static const CajetaNounProvider caj_cuda_noun_provider = {
    "cuda", CAJ_XPU_CUDA,
    caj_cuda_accel_build_aabbs, caj_cuda_accel_build_triangles,
    caj_cuda_accel_free,
};

// HIP/AMD provider — the symmetric twin of the CUDA arm. AMD likewise has no
// cajeta native inline ray-query seam, so AmdgpuTarget.accelImpl() == SoftwareBvh:
// build the host software BVH and upload it into a HIP device buffer the kernel
// reads as bvh[i] under its base name (no $sw twin). Mirrors caj_cuda_* with
// hipMalloc/hipMemcpyHtoD/hipFree (HIP handles are void*, cast to the int64 handle).
static int64_t caj_hip_accel_upload_blob(int64_t blob) {
    if (!blob) return 0;
    if (!g_xpu_hip.hipMalloc || !g_xpu_hip.hipMemcpyHtoD) {
        free((void*) (intptr_t) blob);
        return 0;
    }
    const float* hdr = (const float*) (intptr_t) blob;
    uint64_t bytes = (uint64_t) caj_bvh_block_words(hdr) * 4u;
    void* dev = NULL;
    if (g_xpu_hip.hipMalloc(&dev, (size_t) bytes) != 0 || !dev) {
        free((void*) (intptr_t) blob);
        return 0;
    }
    if (g_xpu_hip.hipMemcpyHtoD(dev, hdr, (size_t) bytes) != 0) {
        if (g_xpu_hip.hipFree) g_xpu_hip.hipFree(dev);
        free((void*) (intptr_t) blob);
        return 0;
    }
    free((void*) (intptr_t) blob);
    return (int64_t) (intptr_t) dev;
}
static int64_t caj_hip_accel_build_aabbs(const float* boxes, uint32_t count,
                                         int32_t pref, CajetaAsImpl* out_impl) {
    (void) pref;
    if (out_impl) *out_impl = CAJ_AS_IMPL_SOFTWARE_BVH;
    return caj_hip_accel_upload_blob(cajeta_xpu_cpu_accel_build_aabbs(boxes, count));
}
static int64_t caj_hip_accel_build_triangles(const float* verts, uint32_t triCount,
                                             uint32_t stride, CajetaAsImpl* out_impl) {
    if (out_impl) *out_impl = CAJ_AS_IMPL_SOFTWARE_BVH;
    return caj_hip_accel_upload_blob(
        cajeta_xpu_cpu_accel_build_triangles(verts, triCount, stride));
}
static void caj_hip_accel_free(int64_t handle, CajetaAsImpl impl) {
    (void) impl;
    if (handle && g_xpu_hip.hipFree)
        g_xpu_hip.hipFree((void*) (intptr_t) handle);
}

static const CajetaNounProvider caj_hip_noun_provider = {
    "hip", CAJ_XPU_HIP,
    caj_hip_accel_build_aabbs, caj_hip_accel_build_triangles,
    caj_hip_accel_free,
};

// Registry indexed by backend id. CUDA + HIP now wire the software-BVH-on-device
// provider (the AccelerationStructure noun built as a portable BVH uploaded to a
// device buffer); Vulkan picks native BLAS or forced-software; CPU is the floor.
static const CajetaNounProvider* const g_xpu_noun_providers[CAJ_XPU_COUNT] = {
    [CAJ_XPU_CUDA]   = &caj_cuda_noun_provider,
    [CAJ_XPU_HIP]    = &caj_hip_noun_provider,
    [CAJ_XPU_VULKAN] = &caj_vk_noun_provider,
    [CAJ_XPU_CPU]    = &caj_cpu_noun_provider,
};

// The provider for the active backend (the build site).
static const CajetaNounProvider* cajeta_xpu_noun_provider(void) {
    int be = cajeta_xpu_active_backend();
    if (be < 0 || be >= CAJ_XPU_COUNT) return NULL;
    return g_xpu_noun_providers[be];
}

// --- AccelerationStructure device-BVH primitives (Part C inc 3b) -------------
// Instance @Native methods on AccelerationStructure.cajeta. The leading `self`
// is the cajeta `this`, ignored — the device side is keyed on the returned
// handle. Ray query / BVH build is a Vulkan-only capability for now; other
// backends return 0 (build) / no-op (free), which the cajeta drop chain handles.

// __cajeta_xpu_accel_build_aabbs(this, aabbs, count) -> int64 handle.
// `aabbs` is a Cajeta float32[] header — { i64 count, [count x f32] data } — so
// the box floats start at offset 8 (matches __cajeta_xpu_texture_upload). Each
// box is 6 floats (minX,minY,minZ,maxX,maxY,maxZ); `count` is the box count.
// STATIC @Native (no `self`): build with an explicit CajetaAsPref override (the
// AccelerationStructure.of factory). `pref` is a CajetaAsPref ordinal.
int64_t __cajeta_xpu_accel_build_aabbs_pref(void* aabbs, uint32_t count,
                                            int32_t pref) {
    if (!aabbs || count == 0) return 0;
    const float* boxes = (const float*) ((const char*) aabbs + 8);
    const CajetaNounProvider* p = cajeta_xpu_noun_provider();
    if (!p || !p->accel_build_aabbs) return 0;  // no device AS on this backend
    CajetaAsImpl impl;                          // reported; recorded via resolve_impl
    int64_t h = p->accel_build_aabbs(boxes, count, pref, &impl);
    (void) impl;
    return h;
}

// INSTANCE @Native (the default ctor): the AUTO preference.
int64_t __cajeta_xpu_accel_build_aabbs(void* self, void* aabbs, uint32_t count) {
    (void) self;
    return __cajeta_xpu_accel_build_aabbs_pref(aabbs, count, CAJ_AS_PREF_AUTO);
}

// __cajeta_xpu_accel_build_triangles(this, vertices, triCount, stride) -> handle.
// `vertices` is a Cajeta float32[] (8-byte count prefix); a triangle soup with
// `stride` floats per vertex (3 = tight). 9 floats define triangle t at vertex
// offset (t*3+v)*stride. v1: software (CPU) path only — Vulkan triangle geometry
// is a follow-up (the Vulkan path still builds AABBs).
int64_t __cajeta_xpu_accel_build_triangles(void* self, void* vertices,
                                           uint32_t triCount, uint32_t stride) {
    (void) self;
    if (!vertices || triCount == 0) return 0;
    const float* verts = (const float*) ((const char*) vertices + 8);
    const CajetaNounProvider* p = cajeta_xpu_noun_provider();
    if (!p || !p->accel_build_triangles) return 0;
    CajetaAsImpl impl;
    int64_t h = p->accel_build_triangles(verts, triCount, stride, &impl);
    (void) impl;
    return h;
}

// STATIC @Native (no `self`): the impl a build with `pref` resolves to on the
// active backend (env > pref > default). The AccelerationStructure.of factory
// records this on the noun; the build uses the same resolver, so the recorded
// impl and the built representation always agree.
int32_t __cajeta_xpu_accel_resolve_impl(int32_t pref) {
    return (int32_t) caj_resolve_as_impl(pref);
}

// INSTANCE @Native (the default ctor records this): the AUTO resolution.
int32_t __cajeta_xpu_accel_impl(void* self) {
    (void) self;
    return (int32_t) caj_resolve_as_impl(CAJ_AS_PREF_AUTO);
}

// Free dispatches on the ACTIVE backend's provider, which branches on the RECORDED
// impl (a forced-software-on-Vulkan AS is a storage buffer, not a host pointer or
// an accel-table entry). v1: an AS is freed while the backend that built it is
// still active (no cross-backend-after-switch free).
void __cajeta_xpu_accel_free(void* self, int64_t handle, int32_t impl) {
    (void) self;
    if (!handle) return;
    const CajetaNounProvider* p = cajeta_xpu_noun_provider();
    if (p && p->accel_free) p->accel_free(handle, (CajetaAsImpl) impl);
}

// __cajeta_xpu_accel_impl_set(this, handle, primaryImpl) -> a bitmask of the
// representations this AS carries, one bit per CajetaAsImpl ordinal (1u<<impl).
// M3 Phase 1: the primary impl bit, OR'd with any registered SECONDARY rep (the
// software floor built alongside an OptiX primary). `implTag()` still reports the
// single primary; this exposes the full set the launch-time selector may choose.
int32_t __cajeta_xpu_accel_impl_set(void* self, int64_t handle, int32_t primaryImpl) {
    (void) self;
    int32_t set = (int32_t) (1u << (unsigned) primaryImpl);
    int32_t secImpl;
    if (handle && caj_as_sec_lookup(handle, &secImpl, NULL))
        set |= (int32_t) (1u << (unsigned) secImpl);
    return set;
}

// --- gfx swapchain (cajeta.gfx.Swapchain, cajeta-gfx §4.c) --------------
//
// The presentable-image-chain noun. The instance @Native convention (leading
// `self`, ignored): create is handed the opaque cajeta.ifx.Surface object (as a
// void*) plus the resolved description and returns a swapchain handle; acquire/
// present/free operate on it.
//
// HOST FLOOR ONLY (this build). The live VK_KHR_swapchain create / acquire /
// queue-present (and the headless offscreen image ring) are device-machine work
// over a real Vulkan WSI surface + a window backend (the separate cajeta-ifx-*
// repos) — none of that exists here. These stubs let the noun construct and its
// acquire/present plumb on the host so the API + frames-in-flight pacing
// (cajeta.gfx.FrameSync) are exercisable; they hold no pixels and present
// nothing. The real WSI backend replaces them on the device machine.
int64_t __cajeta_gfx_swapchain_create(void* self, void* surface, int32_t format,
                                      int32_t colorSpace, int32_t presentMode,
                                      uint32_t imageCount) {
    (void) self; (void) surface; (void) format; (void) colorSpace;
    (void) presentMode; (void) imageCount;
    // A non-null opaque token so the noun reads as constructed; the real backend
    // returns the VkSwapchainKHR / offscreen-ring pointer here.
    return (int64_t) 1;
}

uint32_t __cajeta_gfx_swapchain_acquire(void* self, int64_t handle) {
    (void) self; (void) handle;
    return 0u;   // host floor: always image 0 (no real acquire)
}

void __cajeta_gfx_swapchain_present(void* self, int64_t handle, uint32_t imageIndex) {
    (void) self; (void) handle; (void) imageIndex;   // host floor: nothing presented
}

void __cajeta_gfx_swapchain_free(void* self, int64_t handle) {
    (void) self; (void) handle;   // host floor: nothing to release
}

// Address one axis: clamp-to-edge (addressMode 0) or repeat/wrap (1). `n` > 0.
static inline int cajeta_tex_addr(int c, int n, int32_t addressMode) {
    if (addressMode == 1) {                 // repeat (wrap)
        c %= n;
        if (c < 0) c += n;
        return c;
    }
    if (c < 0) return 0;                     // clamp-to-edge
    if (c >= n) return n - 1;
    return c;
}

// A 4-lane float vector matching LLVM `<4 x float>` in the x86-64 SysV ABI
// (returned in xmm0), so the CPU sampleTexture seam can declare this symbol as
// returning `<4 x float>` and use the result as a Vector<float32,4> directly.
typedef float caj_v4f __attribute__((vector_size(16)));

// Clamp a requested mip level into [0, levels-1].
static inline int cajeta_cpu_lod(const struct cajeta_cpu_texobj* t, int lod) {
    if (lod < 0) return 0;
    if (lod >= t->levels) return t->levels - 1;
    return lod;
}

// Fetch texel (x,y) of mip level `lod` as RGBA from the DECODED float store: read
// `channels` floats from that level's sub-buffer (offset mipoff[lod], width
// mipw[lod]); missing channels default G/B = 0, A = 1. (x,y) are already addressed
// (in-bounds). Level 0 has mipoff 0, so non-mip reads are unchanged.
static inline caj_v4f cajeta_cpu_texel_lod(const struct cajeta_cpu_texobj* t,
                                           int x, int y, int lod) {
    size_t lw = t->mipw[lod];
    const float* p = t->data + t->mipoff[lod] +
                     ((size_t) y * lw + (size_t) x) * t->channels;
    caj_v4f c = { 0.0f, 0.0f, 0.0f, 1.0f };
    for (int i = 0; i < t->channels; ++i) c[i] = p[i];
    return c;
}

// CPU texture sampler — the lowering of `tex.sample(sampler, u, v)` (lod 0) and
// `tex.sampleLod(sampler, u, v, lod)`. (u, v) normalized in [0, 1]; filterMode
// 0 = nearest / 1 = bilinear; addressMode 0 = clamp / 1 = wrap. `lod` selects the
// mip level (CPU v1: nearest mip = floor(lod), clamped; fractional cross-level
// blend is a refinement). The bilinear gather uses the chosen level's dims.
caj_v4f __cajeta_xpu_cpu_tex_sample_rgba(void* texp, int32_t filterMode,
                                         int32_t addressMode, float u, float v,
                                         float lod) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4f zero = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!t || !t->data || t->w == 0 || t->h == 0) return zero;
    int L = cajeta_cpu_lod(t, (int) floorf(lod));
    int W = (int) t->mipw[L], H = (int) t->miph[L];
    if (filterMode == 0) {                   // nearest
        int x = cajeta_tex_addr((int) floorf(u * (float) W), W, addressMode);
        int y = cajeta_tex_addr((int) floorf(v * (float) H), H, addressMode);
        return cajeta_cpu_texel_lod(t, x, y, L);
    }
    // bilinear (texel-center) — blend four RGBA texels of level L
    float fx = u * (float) W - 0.5f;
    float fy = v * (float) H - 0.5f;
    int x0 = (int) floorf(fx), y0 = (int) floorf(fy);
    float dx = fx - (float) x0, dy = fy - (float) y0;
    int cx0 = cajeta_tex_addr(x0,     W, addressMode);
    int cx1 = cajeta_tex_addr(x0 + 1, W, addressMode);
    int cy0 = cajeta_tex_addr(y0,     H, addressMode);
    int cy1 = cajeta_tex_addr(y0 + 1, H, addressMode);
    caj_v4f t00 = cajeta_cpu_texel_lod(t, cx0, cy0, L);
    caj_v4f t10 = cajeta_cpu_texel_lod(t, cx1, cy0, L);
    caj_v4f t01 = cajeta_cpu_texel_lod(t, cx0, cy1, L);
    caj_v4f t11 = cajeta_cpu_texel_lod(t, cx1, cy1, L);
    caj_v4f a = t00 + (t10 - t00) * dx;
    caj_v4f b = t01 + (t11 - t01) * dx;
    return a + (b - a) * dy;
}

// CPU texelFetch — `tex.fetch(x, y)` (lod 0) / `tex.fetchLod(x, y, lod)`: the
// unfiltered, sampler-free read of the exact texel at integer (x, y) in mip level
// `lod`. Coords clamped to the level's dims defensively. G/B = 0, A = 1 for <4 ch.
caj_v4f __cajeta_xpu_cpu_tex_fetch_rgba(void* texp, int32_t x, int32_t y,
                                        int32_t lod) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4f zero = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!t || !t->data || t->w == 0 || t->h == 0) return zero;
    int L = cajeta_cpu_lod(t, lod);
    int W = (int) t->mipw[L], H = (int) t->miph[L];
    int cx = x < 0 ? 0 : (x >= W ? W - 1 : x);
    int cy = y < 0 ? 0 : (y >= H ? H - 1 : y);
    return cajeta_cpu_texel_lod(t, cx, cy, L);
}

// Integer texelFetch — the int twin (raw 32-bit bits read as i32) at mip `lod`.
typedef int32_t caj_v4i __attribute__((vector_size(16)));
caj_v4i __cajeta_xpu_cpu_tex_fetch_rgba_i32(void* texp, int32_t x, int32_t y,
                                            int32_t lod) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4i zero = { 0, 0, 0, 1 };
    if (!t || !t->data || t->w == 0 || t->h == 0) return zero;
    int L = cajeta_cpu_lod(t, lod);
    int W = (int) t->mipw[L], H = (int) t->miph[L];
    int cx = x < 0 ? 0 : (x >= W ? W - 1 : x);
    int cy = y < 0 ? 0 : (y >= H ? H - 1 : y);
    const int32_t* p = (const int32_t*) t->data + t->mipoff[L] +
                       ((size_t) cy * (size_t) W + (size_t) cx) * t->channels;
    caj_v4i c = { 0, 0, 0, 1 };
    for (int i = 0; i < t->channels; ++i) c[i] = p[i];
    return c;
}

// CPU Image2D store/load — the in-process lowering of `img.store(x, y, v)` /
// `img.load(x, y)` (the writable twin of tex.fetch). `imgp` is the host image
// record (a single-channel R32f cajeta_cpu_texobj). Bounds-guarded: an in-range
// store writes data[y*w + x], an out-of-range store is dropped and an OOB load
// returns 0 — so a stray kernel index can't corrupt host memory (the reference
// path is the safe one). LLJIT resolves these like the tex-fetch symbols.
void __cajeta_xpu_cpu_image_store(void* imgp, int32_t x, int32_t y, float v) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) imgp;
    if (!t || !t->data) return;
    if (x < 0 || y < 0 || (uint32_t) x >= t->w || (uint32_t) y >= t->h) return;
    t->data[(size_t) y * t->w + (size_t) x] = v;
}

float __cajeta_xpu_cpu_image_load(void* imgp, int32_t x, int32_t y) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) imgp;
    if (!t || !t->data) return 0.0f;
    if (x < 0 || y < 0 || (uint32_t) x >= t->w || (uint32_t) y >= t->h) return 0.0f;
    return t->data[(size_t) y * t->w + (size_t) x];
}

// --- Texture3D CPU sample/fetch ---------------------------------------------
