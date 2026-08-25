// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// ============================================================================
// cajeta.xpu runtime stubs (CajetaXPU phases 1-2, step 2).
//
// The cajeta.xpu stdlib classes (Stream / Event / Fence / Thread /
// Workgroup / Barrier / Wave) declare their methods @Native and forward to
// the symbols below. LLJIT eagerly materializes all externs at module load
// time, so these have to exist before any XPU implementation does.
//
// Every stub returns the zero value (NULL / 0 / false) or no-ops. Calling
// any of them in v1 yields a null Stream / zero coordinate / false flag —
// not a crash. Step 7 (CPU-emulation backend) replaces these with real
// implementations: thread-local globals for the coordinate readers, a host-
// side ordered queue for Stream/Event, etc. The native and Vulkan
// backends (steps 9-11) replace call sites at codegen time so these stubs
// only fire on the CPU-emulation path.
//
// Buffer<T>'s @Native methods are generic; they emit only when a Buffer<T>
// is instantiated, so no stubs appear here until a real allocator lands.
// ============================================================================

// ============================================================================
// CUDA Driver API binding (dlopen'd) — backs the real NVPTX device path.
// ============================================================================
// Mirrors src/cajeta/xpu/nvidia/CudaDriver.cpp, but lives in the runtime
// bitcode so the LLJIT host path resolves these symbols from merged bitcode
// (the C++ CudaDriver is not visible to the JIT'd module). The driver is
// bound lazily on first use; an absent GPU/driver leaves every entry a
// graceful no-op (alloc returns 0, copies/launch return silently) so host
// code that never touches the device still links and runs.

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
    // Optional (bound non-fatally): device attribute query, used by the
    // capability probes (__cajeta_xpu_device_supports) — e.g. compute
    // capability for the bf16 tensor-core gate. Null on exotic stubs.
    int (*cuDeviceGetAttribute)(int*, int, int);
    int (*cuDevicePrimaryCtxRetain)(void**, int);  // R4: share the per-device PRIMARY
                                     // ctx (a process-wide singleton) so the runtime,
                                     // the JIT-embedded runtime, and the OptiX glue
                                     // all resolve to ONE CUcontext (RT-core interop).
    int (*cuCtxSetCurrent)(void*);   // H9: bind the ctx to the launching thread
    int (*cuModuleLoadData)(void**, const void*);
    int (*cuModuleGetFunction)(void**, void*, const char*);
    int (*cuModuleGetGlobal)(cajeta_cudeviceptr*, size_t*, void*, const char*);
    int (*cuMemAlloc)(cajeta_cudeviceptr*, size_t);
    int (*cuMemcpyHtoD)(cajeta_cudeviceptr, const void*, size_t);
    int (*cuMemcpyDtoH)(void*, cajeta_cudeviceptr, size_t);
    int (*cuMemFree)(cajeta_cudeviceptr);
    // Pinned / unified (managed) memory (Buffer MemoryKind); optional — a missing
    // entry just falls that kind back to plain cuMemAlloc/cuMemFree. Managed
    // memory (cuMemAllocManaged) is one pointer host AND device see; pinned host
    // memory (cuMemHostAlloc) is page-locked + device-accessible, freed with
    // cuMemFreeHost (managed frees with plain cuMemFree).
    int (*cuMemAllocManaged)(cajeta_cudeviceptr*, size_t, unsigned);
    int (*cuMemHostAlloc)(void**, size_t, unsigned);
    int (*cuMemFreeHost)(void*);
    // Real streams + async copies; optional (bound non-fatally; null → default
    // stream + synchronous-memcpy fallback).
    int (*cuStreamCreate)(void**, unsigned);
    int (*cuStreamSynchronize)(void*);
    int (*cuStreamDestroy)(void*);
    int (*cuMemcpyHtoDAsync)(cajeta_cudeviceptr, const void*, size_t, void*);
    int (*cuMemcpyDtoHAsync)(void*, cajeta_cudeviceptr, size_t, void*);
    // Events (Event/Fence); optional. cuEventQuery returns CUDA_SUCCESS(0) when
    // complete, CUDA_ERROR_NOT_READY otherwise; cuStreamWaitEvent is the device-
    // side cross-stream wait.
    int (*cuEventCreate)(void**, unsigned);
    int (*cuEventRecord)(void*, void*);
    int (*cuEventSynchronize)(void*);
    int (*cuEventQuery)(void*);
    int (*cuStreamWaitEvent)(void*, void*, unsigned);
    int (*cuEventDestroy)(void*);
    int (*cuLaunchKernel)(void*, unsigned, unsigned, unsigned,
                          unsigned, unsigned, unsigned, unsigned,
                          void*, void**, void**);
    int (*cuCtxSynchronize)(void);
    // Texture / surface objects (Texture2D + Image2D) — optional (bound non-
    // fatally; absent → textures/storage images unavailable on CUDA and the launch
    // guard skips the dispatch). CUtexObject/CUsurfObject are u64 handles.
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

// Portable shared-library open (the open twin of cajeta_xpu_libsym). The CUDA/HIP
// backends inline LoadLibraryA/dlopen at their single load site; the Vulkan path
// loops over candidate ICD names, so it shares this helper. RTLD_NOW|RTLD_LOCAL on
// POSIX matches those backends.
static void* cajeta_xpu_libopen(const char* name) {
#if defined(_WIN32)
    return (void*) LoadLibraryA(name);
#else
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

// Resolve the driver and create a context. Returns 1 on success. Caller holds
// g_xpu_cuda_lock. Idempotent via the `loaded` tri-state. The driver API
// exposes size-versioned symbols (cuMemAlloc_v2, …); we bind those explicitly.
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
    CAJ_BIND(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain");  // NO _v2 suffix
    CAJ_BIND(cuCtxSetCurrent, "cuCtxSetCurrent");
    CAJ_BIND(cuModuleLoadData, "cuModuleLoadData");
    CAJ_BIND(cuModuleGetFunction, "cuModuleGetFunction");
    CAJ_BIND(cuMemAlloc, "cuMemAlloc_v2");
    CAJ_BIND(cuMemcpyHtoD, "cuMemcpyHtoD_v2");
    CAJ_BIND(cuMemcpyDtoH, "cuMemcpyDtoH_v2");
    CAJ_BIND(cuMemFree, "cuMemFree_v2");
    // Pinned / unified memory — optional (bound non-fatally; null → kind falls
    // back to plain device alloc/free).
    *(void**) (&g_xpu_cuda.cuMemAllocManaged) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuMemAllocManaged");
    *(void**) (&g_xpu_cuda.cuMemHostAlloc) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuMemHostAlloc");
    *(void**) (&g_xpu_cuda.cuMemFreeHost) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuMemFreeHost");
    // Real streams + async copies — optional (non-fatal).
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
    // Texture / surface objects — optional (non-fatal).
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
    if (g_xpu_cuda.cuInit(0) != 0) return 0;
    int count = 0;
    if (g_xpu_cuda.cuDeviceGetCount(&count) != 0 || count <= 0) return 0;
    if (g_xpu_cuda.cuDeviceGet(&g_xpu_cuda.device, 0) != 0) return 0;
    // R4 (OptiX RT-core interop): retain the per-device PRIMARY context instead of
    // creating a private one. The primary context is a process-wide singleton, so the
    // host runtime object, the JIT-embedded runtime, and the OptiX glue (which also
    // cuDevicePrimaryCtxRetains) all resolve to the SAME CUcontext — the AS build, the
    // OptiX pipeline, and the kernel launch share one context (the M1 split, where the
    // runtime used its own cuCtxCreate ctx, is resolved). Unlike cuCtxCreate,
    // cuDevicePrimaryCtxRetain does NOT make the context current, so set it current
    // here to preserve the prior init-thread-current behavior (per-launch SetCurrent
    // at the launch site, H9, still re-asserts it on worker/fiber threads).
    if (g_xpu_cuda.cuDevicePrimaryCtxRetain(&g_xpu_cuda.ctx, g_xpu_cuda.device) != 0) return 0;
    if (g_xpu_cuda.cuCtxSetCurrent) g_xpu_cuda.cuCtxSetCurrent(g_xpu_cuda.ctx);
    g_xpu_cuda.loaded = 1;
    return 1;
}

// Thread-safe "is the device usable?" gate. Once loaded == 1 the bound
// function pointers and single context are stable, so call sites read them
// unlocked after this returns true.
static int cajeta_xpu_cuda_ready(void) {
    int ok;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    ok = cajeta_xpu_cuda_init_locked();
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    return ok;
}

// The runtime's CUDA context (post-R4: the per-device PRIMARY context) as a void*, or
// NULL if CUDA is unavailable. Exposed so the OptiX glue + host probes can confirm the
// AS build / pipeline / launch all share ONE context (M2 Phase 2). Ensures the backend
// is initialized so the primary context is retained + current on the calling thread.
void* cajeta_xpu_cuda_context(void) {
    if (!cajeta_xpu_cuda_ready()) return NULL;
    return g_xpu_cuda.ctx;
}

// ============================================================================
// HIP Driver API binding (dlopen'd) — backs the real AMDGPU device path.
// ============================================================================
// Mirrors src/cajeta/xpu/amd/HipDriver.cpp in C (the C++ HipDriver is
// compiler/test-only). HIP exports plain C symbols (no size-versioning). Shares
// g_xpu_cuda_lock for init/load serialization — only one device backend is
// active per run, so there is no contention. Device pointers are plain void*.
// --- HIP texture object ABI mirror (Item 8 Stage C) -------------------------
// The runtime resolves all HIP entry points by dlsym and never includes the
// ROCm headers (they're not on the default include path and carry C++), so the
// few structs hipCreateTextureObject needs are mirrored here with byte-exact
// layout. Enum values match driver_types.h / texture_types.h.
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
    // Texture object path (Item 8 Stage C); optional — absent on very old HIP,
    // in which case texture sampling on AMD is simply unavailable.
    int (*hipMallocArray)(void**, const void*, size_t, size_t, unsigned);
    int (*hipFreeArray)(void*);
    int (*hipMemcpy2DToArray)(void*, size_t, size_t, const void*, size_t,
                              size_t, size_t, int);
    int (*hipCreateTextureObject)(void**, const void*, const void*,
                                  const void*);
    int (*hipDestroyTextureObject)(void*);
    // Surface object path (Image2D storage images, the writable twin of the
    // texture object); optional — absent → AMD storage images unavailable (the
    // path degrades like mipmaps). hipCreateSurfaceObject builds a writable
    // surface from an ARRAY resource desc (no sampler); the array must be
    // allocated with hipArraySurfaceLoadStore. hipMemcpy2DFromArray reads it back.
    int (*hipCreateSurfaceObject)(void**, const void*);
    int (*hipDestroySurfaceObject)(void*);
    int (*hipMemcpy2DFromArray)(void*, size_t, const void*, size_t, size_t,
                                size_t, size_t, int);
    // 3-D array path (Texture3D); optional — absent → 3-D textures unavailable on AMD.
    int (*hipMalloc3DArray)(void**, const void*, struct caj_hip_extent, unsigned);
    int (*hipMemcpy3D)(const void*);
    // Mipmapped-array path (mip Texture2D); optional — absent → mip textures
    // unavailable on AMD. hipMallocMipmappedArray takes the extent by value
    // ({w, h, 0} for 2-D), the level count, and flags; hipGetMipmappedArrayLevel
    // yields a plain hipArray for one level (copied into via hipMemcpy2DToArray).
    int (*hipMallocMipmappedArray)(void**, const void*, struct caj_hip_extent,
                                   unsigned, unsigned);
    int (*hipGetMipmappedArrayLevel)(void**, void*, unsigned);
    int (*hipFreeMipmappedArray)(void*);
    // Pinned / unified (managed) memory (Buffer MemoryKind); optional — absent →
    // those kinds fall back to plain hipMalloc. hipMallocManaged gives one
    // pointer accessible from host AND device (zero-copy on an APU like Strix
    // Halo); hipHostMalloc gives page-locked, device-accessible host memory
    // (fast/async DMA); hipHostFree releases the latter (managed memory frees
    // with plain hipFree).
    int (*hipMallocManaged)(void**, size_t, unsigned);
    int (*hipHostMalloc)(void**, size_t, unsigned);
    int (*hipHostFree)(void*);
    // Real streams + async copies (Buffer.uploadAsync/downloadAsync, stream-
    // ordered launch); optional — absent → stream create no-ops to the default
    // stream and async copies fall back to the synchronous memcpy.
    int (*hipStreamCreate)(void**);
    int (*hipStreamSynchronize)(void*);
    int (*hipStreamDestroy)(void*);
    int (*hipMemcpyHtoDAsync)(void*, const void*, size_t, void*);
    int (*hipMemcpyDtoHAsync)(void*, void*, size_t, void*);
    // Events (Event/Fence cross-stream + host sync); optional — absent → the
    // synchronous fallback (record/wait no-op, query true). hipEventQuery returns
    // hipSuccess(0) when complete, hipErrorNotReady otherwise; hipStreamWaitEvent
    // makes a stream wait on another stream's recorded event (device-side).
    int (*hipEventCreate)(void**);
    int (*hipEventRecord)(void*, void*);
    int (*hipEventSynchronize)(void*);
    int (*hipEventQuery)(void*);
    int (*hipStreamWaitEvent)(void*, void*, unsigned);
    int (*hipEventDestroy)(void*);
    // Device properties (R0600 ABI). Used only to read gcnArchName for the
    // addrlib-based mip/cube emulation config lookup; optional.
    int (*hipGetDevicePropertiesR0600)(void*, int);
    // Per-attribute scalar query (ABI-stable enum) for the device profile.
    int (*hipDeviceGetAttribute)(int*, int, int);
    // Device-to-device copy for the bandwidth probe (optional).
    int (*hipMemcpyDtoD)(void*, void*, size_t);
};
static struct cajeta_hip_api g_xpu_hip;

// --- libcajeta_amdtex (optional) ---------------------------------------------
// The vendored-addrlib helper for AMD HIP mipmap/cube emulation (option B: a
// hand-built gfx11 image SRD over an addrlib-tiled hipMalloc). dlopen'd exactly
// like libamdhip64 so the heavy C++ stays out of the embedded JIT bitcode; when
// the .so (or a recognised AMD GPU) is absent, the mip/cube emulation degrades to
// unsupported and the existing fallbacks apply. ABI: runtime/native/amd/cajeta_amdtex.h.
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

// HIP texture record: a Texture2D's device handle on AMD is a 1-based-... no, a
// pointer to one of these (the hipArray + dims). The hipTextureObject is built
// per launch from this array + the paired Sampler's modes.
// `array` is the (level-0) plain hipArray for non-mip 2-D/3-D; `mipmap` is the
// hipMipmappedArray for a mip Texture2D (NULL otherwise) and `levels` its level
// count (1 = no mipmaps). A mip texture keeps `array` NULL — its per-level arrays
// come from hipGetMipmappedArrayLevel; the texobj binds the mipmapped array.
struct cajeta_hip_tex {
    void* array; void* mipmap; uint32_t w, h, d; int32_t format; int levels;
    // Emulated mip path (option B, AMD only; emulated=1). When the HIP runtime
    // lacks mipmapped arrays, the mip surface is a plain hipMalloc tiled by
    // addrlib and sampled through a hand-built gfx11 image SRD. `devAlloc` is the
    // raw allocation (freed), `devBase` the addrlib-aligned surface base, `addr`
    // the addrlib handle, `srdBlob` the fine-grain-SVM {imageSRD[8],pad[4],
    // samplerSRD[4]} texobj rebuilt per launch with the bound sampler's modes.
    int emulated;
    void* devAlloc; uint64_t devBase; void* addr; void* srdBlob;
    void* stagingHost;   // persistent host copy of the tiled surface (all levels)
    struct caj_amdtex_layout_c layout;
};

// --- Texture format table ----------------------------------------------------
// TextureFormat ordinals — MUST match runtime/src/cajeta/xpu/core/TextureFormat.cajeta.
// All four are float-sampled (sample() returns a vec4): float formats read back
// as-is, UNORM formats store a byte 0..255 and read back normalized to [0,1].
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
// Raw 32-bit integer storage formats (signed or unsigned) — stored and fetched
// verbatim (no normalization / float convert); fetch-only (no hardware filter).
static inline int cajeta_texfmt_is_integer(int32_t fmt) {
    return fmt == CAJ_TEXFMT_R32I  || fmt == CAJ_TEXFMT_R32UI ||
           fmt == CAJ_TEXFMT_RGBA32I || fmt == CAJ_TEXFMT_RGBA32UI;
}
// True for the unsigned integer formats (HIP channel-kind / VK *_UINT select).
static inline int cajeta_texfmt_is_unsigned(int32_t fmt) {
    return fmt == CAJ_TEXFMT_R32UI || fmt == CAJ_TEXFMT_RGBA32UI;
}
// Bytes per channel in device storage (UNORM = 1, half = 2, float/int = 4).
static inline size_t cajeta_texfmt_channel_bytes(int32_t fmt) {
    if (cajeta_texfmt_is_unorm(fmt)) return 1u;
    if (cajeta_texfmt_is_half(fmt))  return 2u;
    return 4u;
}
// Bytes per texel in device storage.
static inline size_t cajeta_texfmt_texel_bytes(int32_t fmt) {
    return (size_t) cajeta_texfmt_channels(fmt) * cajeta_texfmt_channel_bytes(fmt);
}
// Quantize one float [0,1] to a UNORM byte (round-to-nearest, clamped).
static inline unsigned char cajeta_texfmt_unorm8(float f) {
    if (f <= 0.0f) return 0;
    if (f >= 1.0f) return 255;
    return (unsigned char) (f * 255.0f + 0.5f);
}
// Convert one float32 to IEEE 754 binary16 (round-to-nearest-even), returned as
// the raw 16-bit pattern. Handles sign, subnormals, overflow→Inf, and NaN.
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
// Encode `texels` (= w*h*channels) channel-interleaved floats from `src` into
// `dst` in the storage format: float → memcpy, half → binary16, UNORM →
// quantized bytes. `dst` must hold texels * channel_bytes.
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
// Load libamdhip64, preferring canonical ROCm (/opt/rocm, the
// update-alternatives target) then $ROCM_PATH over a bare soname; pin the
// chosen dir's libhsa-runtime with RTLD_GLOBAL first so hip's transitive HSA
// dependency binds to it by soname (see HipDriver.cpp for the rationale).
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
    // Texture object path (Item 8 Stage C) — optional; a missing entry just
    // disables AMD texture sampling, it doesn't fail the whole HIP backend.
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
    #undef CAJ_HBIND_OPT
    if (g_xpu_hip.hipInit(0) != 0) return 0;
    int count = 0;
    if (g_xpu_hip.hipGetDeviceCount(&count) != 0 || count <= 0) return 0;
    if (g_xpu_hip.hipSetDevice(g_xpu_hip.device) != 0) return 0;
    g_xpu_hip.loaded = 1;
    return 1;
}

#if !defined(_WIN32)
// Locate libcajeta_amdtex.so: $CAJETA_AMD_AMDTEX_LIB, then beside the executable
// (build tree: build-cajeta/; install: bin/ with the .so in ../lib), then by
// SONAME. Uses /proc/self/exe (no _GNU_SOURCE / dladdr — this TU avoids both).
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
        // Build tree: binaries sit in build/src/ and build/test/; the .so lands in
        // the build root one level up.
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

// Read this device's gfx arch token (e.g. "gfx1151") into `out`. Scans the
// (version-stable R0600) device-property blob for a "gfx<digit>" token rather
// than mirroring the large, drift-prone hipDeviceProp_t — the leading marketing
// `name` field never contains that token. Returns 1 on success.
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

// Query the active device into *out for the host-side DeviceModel builder. The
// arch token is the robust signal; the numeric attributes use ABI-stable
// hipDeviceGetAttribute ordinals (ROCm 6/7) and are clamped to plausible ranges
// so a wrong ordinal on another runtime leaves the field 0 rather than poisoning
// the model. See cajeta_xpu_abi.h.
int32_t cajeta_xpu_query_raw_device(CajetaXpuRawDevice* out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    const char* dis = getenv("CAJETA_XPU_DEVICE_PROFILE_DISABLE");
    if (dis && dis[0] && dis[0] != '0') return 0;

    pthread_mutex_lock(&g_xpu_cuda_lock);
    int up = cajeta_xpu_hip_init_locked();
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    if (!up) return 0;

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

// Measure device memory bandwidth (GB/s) from a device-to-device copy of `bytes`
// (2*bytes traffic: read + write), best of `passes`, host-timed around
// hipDeviceSynchronize. Returns 0.0 on failure / no GPU / profiling disabled.
double cajeta_xpu_measure_bandwidth_gbps(uint64_t bytes, int32_t passes) {
    const char* dis = getenv("CAJETA_XPU_DEVICE_PROFILE_DISABLE");
    if (dis && dis[0] && dis[0] != '0') return 0.0;
    if (bytes == 0 || passes < 1) return 0.0;

    pthread_mutex_lock(&g_xpu_cuda_lock);
    int up = cajeta_xpu_hip_init_locked();
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    if (!up || !g_xpu_hip.hipMalloc || !g_xpu_hip.hipFree ||
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

// --- per-kernel parameter metadata (the Vulkan launch translation) ----------
// The compiler registers, per Vulkan-bundled @Kernel, which args are buffers vs
// scalars and the scalar byte sizes. The Vulkan launch path uses it to turn the
// uniform kernelParams argv into descriptor bindings: buffers map to existing
// storage buffers; scalars are copied into transient single-element SSBOs. The
// pointers are program constant data (valid for the process lifetime).
// Per-param kind in the launch metadata (matches xpu::KernelParamInfo::kind).
// The numeric values live exactly once, in CajetaXpuParamKind (the ABI header);
// these short names are internal aliases for the runtime body below so the
// literals can never drift from the compiler/FFI contract again.
#define CAJETA_KP_SCALAR       CAJETA_XPU_KP_SCALAR
#define CAJETA_KP_BUFFER       CAJETA_XPU_KP_BUFFER
#define CAJETA_KP_TEXTURE      CAJETA_XPU_KP_TEXTURE
#define CAJETA_KP_SAMPLER      CAJETA_XPU_KP_SAMPLER
#define CAJETA_KP_ACCEL        CAJETA_XPU_KP_ACCEL   // AccelerationStructure -> descriptor-bound BVH
#define CAJETA_KP_IMAGE        CAJETA_XPU_KP_IMAGE   // Image2D (writable) -> STORAGE_IMAGE descriptor
#define CAJETA_KP_BUFFER_ARRAY CAJETA_XPU_KP_BUFFER_ARRAY  // Buffer<T>[] -> bindless descriptor array
                                   // (descriptorCount = N; N + handles in argv slot)

// The ABI version queryable by external FFI callers (header/runtime handshake).
int32_t __cajeta_xpu_abi_version(void) { return CAJETA_XPU_ABI_VERSION; }

struct cajeta_kparams {
    char name[256];
    int count;
    const uint8_t* kind;
    const uint32_t* byteSize;
};
// 1024, up from 128 (2026-08-25) — same silent-overflow bug as the module
// registry: cajeta-llama's suite crossed 128 kernels and dropped entries
// surfaced only as downstream misbehaviour. Kept equal to
// CAJETA_XPU_MAX_MODULES by convention.
#define CAJETA_XPU_MAX_KPARAMS 1024
static struct cajeta_kparams g_xpu_kparams[CAJETA_XPU_MAX_KPARAMS];
static int g_xpu_kparam_count;

void __cajeta_xpu_register_kernel_params(const char* name, int32_t count,
                                         const uint8_t* kind,
                                         const uint32_t* byteSize) {
    if (!name) return;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    // M3: dedup by name — re-registration (e.g. a re-run or a second backend)
    // overwrites the existing entry instead of appending a stale duplicate and
    // exhausting the fixed table (M backends would otherwise fill it at
    // CAJETA_XPU_MAX_KPARAMS/M kernels).
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
    // L9: publish a new slot only after its fields are fully written, so a
    // lock-free find_kparams can't observe an entry with a stale field set.
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


// ============================================================================
