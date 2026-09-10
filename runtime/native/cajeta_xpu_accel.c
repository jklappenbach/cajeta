// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
// --- Noun seam: one struct of build/free hooks per backend. Each build reports
// --- the CajetaAsImpl used; free follows THAT, not the active backend.
#include "cajeta_noun_impl.h"

// OptiX AS glue (xpu/nvidia/OptixAccel.cpp), JIT-linked; stubbed without the SDK.
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

// --- OptiX ray-query program registry ---------------------------------------
// An OptiX ray query is a program PIPELINE, not one cuLaunchKernel: a separate PTX
// module registered here under the software-BVH cubin's name. `shape` = OptixRqShape.
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

// Native inline ray query on the active device? MUST stay the same condition as
// __cajeta_xpu_device_supports(RayQueryNative), or query and resolver disagree.
static int caj_native_rayquery_available(void) {
#if defined(CAJETA_RT_HAS_VULKAN)
    return (cajeta_xpu_active_backend() == CAJ_XPU_VULKAN && g_xpu_vk.rayQuery) ? 1 : 0;
#else
    return 0;
#endif
}

// Resolve a CajetaAsPref on CUDA, whose native tier is OptiX: the CAJETA_GPU_AS_IMPL
// env override wins, then the preference, then AUTO — which stays SOFTWARE.
static CajetaAsImpl caj_cuda_resolve_as_impl(int pref) {
    int optix = cajeta_xpu_optix_available();
    const char* env = getenv("CAJETA_GPU_AS_IMPL");
    if (env && *env) {
        if (strcmp(env, "software") == 0) return CAJ_AS_IMPL_SOFTWARE_BVH;
        if (strcmp(env, "optix") == 0 || strcmp(env, "native") == 0)
            return optix ? CAJ_AS_IMPL_OPTIX : CAJ_AS_IMPL_SOFTWARE_BVH;
    }
    if (pref == CAJ_AS_PREF_SOFTWARE) return CAJ_AS_IMPL_SOFTWARE_BVH;
    // NATIVE and NATIVE_NO_FLOOR differ only in keeping the software FLOOR.
    if (pref == CAJ_AS_PREF_NATIVE || pref == CAJ_AS_PREF_NATIVE_NO_FLOOR)
        return optix ? CAJ_AS_IMPL_OPTIX : CAJ_AS_IMPL_SOFTWARE_BVH;
    return CAJ_AS_IMPL_SOFTWARE_BVH;   // AUTO — software floor is the build-time primary
}

// Resolve a preference on the ACTIVE backend: CUDA to the OptiX tier, Vulkan to its
// native BLAS, everything else to the portable floor. The build uses this too.
static CajetaAsImpl caj_resolve_as_impl(int pref) {
    if (cajeta_xpu_active_backend() == CAJ_XPU_CUDA)
        return caj_cuda_resolve_as_impl(pref);
    int native = caj_native_rayquery_available();
    const char* env = getenv("CAJETA_GPU_AS_IMPL");
    if (env && *env) {
        if (strcmp(env, "software") == 0) return CAJ_AS_IMPL_SOFTWARE_BVH;
        if (strcmp(env, "native") == 0)
            return native ? CAJ_AS_IMPL_VULKAN_NATIVE : CAJ_AS_IMPL_SOFTWARE_BVH;
    }
    if (pref == CAJ_AS_PREF_SOFTWARE) return CAJ_AS_IMPL_SOFTWARE_BVH;
    // Vulkan native is a single rep, so NATIVE_NO_FLOOR behaves like NATIVE.
    if (pref == CAJ_AS_PREF_NATIVE || pref == CAJ_AS_PREF_NATIVE_NO_FLOOR)
        return native ? CAJ_AS_IMPL_VULKAN_NATIVE : CAJ_AS_IMPL_SOFTWARE_BVH;
    return caj_default_as_impl(native);   // AUTO
}

typedef struct CajetaNounProvider {
    const char*  name;
    int          backend_id;
    // `pref` is the CajetaAsPref override; out_impl reports the impl chosen.
    int64_t      (*accel_build_aabbs)(const float* boxes, uint32_t count,
                                      int32_t pref, CajetaAsImpl* out_impl);
    int64_t      (*accel_build_triangles)(const float* verts, uint32_t triCount,
                                          uint32_t stride, CajetaAsImpl* out_impl);
    void         (*accel_free)(int64_t handle, CajetaAsImpl impl);
    // Buffer / Texture / Image slots: reserved; still on their own dispatchers.
} CajetaNounProvider;

// CPU provider — the portable software BVH floor; handle == host blob pointer.
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

// Vulkan provider — native BLAS, or a software BVH in a storage buffer the "$sw" twin reads.
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
    // The triangle ctor carries no pref override, so AUTO — the noun's own default.
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
// Free follows the RECORDED impl: software is a storage buffer, native an entry.
static void caj_vk_accel_free(int64_t handle, CajetaAsImpl impl) {
    if (impl == CAJ_AS_IMPL_SOFTWARE_BVH) cajeta_xpu_vk_free(handle);
    else cajeta_xpu_vk_accel_free(handle);
}

static const CajetaNounProvider caj_vk_noun_provider = {
    "vulkan", CAJ_XPU_VULKAN,
    caj_vk_accel_build_aabbs, caj_vk_accel_build_triangles,
    caj_vk_accel_free,
};

// CUDA provider — no native inline ray-query seam (RT cores are OptiX-only), so the
// AS is the software BVH in a device buffer, read under the kernel's base name.
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
// --- Multi-impl AS secondary-representation registry ------------------------
// An AS may carry a SECONDARY rep beside its primary (the software floor under an
// OptiX one), so a launch can fall back. Its own lock: builds register, launches read.
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

// --- Lazy native build: retained geometry + lazy OptiX resolver --------------
// Under AUTO the primary stays the software BVH and OptiX is deferred to the first
// supported-shape launch, so keep a host COPY: kind 0 = AABBs, 1 = triangle soup.
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
// Metadata + data pointer for `primary`; the data stays owned by the registry.
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

// Lazy OptiX is eligible with OptiX present and the policy not forced software.
static int caj_cuda_lazy_optix_eligible(void) {
    if (!cajeta_xpu_optix_available()) return 0;
    const char* env = getenv("CAJETA_GPU_AS_IMPL");
    if (env && strcmp(env, "software") == 0) return 0;
    return 1;
}

// Build-once the OptiX rep for an AUTO AS, registering it as the secondary so
// implSet() reports it and free releases it. 0 when none can be built. Thread-safe.
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
            // The software FLOOR as a secondary, unless dropped by NativeNoFloor.
            if (pref != CAJ_AS_PREF_NATIVE_NO_FLOOR) {
                int64_t floor = caj_cuda_accel_upload_blob(
                    cajeta_xpu_cpu_accel_build_aabbs(boxes, count));
                if (floor) caj_as_sec_register(h, CAJ_AS_IMPL_SOFTWARE_BVH, floor);
            }
            return h;
        }
    }
    if (out_impl) *out_impl = CAJ_AS_IMPL_SOFTWARE_BVH;
    int64_t h = caj_cuda_accel_upload_blob(cajeta_xpu_cpu_accel_build_aabbs(boxes, count));
    // AUTO lazy path: retain the geometry so a later launch can build the OptiX rep.
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
    // AUTO lazy path: retain the vertex soup for an on-demand OptiX build.
    if (h && caj_cuda_lazy_optix_eligible())
        caj_as_geom_register(h, /*kind=triangles*/1, verts,
                             (uint64_t) triCount * 3u * stride, triCount, stride);
    return h;
}
static void caj_cuda_accel_free(int64_t handle, CajetaAsImpl impl) {
    if (!handle) return;
    // Release any registered secondary first, then any retained lazy geometry.
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

// HIP/AMD provider — the CUDA arm's twin; HIP's void* handles cast to int64.
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

// Registry indexed by backend id.
static const CajetaNounProvider* const g_xpu_noun_providers[CAJ_XPU_COUNT] = {
    [CAJ_XPU_CUDA]   = &caj_cuda_noun_provider,
    [CAJ_XPU_HIP]    = &caj_hip_noun_provider,
    [CAJ_XPU_VULKAN] = &caj_vk_noun_provider,
    [CAJ_XPU_CPU]    = &caj_cpu_noun_provider,
};

static const CajetaNounProvider* cajeta_xpu_noun_provider(void) {
    int be = cajeta_xpu_active_backend();
    if (be < 0 || be >= CAJ_XPU_COUNT) return NULL;
    return g_xpu_noun_providers[be];
}

// --- AccelerationStructure device-BVH primitives -----------------------------
// @Native methods on AccelerationStructure.cajeta; a leading `self` is ignored.

// `aabbs` is a Cajeta float32[] header, so the box floats start at offset 8, six
// per box (min/max xyz). STATIC @Native: `pref` is a CajetaAsPref ordinal.
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

int64_t __cajeta_xpu_accel_build_aabbs(void* self, void* aabbs, uint32_t count) {
    (void) self;
    return __cajeta_xpu_accel_build_aabbs_pref(aabbs, count, CAJ_AS_PREF_AUTO);
}

// `vertices` is a float32[] triangle soup, `stride` floats per vertex (3 = tight).
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

// The impl a build with `pref` resolves to; the build shares this resolver.
int32_t __cajeta_xpu_accel_resolve_impl(int32_t pref) {
    return (int32_t) caj_resolve_as_impl(pref);
}

int32_t __cajeta_xpu_accel_impl(void* self) {
    (void) self;
    return (int32_t) caj_resolve_as_impl(CAJ_AS_PREF_AUTO);
}

// Free dispatches on the ACTIVE backend's provider, which branches on `impl`.
void __cajeta_xpu_accel_free(void* self, int64_t handle, int32_t impl) {
    (void) self;
    if (!handle) return;
    const CajetaNounProvider* p = cajeta_xpu_noun_provider();
    if (p && p->accel_free) p->accel_free(handle, (CajetaAsImpl) impl);
}

// A bitmask of the reps this AS carries, one bit per CajetaAsImpl ordinal
// (1u<<impl): the primary OR'd with its secondary. implTag() reports the primary.
int32_t __cajeta_xpu_accel_impl_set(void* self, int64_t handle, int32_t primaryImpl) {
    (void) self;
    int32_t set = (int32_t) (1u << (unsigned) primaryImpl);
    int32_t secImpl;
    if (handle && caj_as_sec_lookup(handle, &secImpl, NULL))
        set |= (int32_t) (1u << (unsigned) secImpl);
    return set;
}

// --- gfx swapchain (cajeta.gfx.Swapchain) ------------------------------------
// HOST FLOOR ONLY in this build: these stubs let the noun construct and its
// acquire/present plumb, but hold no pixels and present nothing.
int64_t __cajeta_gfx_swapchain_create(void* self, void* surface, int32_t format,
                                      int32_t colorSpace, int32_t presentMode,
                                      uint32_t imageCount) {
    (void) self; (void) surface; (void) format; (void) colorSpace;
    (void) presentMode; (void) imageCount;
    return (int64_t) 1;   // a non-null token, so the noun reads as constructed
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
// (returned in xmm0), so the CPU sampleTexture seam can use it as Vector<f32,4>.
typedef float caj_v4f __attribute__((vector_size(16)));

static inline int cajeta_cpu_lod(const struct cajeta_cpu_texobj* t, int lod) {
    if (lod < 0) return 0;
    if (lod >= t->levels) return t->levels - 1;
    return lod;
}

// Texel (x,y) of mip `lod` from the DECODED float store, already addressed in-bounds.
static inline caj_v4f cajeta_cpu_texel_lod(const struct cajeta_cpu_texobj* t,
                                           int x, int y, int lod) {
    size_t lw = t->mipw[lod];
    const float* p = t->data + t->mipoff[lod] +
                     ((size_t) y * lw + (size_t) x) * t->channels;
    caj_v4f c = { 0.0f, 0.0f, 0.0f, 1.0f };
    for (int i = 0; i < t->channels; ++i) c[i] = p[i];
    return c;
}

// CPU texture sampler — the lowering of `tex.sample` / `tex.sampleLod`. (u, v) are
// normalized; filterMode 0 = nearest, 1 = bilinear; `lod` picks the nearest mip.
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

// CPU texelFetch — the unfiltered texel at (x, y) in mip `lod`, coords clamped.
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

// CPU Image2D store/load — the writable twin of tex.fetch over a single-channel
// R32f record. Bounds-guarded, so a stray kernel index cannot corrupt host memory.
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
