// === Cajeta runtime fragment, TEXTUALLY #included into cajeta_runtime.c (single-TU
// === build). The dlopen'd CUDA / HIP driver bindings and the stubs the @Native
// === cajeta.xpu classes forward to: LLJIT needs every one of them to exist.

// Launch-failure count: a refused dispatch must be VISIBLE to code, not only on
// stderr. Backends bump it; the stdlib exposes it as Device.launchFailures().
static int64_t g_xpu_launch_failures;
static void cajeta_xpu_note_launch_failure(void) {
    __atomic_fetch_add(&g_xpu_launch_failures, (int64_t) 1, __ATOMIC_RELAXED);
}
int64_t __cajeta_xpu_launch_failures(void) {
    return __atomic_load_n(&g_xpu_launch_failures, __ATOMIC_RELAXED);
}

// CUDA Driver API binding (dlopen'd), backing the NVPTX device path. Mirrors
// src/cajeta/xpu/nvidia/CudaDriver.cpp but lives in the runtime bitcode so LLJIT
// resolves the symbols; an absent driver leaves every entry a graceful no-op.

#if !defined(_WIN32)
#  include <dlfcn.h>
#endif

typedef unsigned long long cajeta_cudeviceptr;

struct cajeta_cuda_api {
    int loaded;            // 0 untried, 1 ready, -1 unavailable
    void* lib;
    void* ctx;
    int device;
    int (*cuInit)(unsigned);
    int (*cuDeviceGetCount)(int*);
    int (*cuDeviceGet)(int*, int);
    int (*cuDeviceGetAttribute)(int*, int, int);
    int (*cuDeviceTotalMem)(size_t*, int);
    // R4: the PRIMARY context is a process-wide singleton shared with OptiX.
    int (*cuDevicePrimaryCtxRetain)(void**, int);
    int (*cuCtxSetCurrent)(void*);   // H9: bind the ctx to the launching thread
    int (*cuModuleLoadData)(void**, const void*);
    int (*cuModuleGetFunction)(void**, void*, const char*);
    int (*cuModuleGetGlobal)(cajeta_cudeviceptr*, size_t*, void*, const char*);
    int (*cuMemAlloc)(cajeta_cudeviceptr*, size_t);
    int (*cuMemcpyHtoD)(cajeta_cudeviceptr, const void*, size_t);
    int (*cuMemcpyDtoH)(void*, cajeta_cudeviceptr, size_t);
    int (*cuMemcpyDtoD)(cajeta_cudeviceptr, cajeta_cudeviceptr, size_t);
    int (*cuMemFree)(cajeta_cudeviceptr);
    // Pinned / unified memory; optional (pinned frees with cuMemFreeHost).
    int (*cuMemAllocManaged)(cajeta_cudeviceptr*, size_t, unsigned);
    int (*cuMemHostAlloc)(void**, size_t, unsigned);
    int (*cuMemFreeHost)(void*);
    int (*cuStreamCreate)(void**, unsigned);
    int (*cuStreamSynchronize)(void*);
    int (*cuStreamDestroy)(void*);
    int (*cuMemcpyHtoDAsync)(cajeta_cudeviceptr, const void*, size_t, void*);
    int (*cuMemcpyDtoHAsync)(void*, cajeta_cudeviceptr, size_t, void*);
    // Events; optional. cuEventQuery answers 0 when complete, 600 when not.
    int (*cuEventCreate)(void**, unsigned);
    int (*cuEventRecord)(void*, void*);
    int (*cuEventSynchronize)(void*);
    int (*cuEventQuery)(void*);
    int (*cuStreamWaitEvent)(void*, void*, unsigned);
    int (*cuEventDestroy)(void*);
    // The profiler's EVENT-tier fallback: FLOAT MILLISECONDS between two events.
    int (*cuEventElapsedTime)(float*, void*, void*);
    int (*cuLaunchKernel)(void*, unsigned, unsigned, unsigned,
                          unsigned, unsigned, unsigned, unsigned,
                          void*, void**, void**);
    int (*cuCtxSynchronize)(void);
    int (*cuArrayCreate)(void**, const void*);            // CUDA_ARRAY_DESCRIPTOR
    int (*cuArrayDestroy)(void*);
    int (*cuMemcpy2D)(const void*);                       // CUDA_MEMCPY2D
    int (*cuTexObjectCreate)(unsigned long long*, const void*, const void*,
                             const void*);
    int (*cuTexObjectDestroy)(unsigned long long);
    int (*cuSurfObjectCreate)(unsigned long long*, const void*);
    int (*cuSurfObjectDestroy)(unsigned long long);
};
static struct cajeta_cuda_api g_xpu_cuda;                       // zero-initialized
static pthread_mutex_t g_xpu_cuda_lock = PTHREAD_MUTEX_INITIALIZER;


static void* cajeta_xpu_libsym(void* lib, const char* name) {
#if defined(_WIN32)
    return (void*) GetProcAddress((HMODULE) lib, name);
#else
    return dlsym(lib, name);
#endif
}

// Portable shared-library open, the twin of cajeta_xpu_libsym (RTLD_NOW|LOCAL).
static void* cajeta_xpu_libopen(const char* name) {
#if defined(_WIN32)
    return (void*) LoadLibraryA(name);
#else
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

// The profiler's CUDA event bracket (EVENT tier): cuEventRecord and
// cuEventElapsedTime ship with the DRIVER, unlike CUPTI. Nothing here waits, and
// spans are placed against a REFERENCE event recorded beside a host timestamp.
#define CAJ_CUDA_BRACKET_MAX 256

// cuEventElapsedTime answers FLOAT MILLISECONDS, so placement decays with the gap.
#define CAJ_CUDA_ANCHOR_MAX_MS 10000.0f

/* CUDA_ERROR_NOT_READY, spelled as its value: no CUDA header is included here. */
#define CAJ_CUDA_ERROR_NOT_READY 600

typedef struct {
    int32_t in_use;
    int64_t launch_id;
    void*   ev_start;
    void*   ev_end;
} CajCudaBracket;

static CajCudaBracket  g_cuda_brackets[CAJ_CUDA_BRACKET_MAX];
static pthread_mutex_t g_cuda_bracket_lock = PTHREAD_MUTEX_INITIALIZER;
static void*           g_cuda_ref_event;       /* the anchor, device side */
static int64_t         g_cuda_ref_host_ns;     /* ... and host side */
static int32_t         g_cuda_bracket_armed;
static int64_t         g_cuda_bracket_dropped; /* pool full: host window stands */

static int caj_cuda_events_bound(void) {
    return g_xpu_cuda.cuEventCreate && g_xpu_cuda.cuEventRecord
        && g_xpu_cuda.cuEventQuery && g_xpu_cuda.cuEventElapsedTime
        && g_xpu_cuda.cuEventSynchronize && g_xpu_cuda.cuEventDestroy;
}

// How many attempts to take, and the host-bracket width good enough to stop at.
#define CAJ_CUDA_ANCHOR_TRIES        4
#define CAJ_CUDA_ANCHOR_GOOD_ENOUGH  20000   /* ns */

static int64_t g_cuda_anchor_spread_ns;

/* Establishes (or replaces) the host-clock anchor, keeping the NARROWEST of
 * several host brackets. It synchronizes, so never call it from a launch. */
static int caj_cuda_anchor_locked(void) {
    void* ev = NULL;
    void* warm = NULL;
    int64_t bestHost = 0, bestSpread = 0;
    int attempt;

    if (g_xpu_cuda.cuEventCreate(&ev, 0) != 0 || !ev) return 0;

    // Warm the path FIRST: a fresh context's first event pays lazy init as BIAS.
    if (g_xpu_cuda.cuEventCreate(&warm, 0) == 0 && warm) {
        g_xpu_cuda.cuEventRecord(warm, NULL);
        g_xpu_cuda.cuEventSynchronize(warm);
        g_xpu_cuda.cuEventDestroy(warm);
    }

    // Poll rather than synchronize, which widens the interval and biases it late.
    for (attempt = 0; attempt < CAJ_CUDA_ANCHOR_TRIES; ++attempt) {
        int64_t before, after, spread;
        int spins = 0;
        before = __cajeta_currentTimeNanos();
        if (g_xpu_cuda.cuEventRecord(ev, NULL) != 0) break;
        while (g_xpu_cuda.cuEventQuery(ev) == CAJ_CUDA_ERROR_NOT_READY) {
            if (++spins > 1000000) {
                if (g_xpu_cuda.cuEventSynchronize(ev) != 0) break;
                break;
            }
        }
        after  = __cajeta_currentTimeNanos();
        spread = after - before;
        if (spread < 0) continue;
        if (bestSpread == 0 || spread < bestSpread) {
            bestSpread = spread;
            // The midpoint of [before, after] has the least worst-case error.
            bestHost = before + spread / 2;
        }
        if (bestSpread <= CAJ_CUDA_ANCHOR_GOOD_ENOUGH) break;
    }
    if (bestSpread == 0 && bestHost == 0) {
        g_xpu_cuda.cuEventDestroy(ev);
        return 0;
    }

    if (g_cuda_ref_event) g_xpu_cuda.cuEventDestroy(g_cuda_ref_event);
    g_cuda_ref_event         = ev;
    g_cuda_ref_host_ns       = bestHost;
    g_cuda_anchor_spread_ns  = bestSpread;
    return 1;
}

/* Called from the CUDA loader with the context current, and only when the
 * profiler is armed. Every outcome reports a sentence. */
static void caj_cuda_bracket_arm(void) {
    char why[224];
    if (!__cajeta_prof_gpu_is_armed()) {
        __cajeta_prof_cuda_events_note(0,
            "profiler not armed; the CUDA event bracket costs nothing here");
        return;
    }
    if (!caj_cuda_events_bound()) {
        snprintf(why, sizeof(why),
                 "the driver exported no cuEventElapsedTime (%p) or its event "
                 "entry points are incomplete; GPU timing stays at host "
                 "submit-to-complete",
                 (void*) g_xpu_cuda.cuEventElapsedTime);
        __cajeta_prof_cuda_events_note(0, why);
        return;
    }
    pthread_mutex_lock(&g_cuda_bracket_lock);
    g_cuda_bracket_armed = caj_cuda_anchor_locked();
    pthread_mutex_unlock(&g_cuda_bracket_lock);
    if (!g_cuda_bracket_armed) {
        __cajeta_prof_cuda_events_note(0,
            "the reference event could not be recorded, so device spans could "
            "not be anchored to the host clock; GPU timing stays at host "
            "submit-to-complete");
        return;
    }
    snprintf(why, sizeof(why),
             "CUDA driver events armed (EVENT tier: device-measured durations "
             "anchored to the host clock within %lld ns; no CUDA Toolkit "
             "required)", (long long) g_cuda_anchor_spread_ns);
    __cajeta_prof_cuda_events_note(1, why);
}

/* Opens a bracket for `launchId` on `stream`; -1 when the pool is full, and the
 * launch then keeps its honest host window. */
static int caj_cuda_bracket_begin(int64_t launchId, void* stream) {
    int i, slot = -1;
    CajCudaBracket* b;
    if (!g_cuda_bracket_armed || launchId == 0) return -1;

    pthread_mutex_lock(&g_cuda_bracket_lock);
    for (i = 0; i < CAJ_CUDA_BRACKET_MAX; ++i) {
        if (!g_cuda_brackets[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        g_cuda_bracket_dropped++;
        pthread_mutex_unlock(&g_cuda_bracket_lock);
        return -1;
    }
    b = &g_cuda_brackets[slot];
    if (!b->ev_start && g_xpu_cuda.cuEventCreate(&b->ev_start, 0) != 0) b->ev_start = NULL;
    if (!b->ev_end   && g_xpu_cuda.cuEventCreate(&b->ev_end, 0)   != 0) b->ev_end   = NULL;
    if (!b->ev_start || !b->ev_end) {
        pthread_mutex_unlock(&g_cuda_bracket_lock);
        return -1;
    }
    b->in_use    = 1;
    b->launch_id = launchId;
    pthread_mutex_unlock(&g_cuda_bracket_lock);

    if (g_xpu_cuda.cuEventRecord(b->ev_start, stream) != 0) {
        pthread_mutex_lock(&g_cuda_bracket_lock);
        b->in_use = 0;
        pthread_mutex_unlock(&g_cuda_bracket_lock);
        return -1;
    }
    return slot;
}

/* Records the bracket's end event; frees the slot if the record fails. */
static void caj_cuda_bracket_end(int slot, void* stream) {
    if (slot < 0) return;
    if (g_xpu_cuda.cuEventRecord(g_cuda_brackets[slot].ev_end, stream) != 0) {
        pthread_mutex_lock(&g_cuda_bracket_lock);
        g_cuda_brackets[slot].in_use = 0;
        pthread_mutex_unlock(&g_cuda_bracket_lock);
    }
}

/* Resolves every bracket whose closing event has completed. POLLS, never waits. */
static void caj_cuda_bracket_drain(void) {
    int i, live = 0;
    float furthest = 0.0f;
    if (!g_cuda_bracket_armed) return;

    for (i = 0; i < CAJ_CUDA_BRACKET_MAX; ++i) {
        void *evs, *eve;
        int64_t id, refHost, sNs, eNs;
        float toStart = 0.0f, dur = 0.0f;
        int q, ok;

        pthread_mutex_lock(&g_cuda_bracket_lock);
        if (!g_cuda_brackets[i].in_use) {
            pthread_mutex_unlock(&g_cuda_bracket_lock);
            continue;
        }
        evs = g_cuda_brackets[i].ev_start;
        eve = g_cuda_brackets[i].ev_end;
        id  = g_cuda_brackets[i].launch_id;
        pthread_mutex_unlock(&g_cuda_bracket_lock);

        q = g_xpu_cuda.cuEventQuery(eve);
        if (q == CAJ_CUDA_ERROR_NOT_READY) { live++; continue; }
        if (q != 0) {
            /* Never becoming ready; free the slot rather than poll forever. */
            pthread_mutex_lock(&g_cuda_bracket_lock);
            g_cuda_brackets[i].in_use = 0;
            pthread_mutex_unlock(&g_cuda_bracket_lock);
            continue;
        }

        pthread_mutex_lock(&g_cuda_bracket_lock);
        ok = g_cuda_ref_event
          && g_xpu_cuda.cuEventElapsedTime(&toStart, g_cuda_ref_event, evs) == 0
          && g_xpu_cuda.cuEventElapsedTime(&dur, evs, eve) == 0;
        refHost = g_cuda_ref_host_ns;
        g_cuda_brackets[i].in_use = 0;
        pthread_mutex_unlock(&g_cuda_bracket_lock);
        if (!ok) continue;   /* the pair said nothing; the host window stands */
        if (toStart > furthest) furthest = toStart;

        sNs = refHost + (int64_t) ((double) toStart * 1000000.0);
        eNs = sNs     + (int64_t) ((double) dur     * 1000000.0);
        __cajeta_prof_cuda_bracket_resolved(id, sNs, eNs);
    }

    /* Re-anchor only with the pool EMPTY, or a bracket measures from nothing. */
    if (live == 0 && furthest > CAJ_CUDA_ANCHOR_MAX_MS) {
        pthread_mutex_lock(&g_cuda_bracket_lock);
        for (i = 0; i < CAJ_CUDA_BRACKET_MAX && !g_cuda_brackets[i].in_use; ++i) { }
        if (i == CAJ_CUDA_BRACKET_MAX) caj_cuda_anchor_locked();
        pthread_mutex_unlock(&g_cuda_bracket_lock);
    }
}

// Resolves the driver and creates a context; caller holds g_xpu_cuda_lock.
static int cajeta_xpu_cuda_init_locked(void) {
    if (g_xpu_cuda.loaded == 1) return 1;
    if (g_xpu_cuda.loaded == -1) return 0;
    g_xpu_cuda.loaded = -1;  // assume failure until everything resolves
#if defined(_WIN32)
    g_xpu_cuda.lib = cajeta_xpu_libopen("nvcuda.dll");
#else
    g_xpu_cuda.lib = cajeta_xpu_libopen("libcuda.so.1");
#endif
    if (!g_xpu_cuda.lib) return 0;
    #define CAJ_BIND(fp, nm)                                                  \
        do { *(void**)(&g_xpu_cuda.fp) = cajeta_xpu_libsym(g_xpu_cuda.lib, nm); \
             if (!g_xpu_cuda.fp) return 0; } while (0)
    CAJ_BIND(cuInit, "cuInit");
    CAJ_BIND(cuDeviceGetCount, "cuDeviceGetCount");
    CAJ_BIND(cuDeviceGet, "cuDeviceGet");
    *(void**) (&g_xpu_cuda.cuDeviceGetAttribute) =            // optional (non-fatal)
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuDeviceGetAttribute");
    *(void**) (&g_xpu_cuda.cuDeviceTotalMem) =                // optional (non-fatal)
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuDeviceTotalMem_v2");
    CAJ_BIND(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain");  // NO _v2 suffix
    CAJ_BIND(cuCtxSetCurrent, "cuCtxSetCurrent");
    CAJ_BIND(cuModuleLoadData, "cuModuleLoadData");
    CAJ_BIND(cuModuleGetFunction, "cuModuleGetFunction");
    CAJ_BIND(cuMemAlloc, "cuMemAlloc_v2");
    CAJ_BIND(cuMemcpyHtoD, "cuMemcpyHtoD_v2");
    CAJ_BIND(cuMemcpyDtoH, "cuMemcpyDtoH_v2");
    CAJ_BIND(cuMemFree, "cuMemFree_v2");
    *(void**) (&g_xpu_cuda.cuMemcpyDtoD) =                    // optional (non-fatal)
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuMemcpyDtoD_v2");
    *(void**) (&g_xpu_cuda.cuMemAllocManaged) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuMemAllocManaged");
    *(void**) (&g_xpu_cuda.cuMemHostAlloc) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuMemHostAlloc");
    *(void**) (&g_xpu_cuda.cuMemFreeHost) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuMemFreeHost");
    *(void**) (&g_xpu_cuda.cuStreamCreate) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuStreamCreate");
    *(void**) (&g_xpu_cuda.cuStreamSynchronize) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuStreamSynchronize");
    *(void**) (&g_xpu_cuda.cuStreamDestroy) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuStreamDestroy_v2");
    *(void**) (&g_xpu_cuda.cuModuleGetGlobal) =        // spec-override constant set
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuModuleGetGlobal_v2");
    *(void**) (&g_xpu_cuda.cuMemcpyHtoDAsync) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuMemcpyHtoDAsync_v2");
    *(void**) (&g_xpu_cuda.cuMemcpyDtoHAsync) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuMemcpyDtoHAsync_v2");
    *(void**) (&g_xpu_cuda.cuEventCreate) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuEventCreate");
    *(void**) (&g_xpu_cuda.cuEventRecord) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuEventRecord");
    *(void**) (&g_xpu_cuda.cuEventSynchronize) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuEventSynchronize");
    *(void**) (&g_xpu_cuda.cuEventQuery) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuEventQuery");
    *(void**) (&g_xpu_cuda.cuStreamWaitEvent) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuStreamWaitEvent");
    *(void**) (&g_xpu_cuda.cuEventDestroy) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuEventDestroy_v2");
    *(void**) (&g_xpu_cuda.cuEventElapsedTime) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuEventElapsedTime");
    *(void**) (&g_xpu_cuda.cuArrayCreate) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuArrayCreate_v2");
    *(void**) (&g_xpu_cuda.cuArrayDestroy) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuArrayDestroy");
    *(void**) (&g_xpu_cuda.cuMemcpy2D) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuMemcpy2D_v2");
    *(void**) (&g_xpu_cuda.cuTexObjectCreate) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuTexObjectCreate");
    *(void**) (&g_xpu_cuda.cuTexObjectDestroy) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuTexObjectDestroy");
    *(void**) (&g_xpu_cuda.cuSurfObjectCreate) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuSurfObjectCreate");
    *(void**) (&g_xpu_cuda.cuSurfObjectDestroy) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuSurfObjectDestroy");
    CAJ_BIND(cuLaunchKernel, "cuLaunchKernel");
    CAJ_BIND(cuCtxSynchronize, "cuCtxSynchronize");
    #undef CAJ_BIND
    if (getenv("CAJETA_XPU_DEBUG")) {
        fprintf(stderr, "cajeta.xpu.cuda[dbg]: optional symbols: "
            "AllocManaged=%p HostAlloc=%p FreeHost=%p StreamCreate=%p "
            "StreamSync=%p StreamDestroy=%p ModuleGetGlobal=%p HtoDAsync=%p "
            "DtoHAsync=%p EventCreate=%p EventRecord=%p EventSync=%p "
            "EventQuery=%p StreamWaitEvent=%p EventDestroy=%p\n",
            (void*) g_xpu_cuda.cuMemAllocManaged, (void*) g_xpu_cuda.cuMemHostAlloc,
            (void*) g_xpu_cuda.cuMemFreeHost, (void*) g_xpu_cuda.cuStreamCreate,
            (void*) g_xpu_cuda.cuStreamSynchronize, (void*) g_xpu_cuda.cuStreamDestroy,
            (void*) g_xpu_cuda.cuModuleGetGlobal, (void*) g_xpu_cuda.cuMemcpyHtoDAsync,
            (void*) g_xpu_cuda.cuMemcpyDtoHAsync, (void*) g_xpu_cuda.cuEventCreate,
            (void*) g_xpu_cuda.cuEventRecord, (void*) g_xpu_cuda.cuEventSynchronize,
            (void*) g_xpu_cuda.cuEventQuery, (void*) g_xpu_cuda.cuStreamWaitEvent,
            (void*) g_xpu_cuda.cuEventDestroy);
    }
    // CUPTI's counterpart of the rocprofiler configure below: ARMING must precede
    // the context that produces the records, or the first launches produce none.
    if (__cajeta_prof_gpu_is_armed()) {
        if (__cajeta_prof_cupti_init()) __cajeta_prof_cupti_configure();
    }
    if (g_xpu_cuda.cuInit(0) != 0) return 0;
    int count = 0;
    if (g_xpu_cuda.cuDeviceGetCount(&count) != 0 || count <= 0) return 0;
    if (g_xpu_cuda.cuDeviceGet(&g_xpu_cuda.device, 0) != 0) return 0;
    // R4: retain the per-device PRIMARY context so the runtime, the JIT runtime
    // and the OptiX glue share ONE CUcontext. Retain leaves it not current.
    if (g_xpu_cuda.cuDevicePrimaryCtxRetain(&g_xpu_cuda.ctx, g_xpu_cuda.device) != 0) return 0;
    if (g_xpu_cuda.cuCtxSetCurrent) g_xpu_cuda.cuCtxSetCurrent(g_xpu_cuda.ctx);
    // Armed HERE because the reference event needs a current context.
    caj_cuda_bracket_arm();
    g_xpu_cuda.loaded = 1;
    return 1;
}

// Thread-safe "is the device usable?" gate; after true, call sites read unlocked.
static int cajeta_xpu_cuda_ready(void) {
    int ok;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    ok = cajeta_xpu_cuda_init_locked();
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    return ok;
}

// The runtime's CUDA context (the per-device PRIMARY), or NULL; shared with OptiX.
void* cajeta_xpu_cuda_context(void) {
    if (!cajeta_xpu_cuda_ready()) return NULL;
    return g_xpu_cuda.ctx;
}

// HIP Driver API binding (dlopen'd), backing the AMDGPU path; one backend per run.
// HIP texture-object ABI mirror: the ROCm headers are never included here.
enum { CAJ_HIP_CHANNEL_SIGNED = 0 };    // hipChannelFormatKindSigned (R32I store)
enum { CAJ_HIP_CHANNEL_UNSIGNED = 1 };  // hipChannelFormatKindUnsigned (UNORM / R32UI store)
enum { CAJ_HIP_CHANNEL_FLOAT = 2 };     // hipChannelFormatKindFloat
enum { CAJ_HIP_RES_ARRAY = 0 };         // hipResourceTypeArray
enum { CAJ_HIP_RES_MIPMAPPED_ARRAY = 1 };  // hipResourceTypeMipmappedArray
enum { CAJ_HIP_ADDR_WRAP = 0, CAJ_HIP_ADDR_CLAMP = 1 };  // hipTextureAddressMode
enum { CAJ_HIP_FILTER_POINT = 0, CAJ_HIP_FILTER_LINEAR = 1 };  // filter mode
enum { CAJ_HIP_READ_ELEMENT = 0 };      // hipReadModeElementType
enum { CAJ_HIP_READ_NORMALIZED_FLOAT = 1 };  // hipReadModeNormalizedFloat (UNORM→[0,1])
enum { CAJ_HIP_MEMCPY_HTOD = 1 };       // hipMemcpyHostToDevice
enum { CAJ_HIP_MEMCPY_DTOH = 2 };       // hipMemcpyDeviceToHost
// hipArray creation flags (driver_types.h; mirror the CUDA values).
enum { CAJ_HIP_ARRAY_LAYERED = 0x01 };  // hipArrayLayered (2-D array)
enum { CAJ_HIP_ARRAY_SURFACE_LOAD_STORE = 0x02 };  // hipArraySurfaceLoadStore (Image2D)
enum { CAJ_HIP_ARRAY_CUBEMAP = 0x04 };  // hipArrayCubemap (6-face cube)

struct caj_hip_channel_format_desc { int x, y, z, w; int f; };
struct caj_hip_resource_desc {
    int resType;
    union {
        struct { void* array; } array;
        struct { void* mipmap; } mipmap;
        struct { void* devPtr; struct caj_hip_channel_format_desc desc;
                 size_t sizeInBytes; } linear;
        struct { void* devPtr; struct caj_hip_channel_format_desc desc;
                 size_t width, height, pitchInBytes; } pitch2D;
    } res;
};
struct caj_hip_texture_desc {
    int addressMode[3];
    int filterMode;
    int readMode;
    int sRGB;
    float borderColor[4];
    int normalizedCoords;
    unsigned int maxAnisotropy;
    int mipmapFilterMode;
    float mipmapLevelBias;
    float minMipmapLevelClamp;
    float maxMipmapLevelClamp;
};
// 3-D array ABI mirrors (Texture3D). Byte-exact with HIP driver_types.h.
struct caj_hip_extent { size_t w, h, d; };          // hipExtent {width,height,depth}
struct caj_hip_pos { size_t x, y, z; };             // hipPos
struct caj_hip_pitched_ptr {                        // hipPitchedPtr
    void* ptr; size_t pitch; size_t xsize; size_t ysize;
};
struct caj_hip_memcpy3d_parms {                     // hipMemcpy3DParms
    void* srcArray;
    struct caj_hip_pos srcPos;
    struct caj_hip_pitched_ptr srcPtr;
    void* dstArray;
    struct caj_hip_pos dstPos;
    struct caj_hip_pitched_ptr dstPtr;
    struct caj_hip_extent extent;
    int kind;
};

struct cajeta_hip_api {
    int loaded;             // 0 untried, 1 ready, -1 unavailable
    void* lib;
    int device;
    int (*hipInit)(unsigned);
    int (*hipGetDeviceCount)(int*);
    int (*hipSetDevice)(int);
    int (*hipModuleLoadData)(void**, const void*);
    int (*hipModuleGetFunction)(void**, void*, const char*);
    int (*hipModuleGetGlobal)(void**, size_t*, void*, const char*);
    int (*hipMalloc)(void**, size_t);
    int (*hipMemcpyHtoD)(void*, const void*, size_t);
    int (*hipMemcpyDtoH)(void*, void*, size_t);
    int (*hipFree)(void*);
    int (*hipModuleLaunchKernel)(void*, unsigned, unsigned, unsigned,
                                 unsigned, unsigned, unsigned, unsigned,
                                 void*, void**, void**);
    int (*hipDeviceSynchronize)(void);
    int (*hipMallocArray)(void**, const void*, size_t, size_t, unsigned);
    int (*hipFreeArray)(void*);
    int (*hipMemcpy2DToArray)(void*, size_t, size_t, const void*, size_t,
                              size_t, size_t, int);
    int (*hipCreateTextureObject)(void**, const void*, const void*,
                                  const void*);
    int (*hipDestroyTextureObject)(void*);
    // Surface objects (Image2D); the array needs hipArraySurfaceLoadStore.
    int (*hipCreateSurfaceObject)(void**, const void*);
    int (*hipDestroySurfaceObject)(void*);
    int (*hipMemcpy2DFromArray)(void*, size_t, const void*, size_t, size_t,
                                size_t, size_t, int);
    int (*hipMalloc3DArray)(void**, const void*, struct caj_hip_extent, unsigned);
    int (*hipMemcpy3D)(const void*);
    // Mipmapped arrays; GetMipmappedArrayLevel yields one level as a hipArray.
    int (*hipMallocMipmappedArray)(void**, const void*, struct caj_hip_extent,
                                   unsigned, unsigned);
    int (*hipGetMipmappedArrayLevel)(void**, void*, unsigned);
    int (*hipFreeMipmappedArray)(void*);
    // Pinned / unified memory; managed frees with hipFree, pinned with hipHostFree.
    int (*hipMallocManaged)(void**, size_t, unsigned);
    int (*hipHostMalloc)(void**, size_t, unsigned);
    int (*hipHostFree)(void*);
    int (*hipStreamCreate)(void**);
    int (*hipStreamSynchronize)(void*);
    int (*hipStreamDestroy)(void*);
    int (*hipMemcpyHtoDAsync)(void*, const void*, size_t, void*);
    int (*hipMemcpyDtoHAsync)(void*, void*, size_t, void*);
    int (*hipEventCreate)(void**);
    int (*hipEventRecord)(void*, void*);
    int (*hipEventSynchronize)(void*);
    int (*hipEventQuery)(void*);
    int (*hipStreamWaitEvent)(void*, void*, unsigned);
    int (*hipEventDestroy)(void*);
    // Device properties (R0600 ABI), read for gcnArchName only; optional.
    int (*hipGetDevicePropertiesR0600)(void*, int);
    int (*hipDeviceGetAttribute)(int*, int, int);
    int (*hipMemcpyDtoD)(void*, void*, size_t);
    // Device memory size; on a UMA part `total` is the GTT-visible pool.
    int (*hipMemGetInfo)(size_t*, size_t*);
};
static struct cajeta_hip_api g_xpu_hip;

// The optional vendored-addrlib helper for AMD mip/cube emulation, dlopen'd.
struct caj_amdtex_layout_c {       // byte-exact mirror of caj_amdtex_layout
    uint64_t surfSize;
    uint32_t baseAlign;
    uint32_t pitch;
    uint32_t swMode;
    uint32_t levelW[16];
    uint32_t levelH[16];
    uint64_t levelOffset[16];
};
struct cajeta_amdtex_api {
    void* lib;
    int loaded;   // 0=untried, 1=ok, -1=failed
    int (*query_gfx_config)(const char*, uint32_t*, uint32_t*, uint32_t*);
    void* (*create)(uint32_t, uint32_t, uint32_t);
    void (*destroy)(void*);
    int (*mip_layout)(void*, uint32_t, uint32_t, uint32_t, uint32_t,
                      struct caj_amdtex_layout_c*);
    uint64_t (*addr_from_coord)(void*, uint32_t, uint32_t, uint32_t, uint32_t,
                                uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
};
static struct cajeta_amdtex_api g_xpu_amdtex;

// A Texture2D's AMD device handle: the array plus dims; the texobj is per launch.
struct cajeta_hip_tex {
    void* array; void* mipmap; uint32_t w, h, d; int32_t format; int levels;
    // Emulated mip path (AMD only): addrlib-tiled hipMalloc + a gfx11 image SRD.
    int emulated;
    void* devAlloc; uint64_t devBase; void* addr; void* srdBlob;
    void* stagingHost;   // persistent host copy of the tiled surface (all levels)
    struct caj_amdtex_layout_c layout;
};

// TextureFormat ordinals - MUST match xpu/core/TextureFormat.cajeta (UNORM: [0,1]).
#define CAJ_TEXFMT_R32F        0
#define CAJ_TEXFMT_R8_UNORM    1
#define CAJ_TEXFMT_RGBA8_UNORM 2
#define CAJ_TEXFMT_RGBA32F     3
#define CAJ_TEXFMT_R16F        4
#define CAJ_TEXFMT_RGBA16F     5
#define CAJ_TEXFMT_R32I        6   // 1ch 32-bit signed int   — fetch-only (raw, no convert)
#define CAJ_TEXFMT_R32UI       7   // 1ch 32-bit unsigned int — fetch-only
#define CAJ_TEXFMT_RGBA32I     8   // 4ch 32-bit signed int   — fetch-only
#define CAJ_TEXFMT_RGBA32UI    9   // 4ch 32-bit unsigned int — fetch-only

static inline int cajeta_texfmt_channels(int32_t fmt) {
    return (fmt == CAJ_TEXFMT_RGBA8_UNORM || fmt == CAJ_TEXFMT_RGBA32F ||
            fmt == CAJ_TEXFMT_RGBA16F     || fmt == CAJ_TEXFMT_RGBA32I ||
            fmt == CAJ_TEXFMT_RGBA32UI) ? 4 : 1;
}
static inline int cajeta_texfmt_is_unorm(int32_t fmt) {
    return fmt == CAJ_TEXFMT_R8_UNORM || fmt == CAJ_TEXFMT_RGBA8_UNORM;
}
// Half-float (16-bit IEEE binary16) storage formats — the cheap-HDR path.
static inline int cajeta_texfmt_is_half(int32_t fmt) {
    return fmt == CAJ_TEXFMT_R16F || fmt == CAJ_TEXFMT_RGBA16F;
}
// Raw 32-bit integer storage: stored and fetched verbatim, and fetch-only.
static inline int cajeta_texfmt_is_integer(int32_t fmt) {
    return fmt == CAJ_TEXFMT_R32I  || fmt == CAJ_TEXFMT_R32UI ||
           fmt == CAJ_TEXFMT_RGBA32I || fmt == CAJ_TEXFMT_RGBA32UI;
}
static inline int cajeta_texfmt_is_unsigned(int32_t fmt) {
    return fmt == CAJ_TEXFMT_R32UI || fmt == CAJ_TEXFMT_RGBA32UI;
}
// Bytes per channel in device storage (UNORM = 1, half = 2, float/int = 4).
static inline size_t cajeta_texfmt_channel_bytes(int32_t fmt) {
    if (cajeta_texfmt_is_unorm(fmt)) return 1u;
    if (cajeta_texfmt_is_half(fmt))  return 2u;
    return 4u;
}
static inline size_t cajeta_texfmt_texel_bytes(int32_t fmt) {
    return (size_t) cajeta_texfmt_channels(fmt) * cajeta_texfmt_channel_bytes(fmt);
}
// Quantize one float [0,1] to a UNORM byte (round-to-nearest, clamped).
static inline unsigned char cajeta_texfmt_unorm8(float f) {
    if (f <= 0.0f) return 0;
    if (f >= 1.0f) return 255;
    return (unsigned char) (f * 255.0f + 0.5f);
}
// float32 to IEEE 754 binary16 (round-to-nearest-even), as the raw bit pattern.
static inline uint16_t cajeta_f32_to_f16(float f) {
    uint32_t x;
    memcpy(&x, &f, sizeof(x));
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t  exp  = (int32_t) ((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFFu) == 0xFFu) {                 // Inf / NaN
        return (uint16_t) (sign | 0x7C00u | (mant ? 0x200u : 0u));
    }
    if (exp >= 0x1F) return (uint16_t) (sign | 0x7C00u);  // overflow → Inf
    if (exp <= 0) {                                      // subnormal / zero
        if (exp < -10) return (uint16_t) sign;
        mant |= 0x800000u;
        uint32_t shift = (uint32_t) (14 - exp);
        uint32_t half  = mant >> shift;
        uint32_t rem   = mant & ((1u << shift) - 1u);
        uint32_t mid   = 1u << (shift - 1);
        if (rem > mid || (rem == mid && (half & 1u))) half++;
        return (uint16_t) (sign | half);
    }
    uint16_t half = (uint16_t) (sign | ((uint32_t) exp << 10) | (mant >> 13));
    uint32_t rem = mant & 0x1FFFu;                       // round-to-nearest-even
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) half++;
    return half;
}
// Convert one IEEE 754 binary16 bit pattern back to float32 (exact).
static inline float cajeta_f16_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t) (h & 0x8000u) << 16;
    uint32_t exp  = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) { bits = sign; }                 // +/- zero
        else {                                          // subnormal
            exp = 127 - 15 + 1;
            while ((mant & 0x400u) == 0) { mant <<= 1; exp--; }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 0x1Fu) {                           // Inf / NaN
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}
// Encodes `texels` channel-interleaved floats into `dst` in the storage format.
static void cajeta_texfmt_encode(void* dst, const float* src, size_t texels,
                                 int32_t fmt) {
    if (cajeta_texfmt_is_unorm(fmt)) {
        unsigned char* b = (unsigned char*) dst;
        for (size_t i = 0; i < texels; ++i) b[i] = cajeta_texfmt_unorm8(src[i]);
    } else if (cajeta_texfmt_is_half(fmt)) {
        uint16_t* h = (uint16_t*) dst;
        for (size_t i = 0; i < texels; ++i) h[i] = cajeta_f32_to_f16(src[i]);
    } else {
        memcpy(dst, src, texels * sizeof(float));
    }
}

#if !defined(_WIN32)
// Loads libamdhip64, preferring canonical ROCm, and pins its libhsa-runtime first.
static void* cajeta_xpu_load_hip_from_dir(const char* dir) {
    char hsa[600], hip[600];
    snprintf(hsa, sizeof(hsa), "%s/libhsa-runtime64.so.1", dir);
    snprintf(hip, sizeof(hip), "%s/libamdhip64.so", dir);
    dlopen(hsa, RTLD_NOW | RTLD_GLOBAL);          // pin canonical HSA (best-effort)
    return dlopen(hip, RTLD_NOW | RTLD_LOCAL);
}
static void* cajeta_xpu_load_hip(void) {
    void* h = cajeta_xpu_load_hip_from_dir("/opt/rocm/lib");
    if (h) return h;
    const char* rp = getenv("ROCM_PATH");
    if (rp) {
        char dir[520];
        snprintf(dir, sizeof(dir), "%s/lib", rp);
        if ((h = cajeta_xpu_load_hip_from_dir(dir))) return h;
    }
    if ((h = dlopen("libamdhip64.so", RTLD_NOW | RTLD_LOCAL))) return h;
    return dlopen("libamdhip64.so.7", RTLD_NOW | RTLD_LOCAL);
}
#endif

// Caller holds g_xpu_cuda_lock. Idempotent via the `loaded` tri-state.
static int cajeta_xpu_hip_init_locked(void) {
    if (g_xpu_hip.loaded == 1) return 1;
    if (g_xpu_hip.loaded == -1) return 0;
    g_xpu_hip.loaded = -1;
#if defined(_WIN32)
    g_xpu_hip.lib = (void*) LoadLibraryA("amdhip64.dll");
#else
    g_xpu_hip.lib = cajeta_xpu_load_hip();
#endif
    if (!g_xpu_hip.lib) return 0;
    #define CAJ_HBIND(fp, nm)                                                  \
        do { *(void**)(&g_xpu_hip.fp) = cajeta_xpu_libsym(g_xpu_hip.lib, nm);  \
             if (!g_xpu_hip.fp) return 0; } while (0)
    CAJ_HBIND(hipInit, "hipInit");
    CAJ_HBIND(hipGetDeviceCount, "hipGetDeviceCount");
    CAJ_HBIND(hipSetDevice, "hipSetDevice");
    CAJ_HBIND(hipModuleLoadData, "hipModuleLoadData");
    CAJ_HBIND(hipModuleGetFunction, "hipModuleGetFunction");
    CAJ_HBIND(hipMalloc, "hipMalloc");
    CAJ_HBIND(hipMemcpyHtoD, "hipMemcpyHtoD");
    CAJ_HBIND(hipMemcpyDtoH, "hipMemcpyDtoH");
    CAJ_HBIND(hipFree, "hipFree");
    CAJ_HBIND(hipModuleLaunchKernel, "hipModuleLaunchKernel");
    CAJ_HBIND(hipDeviceSynchronize, "hipDeviceSynchronize");
    #undef CAJ_HBIND
    // Optional entries: a missing one disables that path, not the HIP backend.
    #define CAJ_HBIND_OPT(fp, nm)                                              \
        *(void**) (&g_xpu_hip.fp) = cajeta_xpu_libsym(g_xpu_hip.lib, nm)
    CAJ_HBIND_OPT(hipMallocArray, "hipMallocArray");
    CAJ_HBIND_OPT(hipFreeArray, "hipFreeArray");
    CAJ_HBIND_OPT(hipMemcpy2DToArray, "hipMemcpy2DToArray");
    CAJ_HBIND_OPT(hipCreateTextureObject, "hipCreateTextureObject");
    CAJ_HBIND_OPT(hipDestroyTextureObject, "hipDestroyTextureObject");
    CAJ_HBIND_OPT(hipCreateSurfaceObject, "hipCreateSurfaceObject");
    CAJ_HBIND_OPT(hipDestroySurfaceObject, "hipDestroySurfaceObject");
    CAJ_HBIND_OPT(hipMemcpy2DFromArray, "hipMemcpy2DFromArray");
    CAJ_HBIND_OPT(hipMalloc3DArray, "hipMalloc3DArray");
    CAJ_HBIND_OPT(hipMemcpy3D, "hipMemcpy3D");
    CAJ_HBIND_OPT(hipMallocMipmappedArray, "hipMallocMipmappedArray");
    CAJ_HBIND_OPT(hipGetMipmappedArrayLevel, "hipGetMipmappedArrayLevel");
    CAJ_HBIND_OPT(hipFreeMipmappedArray, "hipFreeMipmappedArray");
    CAJ_HBIND_OPT(hipMallocManaged, "hipMallocManaged");
    CAJ_HBIND_OPT(hipHostMalloc, "hipHostMalloc");
    CAJ_HBIND_OPT(hipHostFree, "hipHostFree");
    CAJ_HBIND_OPT(hipStreamCreate, "hipStreamCreate");
    CAJ_HBIND_OPT(hipStreamSynchronize, "hipStreamSynchronize");
    CAJ_HBIND_OPT(hipStreamDestroy, "hipStreamDestroy");
    CAJ_HBIND_OPT(hipMemcpyHtoDAsync, "hipMemcpyHtoDAsync");
    CAJ_HBIND_OPT(hipModuleGetGlobal, "hipModuleGetGlobal");  // spec-override set
    CAJ_HBIND_OPT(hipMemcpyDtoHAsync, "hipMemcpyDtoHAsync");
    CAJ_HBIND_OPT(hipEventCreate, "hipEventCreate");
    CAJ_HBIND_OPT(hipEventRecord, "hipEventRecord");
    CAJ_HBIND_OPT(hipEventSynchronize, "hipEventSynchronize");
    CAJ_HBIND_OPT(hipEventQuery, "hipEventQuery");
    CAJ_HBIND_OPT(hipStreamWaitEvent, "hipStreamWaitEvent");
    CAJ_HBIND_OPT(hipEventDestroy, "hipEventDestroy");
    CAJ_HBIND_OPT(hipGetDevicePropertiesR0600, "hipGetDevicePropertiesR0600");
    CAJ_HBIND_OPT(hipDeviceGetAttribute, "hipDeviceGetAttribute");
    CAJ_HBIND_OPT(hipMemcpyDtoD, "hipMemcpyDtoD");
    CAJ_HBIND_OPT(hipMemGetInfo, "hipMemGetInfo");
    #undef CAJ_HBIND_OPT
    // The one window rocprofiler can be configured in: it intercepts HIP while
    // HIP loads, so this runs after the symbols bind and BEFORE hipInit.
    if (__cajeta_prof_gpu_is_armed()) {
        if (__cajeta_prof_rocm_init()) __cajeta_prof_rocm_configure();
    }
    if (g_xpu_hip.hipInit(0) != 0) return 0;
    int count = 0;
    if (g_xpu_hip.hipGetDeviceCount(&count) != 0 || count <= 0) return 0;
    if (g_xpu_hip.hipSetDevice(g_xpu_hip.device) != 0) return 0;
    g_xpu_hip.loaded = 1;
    return 1;
}

#if !defined(_WIN32)
// Locates libcajeta_amdtex.so: env, then beside the executable, then by SONAME.
static void* cajeta_xpu_load_amdtex(void) {
    const char* env = getenv("CAJETA_AMD_AMDTEX_LIB");
    if (env && *env) { void* h = dlopen(env, RTLD_NOW | RTLD_LOCAL); if (h) return h; }
    char exe[768]; char path[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char* slash = strrchr(exe, '/'); if (slash) *slash = '\0'; else exe[0] = '\0';
        snprintf(path, sizeof(path), "%s/libcajeta_amdtex.so", exe);
        void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL); if (h) return h;
        snprintf(path, sizeof(path), "%s/../libcajeta_amdtex.so", exe);
        if ((h = dlopen(path, RTLD_NOW | RTLD_LOCAL))) return h;
        snprintf(path, sizeof(path), "%s/../lib/libcajeta_amdtex.so", exe);
        if ((h = dlopen(path, RTLD_NOW | RTLD_LOCAL))) return h;
    }
    return dlopen("libcajeta_amdtex.so", RTLD_NOW | RTLD_LOCAL);
}
#endif

// Bind the optional addrlib helper. Idempotent (tri-state). Returns 1 if usable.
static int cajeta_xpu_amdtex_init(void) {
    if (g_xpu_amdtex.loaded == 1) return 1;
    if (g_xpu_amdtex.loaded == -1) return 0;
    g_xpu_amdtex.loaded = -1;
#if defined(_WIN32)
    return 0;   // ROCm/HIP texture emulation is Linux-only for cajeta.
#else
    g_xpu_amdtex.lib = cajeta_xpu_load_amdtex();
    if (!g_xpu_amdtex.lib) return 0;
    #define CAJ_ATBIND(fp, nm)                                                  \
        do { *(void**)(&g_xpu_amdtex.fp) = dlsym(g_xpu_amdtex.lib, nm);         \
             if (!g_xpu_amdtex.fp) return 0; } while (0)
    CAJ_ATBIND(query_gfx_config, "cajeta_amdtex_query_gfx_config");
    CAJ_ATBIND(create, "cajeta_amdtex_create");
    CAJ_ATBIND(destroy, "cajeta_amdtex_destroy");
    CAJ_ATBIND(mip_layout, "cajeta_amdtex_mip_layout");
    CAJ_ATBIND(addr_from_coord, "cajeta_amdtex_addr_from_coord");
    #undef CAJ_ATBIND
    g_xpu_amdtex.loaded = 1;
    return 1;
#endif
}

// Reads this device's gfx arch token (e.g. "gfx1151") into `out`; 1 on success. It
// scans the R0600 blob rather than mirroring the drift-prone hipDeviceProp_t.
static int cajeta_xpu_hip_gfx_arch(char* out, size_t outLen) {
    if (!g_xpu_hip.hipGetDevicePropertiesR0600) return 0;
    // Over-allocate well past the real struct so the runtime can't overflow.
    unsigned char buf[4096];
    memset(buf, 0, sizeof(buf));
    if (g_xpu_hip.hipGetDevicePropertiesR0600(buf, g_xpu_hip.device) != 0) return 0;
    for (size_t i = 0; i + 4 < sizeof(buf); ++i) {
        if (buf[i] == 'g' && buf[i+1] == 'f' && buf[i+2] == 'x' &&
            buf[i+3] >= '0' && buf[i+3] <= '9') {
            size_t j = 0;
            while (i + j < sizeof(buf) && buf[i+j] &&
                   buf[i+j] != ':' && buf[i+j] != ' ' && j + 1 < outLen) {
                out[j] = (char) buf[i+j]; ++j;
            }
            out[j] = '\0';
            return j > 3;
        }
    }
    return 0;
}

// Fills *out from the CUDA driver, the twin of the HIP block below. Every ordinal
// was validated live and every read range-clamped, so a wrong one leaves it 0.
static int cajeta_xpu_cuda_fill_raw_device(CajetaXpuRawDevice* out) {
    if (!g_xpu_cuda.cuDeviceGetAttribute) return 0;
    int v = 0, dev = g_xpu_cuda.device;
    int major = 0, minor = 0;
    if (g_xpu_cuda.cuDeviceGetAttribute(&major, 75, dev) != 0 || major <= 0) return 0;
    if (g_xpu_cuda.cuDeviceGetAttribute(&minor, 76, dev) != 0 || minor <  0) return 0;
    snprintf(out->archName, sizeof(out->archName), "sm_%d", major * 10 + minor);

    v = 0;   // CU_DEVICE_ATTRIBUTE_WARP_SIZE
    if (g_xpu_cuda.cuDeviceGetAttribute(&v, 10, dev) == 0 && (v == 32 || v == 64))
        out->waveSize = (uint32_t) v;
    v = 0;   // MAX_THREADS_PER_BLOCK
    if (g_xpu_cuda.cuDeviceGetAttribute(&v, 1, dev) == 0 && v >= 1 && v <= 4096)
        out->maxThreadsPerBlock = (uint32_t) v;
    v = 0;   // MULTIPROCESSOR_COUNT (SMs; no WGP folding on NVIDIA)
    if (g_xpu_cuda.cuDeviceGetAttribute(&v, 16, dev) == 0 && v >= 1 && v <= 4096)
        out->multiprocessorCount = (uint32_t) v;
    v = 0;   // MAX_REGISTERS_PER_MULTIPROCESSOR — the live register file
    if (g_xpu_cuda.cuDeviceGetAttribute(&v, 82, dev) == 0 && v >= 1024 && v <= (1 << 22))
        out->regsPerMP = (uint32_t) v;
    v = 0;   // MAX_THREADS_PER_MULTIPROCESSOR — the live warp cap
    if (g_xpu_cuda.cuDeviceGetAttribute(&v, 39, dev) == 0 && v >= 64 && v <= 8192)
        out->threadsPerMP = (uint32_t) v;
    v = 0;   // MAX_SHARED_MEMORY_PER_MULTIPROCESSOR
    if (g_xpu_cuda.cuDeviceGetAttribute(&v, 81, dev) == 0 && v >= 1024 && v <= (1 << 20))
        out->ldsBytesPerMP = (uint32_t) v;

    // Tier-B geometry, same discipline: validated ordinals, clamped reads.
    v = 0;   // MAX_SHARED_MEMORY_PER_BLOCK
    if (g_xpu_cuda.cuDeviceGetAttribute(&v, 8, dev) == 0 && v >= 1024 && v <= (1 << 20))
        out->ldsBytesPerBlock = (uint32_t) v;
    v = 0;   // MAX_SHARED_MEMORY_PER_BLOCK_OPTIN
    if (g_xpu_cuda.cuDeviceGetAttribute(&v, 97, dev) == 0 && v >= 1024 && v <= (1 << 20))
        out->ldsBytesPerBlockOptin = (uint32_t) v;
    v = 0;   // MAX_BLOCKS_PER_MULTIPROCESSOR
    if (g_xpu_cuda.cuDeviceGetAttribute(&v, 106, dev) == 0 && v >= 1 && v <= 1024)
        out->maxBlocksPerMP = (uint32_t) v;
    v = 0;   // L2_CACHE_SIZE — the limiter a saturation target is really bound by
    if (g_xpu_cuda.cuDeviceGetAttribute(&v, 38, dev) == 0 && v >= 1024)
        out->l2CacheBytes = (uint32_t) v;
    v = 0;   // MEMORY_CLOCK_RATE (kHz)
    if (g_xpu_cuda.cuDeviceGetAttribute(&v, 36, dev) == 0 && v >= 1000)
        out->memoryClockKHz = (uint32_t) v;
    v = 0;   // GLOBAL_MEMORY_BUS_WIDTH (bits)
    if (g_xpu_cuda.cuDeviceGetAttribute(&v, 37, dev) == 0 && v >= 8 && v <= 8192)
        out->memoryBusWidthBits = (uint32_t) v;
    v = 0;   // CLOCK_RATE (kHz)
    if (g_xpu_cuda.cuDeviceGetAttribute(&v, 13, dev) == 0 && v >= 1000)
        out->clockRateKHz = (uint32_t) v;
    v = 0;   // MAX_GRID_DIM_X
    if (g_xpu_cuda.cuDeviceGetAttribute(&v, 5, dev) == 0 && v >= 1)
        out->maxGridDimX = (uint32_t) v;
    v = 0;   // MAX_BLOCK_DIM_X
    if (g_xpu_cuda.cuDeviceGetAttribute(&v, 2, dev) == 0 && v >= 1 && v <= 4096)
        out->maxBlockDimX = (uint32_t) v;
    v = 0;   // INTEGRATED — an APU, where a host<->device copy is not a transfer
    if (g_xpu_cuda.cuDeviceGetAttribute(&v, 18, dev) == 0)
        out->integrated = v ? 1 : 0;
    if (g_xpu_cuda.cuDeviceTotalMem) {
        size_t tot = 0;
        if (g_xpu_cuda.cuDeviceTotalMem(&tot, dev) == 0 && tot > 0)
            out->totalGlobalMemBytes = (uint64_t) tot;
    }
    return 1;
}

// Queries the active device into *out for the host-side DeviceModel builder; the
// arch token is the robust signal and the numeric ordinals are clamped.
int32_t cajeta_xpu_query_raw_device(CajetaXpuRawDevice* out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    const char* dis = getenv("CAJETA_XPU_DEVICE_PROFILE_DISABLE");
    if (dis && dis[0] && dis[0] != '0') return 0;

    pthread_mutex_lock(&g_xpu_cuda_lock);
    int up = cajeta_xpu_hip_init_locked();
    pthread_mutex_unlock(&g_xpu_cuda_lock);

    // No HIP: try CUDA, or every NVIDIA box reports gfx-shaped constants.
    if (!up) {
        pthread_mutex_lock(&g_xpu_cuda_lock);
        int cu = cajeta_xpu_cuda_init_locked();
        int ok = cu ? cajeta_xpu_cuda_fill_raw_device(out) : 0;
        pthread_mutex_unlock(&g_xpu_cuda_lock);
        if (!ok) return 0;
        out->valid = 1;
        return 1;
    }

    if (!cajeta_xpu_hip_gfx_arch(out->archName, sizeof(out->archName))) return 0;

    if (g_xpu_hip.hipDeviceGetAttribute) {
        int v = 0, dev = g_xpu_hip.device;
        if (g_xpu_hip.hipDeviceGetAttribute(&v, 87, dev) == 0 && (v == 32 || v == 64))
            out->waveSize = (uint32_t) v;
        v = 0;
        if (g_xpu_hip.hipDeviceGetAttribute(&v, 56, dev) == 0 && v >= 1 && v <= 4096)
            out->maxThreadsPerBlock = (uint32_t) v;
        v = 0;
        if (g_xpu_hip.hipDeviceGetAttribute(&v, 63, dev) == 0 && v >= 1 && v <= 4096)
            out->multiprocessorCount = (uint32_t) v;
        v = 0;   // MaxRegistersPerMultiprocessor — the live VGPR file
        if (g_xpu_hip.hipDeviceGetAttribute(&v, 72, dev) == 0 && v >= 1024 && v <= (1 << 22))
            out->regsPerMP = (uint32_t) v;
        v = 0;   // MaxThreadsPerMultiProcessor — the live wave cap
        if (g_xpu_hip.hipDeviceGetAttribute(&v, 57, dev) == 0 && v >= 64 && v <= 8192)
            out->threadsPerMP = (uint32_t) v;
        v = 0;   // MaxSharedMemoryPerMultiprocessor (AMD-specific ordinal)
        if (g_xpu_hip.hipDeviceGetAttribute(&v, 10002, dev) == 0 && v >= 1024 && v <= (1 << 20))
            out->ldsBytesPerMP = (uint32_t) v;
    }
    out->valid = 1;
    return 1;
}

// The CUDA half of the roofline probe, shaped like the HIP one so they compare.
static double cajeta_xpu_cuda_measure_bandwidth_gbps(uint64_t bytes, int32_t passes) {
    pthread_mutex_lock(&g_xpu_cuda_lock);
    int up = cajeta_xpu_cuda_init_locked();
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    if (!up || !g_xpu_cuda.cuMemAlloc || !g_xpu_cuda.cuMemFree ||
        !g_xpu_cuda.cuMemcpyDtoD || !g_xpu_cuda.cuCtxSynchronize) return 0.0;

    cajeta_cudeviceptr src = 0, dst = 0;
    if (g_xpu_cuda.cuMemAlloc(&src, bytes) != 0) return 0.0;
    if (g_xpu_cuda.cuMemAlloc(&dst, bytes) != 0) { g_xpu_cuda.cuMemFree(src); return 0.0; }

    g_xpu_cuda.cuMemcpyDtoD(dst, src, bytes);   // warmup
    g_xpu_cuda.cuCtxSynchronize();

    double best = 1e30;
    for (int32_t i = 0; i < passes; ++i) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (g_xpu_cuda.cuMemcpyDtoD(dst, src, bytes) != 0) { best = 1e30; break; }
        g_xpu_cuda.cuCtxSynchronize();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double sec = (double)(t1.tv_sec - t0.tv_sec) +
                     (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
        if (sec > 0.0 && sec < best) best = sec;
    }
    g_xpu_cuda.cuMemFree(src);
    g_xpu_cuda.cuMemFree(dst);
    if (best >= 1e30) return 0.0;
    return (2.0 * (double) bytes) / best / 1e9;
}

// Measures bandwidth (GB/s) from a d2d copy of `bytes` (2x traffic), best of N.
double cajeta_xpu_measure_bandwidth_gbps(uint64_t bytes, int32_t passes) {
    const char* dis = getenv("CAJETA_XPU_DEVICE_PROFILE_DISABLE");
    if (dis && dis[0] && dis[0] != '0') return 0.0;
    if (bytes == 0 || passes < 1) return 0.0;

    pthread_mutex_lock(&g_xpu_cuda_lock);
    int up = cajeta_xpu_hip_init_locked();
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    if (!up) return cajeta_xpu_cuda_measure_bandwidth_gbps(bytes, passes);
    if (!g_xpu_hip.hipMalloc || !g_xpu_hip.hipFree ||
        !g_xpu_hip.hipMemcpyDtoD || !g_xpu_hip.hipDeviceSynchronize) return 0.0;

    void* src = NULL; void* dst = NULL;
    if (g_xpu_hip.hipMalloc(&src, bytes) != 0) return 0.0;
    if (g_xpu_hip.hipMalloc(&dst, bytes) != 0) { g_xpu_hip.hipFree(src); return 0.0; }

    g_xpu_hip.hipMemcpyDtoD(dst, src, bytes);   // warmup
    g_xpu_hip.hipDeviceSynchronize();

    double best = 1e30;
    for (int32_t i = 0; i < passes; ++i) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (g_xpu_hip.hipMemcpyDtoD(dst, src, bytes) != 0) { best = 1e30; break; }
        g_xpu_hip.hipDeviceSynchronize();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double sec = (double)(t1.tv_sec - t0.tv_sec) +
                     (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
        if (sec > 0.0 && sec < best) best = sec;
    }
    g_xpu_hip.hipFree(src);
    g_xpu_hip.hipFree(dst);
    if (best >= 1e30) return 0.0;
    return (2.0 * (double) bytes) / best / 1e9;
}

// Per-kernel parameter metadata for the Vulkan launch translation: which args are
// buffers and which scalars. Scalars become transient single-element SSBOs.
#define CAJETA_KP_SCALAR       CAJETA_XPU_KP_SCALAR
#define CAJETA_KP_BUFFER       CAJETA_XPU_KP_BUFFER
#define CAJETA_KP_TEXTURE      CAJETA_XPU_KP_TEXTURE
#define CAJETA_KP_SAMPLER      CAJETA_XPU_KP_SAMPLER
#define CAJETA_KP_ACCEL        CAJETA_XPU_KP_ACCEL   // AccelerationStructure -> descriptor-bound BVH
#define CAJETA_KP_IMAGE        CAJETA_XPU_KP_IMAGE   // Image2D (writable) -> STORAGE_IMAGE descriptor
#define CAJETA_KP_BUFFER_ARRAY CAJETA_XPU_KP_BUFFER_ARRAY  // Buffer<T>[] -> bindless descriptor array

// The ABI version queryable by external FFI callers (header/runtime handshake).
int32_t __cajeta_xpu_abi_version(void) { return CAJETA_XPU_ABI_VERSION; }

struct cajeta_kparams {
    char name[256];
    int count;
    const uint8_t* kind;
    const uint32_t* byteSize;
};
// 1024 by convention, equal to CAJETA_XPU_MAX_MODULES; overflow drops kernels.
#define CAJETA_XPU_MAX_KPARAMS 1024
static struct cajeta_kparams g_xpu_kparams[CAJETA_XPU_MAX_KPARAMS];
static int g_xpu_kparam_count;

// Registers (or replaces, by name) a kernel's param metadata; arrays kept by pointer.
void __cajeta_xpu_register_kernel_params(const char* name, int32_t count,
                                         const uint8_t* kind,
                                         const uint32_t* byteSize) {
    if (!name) return;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    // Dedup by name: a re-registration overwrites rather than exhausting the table.
    int idx = -1;
    for (int i = 0; i < g_xpu_kparam_count; ++i)
        if (strncmp(g_xpu_kparams[i].name, name,
                    sizeof(g_xpu_kparams[i].name)) == 0) { idx = i; break; }
    int isNew = 0;
    if (idx < 0) {
        if (g_xpu_kparam_count >= CAJETA_XPU_MAX_KPARAMS) {
            fprintf(stderr,
                    "cajeta.xpu: kparams registry FULL (%d) — dropping "
                    "'%s'; raise CAJETA_XPU_MAX_KPARAMS\n",
                    CAJETA_XPU_MAX_KPARAMS, name);
            pthread_mutex_unlock(&g_xpu_cuda_lock);
            return;
        }
        idx = g_xpu_kparam_count;
        isNew = 1;
    }
    struct cajeta_kparams* e = &g_xpu_kparams[idx];
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    e->count = count;
    e->kind = kind;
    e->byteSize = byteSize;
    // Publish a new slot only after its fields are written (lock-free readers).
    if (isNew) g_xpu_kparam_count++;
    pthread_mutex_unlock(&g_xpu_cuda_lock);
}

static struct cajeta_kparams* cajeta_xpu_find_kparams(const char* name) {
    for (int i = 0; i < g_xpu_kparam_count; ++i)
        if (strncmp(g_xpu_kparams[i].name, name,
                    sizeof(g_xpu_kparams[i].name)) == 0)
            return &g_xpu_kparams[i];
    return NULL;
}
