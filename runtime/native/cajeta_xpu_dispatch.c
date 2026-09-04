// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// --- registered kernel modules (device images keyed by entry name + backend) -
// Each backend's registration ctor calls __cajeta_xpu_register_module_be once
// per @Kernel with ITS image and backend id; the launch path resolves by
// (name, active backend) and loads lazily on first use. A multi-backend build
// registers one image per backend under the same name — keying by name alone
// made the last ctor win, so on a nvptx,amdgpu,vulkan,cpu build the CUDA and
// HIP paths saw the SPIR-V image ("no registered kernel" via the magic check,
// or a RADV crash in the reverse order). backend == -1 marks a legacy 3-arg
// registration and matches any requested backend.
struct cajeta_xpu_module {
    char name[256];
    int backend;      // CAJ_XPU_* id of the image's consumer, or -1 (legacy/any)
    const void* image;
    uint64_t len;     // image byte length (SPIR-V needs it; CUDA/HIP ignore it)
    void* module;     // CUmodule/hipModule, lazily loaded
    void* function;   // CUfunction/hipFunction, lazily resolved
};
// 1024, up from 128 (2026-08-25): cajeta-llama's engine + test suite crossed
// 128 registered kernels and the overflow was SILENT — registration returned
// without storing, and the dropped kernel surfaced only at launch as "no
// registered kernel", two programs and one linker away from the cause. The
// register path now also says so at the moment of the drop (see
// cajeta_xpu_register_module_impl); this headroom is cheap (~300 KB static).
#define CAJETA_XPU_MAX_MODULES 1024
static struct cajeta_xpu_module g_xpu_modules[CAJETA_XPU_MAX_MODULES];
static int g_xpu_module_count;

// Caller holds g_xpu_cuda_lock. `backend` is the requesting consumer's
// CAJ_XPU_* id, or -1 for the legacy don't-care lookup. Exact backend match
// wins; a legacy (-1) entry serves any requester.
static struct cajeta_xpu_module* cajeta_xpu_find_module(const char* name,
                                                        int backend) {
    int i;
    struct cajeta_xpu_module* wild = NULL;
    for (i = 0; i < g_xpu_module_count; i++) {
        if (strncmp(g_xpu_modules[i].name, name,
                    sizeof(g_xpu_modules[i].name)) == 0) {
            if (backend == -1 || g_xpu_modules[i].backend == backend) {
                return &g_xpu_modules[i];
            }
            if (g_xpu_modules[i].backend == -1 && !wild) {
                wild = &g_xpu_modules[i];
            }
        }
    }
    if (wild) { return wild; }
    return NULL;
}

// --- Stream -----------------------------------------------------------------
// Streams. The Stream handle (int64) is the per-backend stream object: 0 = the
// default stream (the original v1 behaviour; current() returns it and the launch
// path passes NULL for it). create() makes a REAL stream (hipStreamCreate /
// cuStreamCreate) so async copies + stream-ordered launches queue independently;
// sync() drains either the named stream (real handle) or the whole context (0).
// Defined with the backend dispatcher below (after the kernel registries).
static void cajeta_xpu_sync_active(void);

int64_t __cajeta_xpu_stream_current(void) { return 0; }   // the default stream
// __cajeta_xpu_stream_{create,sync,destroy,wait_for} and the Event/Fence natives
// are defined further below, after the backend enum + cajeta_xpu_active_backend()
// they switch on (alongside the buffer async-copy functions).

// --- Thread / Workgroup coordinate readers ---------------------------------
// Returns zero in v1; step 7 plumbs these into TLS set by the emulation
// dispatch loop so kernel bodies see real thread indices.
uint32_t __cajeta_xpu_thread_x(void) { return 0; }
uint32_t __cajeta_xpu_thread_y(void) { return 0; }
uint32_t __cajeta_xpu_thread_z(void) { return 0; }
uint32_t __cajeta_xpu_thread_global_id_x(void) { return 0; }
uint32_t __cajeta_xpu_thread_global_id_y(void) { return 0; }
uint32_t __cajeta_xpu_thread_global_id_z(void) { return 0; }
uint32_t __cajeta_xpu_workgroup_x(void) { return 0; }
uint32_t __cajeta_xpu_workgroup_y(void) { return 0; }
uint32_t __cajeta_xpu_workgroup_z(void) { return 0; }
uint32_t __cajeta_xpu_workgroup_dim_x(void) { return 0; }
uint32_t __cajeta_xpu_workgroup_dim_y(void) { return 0; }
uint32_t __cajeta_xpu_workgroup_dim_z(void) { return 0; }

// --- Barrier ---------------------------------------------------------------
void __cajeta_xpu_barrier_workgroup(void) { /* no-op on CPU emulation */ }
void __cajeta_xpu_barrier_wave(void) { /* no-op on CPU emulation */ }
void __cajeta_xpu_barrier_workgroup_memory(void) { /* host no-op; kernel path lowers to a scoped fence */ }
void __cajeta_xpu_barrier_device_memory(void) { /* host no-op; kernel path lowers to a scoped fence */ }
void __cajeta_xpu_barrier_workgroup_memory_ord(int32_t order) { (void) order; /* host no-op; kernel path lowers with the order */ }
void __cajeta_xpu_barrier_device_memory_ord(int32_t order) { (void) order; /* host no-op; kernel path lowers with the order */ }

// --- Wave ------------------------------------------------------------------
// width=1 on CPU emulation (single-threaded) is the variance-correct
// default that doesn't make any kernel's wave-uniformity assumption
// false on this backend.
uint32_t __cajeta_xpu_wave_width(void) { return 1; }
// Lane within the wave: 0 on the width-1 emulation (only lane 0 exists). In a
// vectorized CPU kernel the lowering computes `tid.x % width` inline instead of
// calling this stub; this is the host @Native / scalar-fallback value.
uint32_t __cajeta_xpu_wave_lane_id(void) { return 0; }
// Width-1 emulation: the single lane is always the first.
bool __cajeta_xpu_wave_is_first_lane(void) { return true; }
uint32_t __cajeta_xpu_wave_shuffle_sync_u32(uint32_t value, uint32_t srcLane) {
    (void)srcLane; return value;
}
uint64_t __cajeta_xpu_wave_ballot_sync(bool predicate) {
    return predicate ? 1ULL : 0ULL;
}
// Single-lane wave (width=1) on CPU emulation: the wave-wide reduction of one
// lane's value is just that value. The real cross-lane reduction happens in the
// vectorized VFABI variant (CpuRegistration) when a wave kernel is widened;
// these scalars are the width-1 fallback.
uint32_t __cajeta_xpu_wave_reduce_sum_u32(uint32_t value) { return value; }
uint32_t __cajeta_xpu_wave_reduce_max_u32(uint32_t value) { return value; }
uint32_t __cajeta_xpu_wave_reduce_min_u32(uint32_t value) { return value; }
uint32_t __cajeta_xpu_wave_reduce_and_u32(uint32_t value) { return value; }
uint32_t __cajeta_xpu_wave_reduce_or_u32(uint32_t value) { return value; }
uint32_t __cajeta_xpu_wave_reduce_xor_u32(uint32_t value) { return value; }
// Mask-as-data spellings (compiler-generated; CpuRegistration's mask-as-data
// rewrite hoists a guarded wave reduce out of divergent control flow and
// passes the guard as an explicit lane-active argument, so LoopVectorize can
// never scalarize the cross-lane op per lane — the arm64-darwin/NEON class
// of silent wrong sums). Width-1 fallback: an active lane reduces to its own
// value, an inactive lane contributes the op's identity (result unused —
// every consumer of the result is still under the original guard).
float __cajeta_xpu_wave_reduce_sum_f32(float value) { return value; }
float __cajeta_xpu_wave_reduce_max_f32(float value) { return value; }
uint32_t __cajeta_xpu_wave_reduce_sum_u32_m(uint32_t value, _Bool active) { return active ? value : 0u; }
uint32_t __cajeta_xpu_wave_reduce_max_u32_m(uint32_t value, _Bool active) { return active ? value : 0u; }
uint32_t __cajeta_xpu_wave_reduce_min_u32_m(uint32_t value, _Bool active) { return active ? value : 0xFFFFFFFFu; }
uint32_t __cajeta_xpu_wave_reduce_and_u32_m(uint32_t value, _Bool active) { return active ? value : 0xFFFFFFFFu; }
uint32_t __cajeta_xpu_wave_reduce_or_u32_m(uint32_t value, _Bool active) { return active ? value : 0u; }
uint32_t __cajeta_xpu_wave_reduce_xor_u32_m(uint32_t value, _Bool active) { return active ? value : 0u; }
float __cajeta_xpu_wave_reduce_sum_f32_m(float value, _Bool active) { return active ? value : 0.0f; }
float __cajeta_xpu_wave_reduce_max_f32_m(float value, _Bool active) { return active ? value : -3.402823466e38f; }
// Exclusive prefix scan: width-1 fallback — lane 0's exclusive prefix is the
// identity (0 for sum, 1 for product). The real scan runs in the VFABI variant.
uint32_t __cajeta_xpu_wave_prefix_sum_u32(uint32_t value) { (void)value; return 0; }
uint32_t __cajeta_xpu_wave_prefix_product_u32(uint32_t value) { (void)value; return 1; }
// Width-1 wave rotate: a single-lane wave rotated by any delta is the lane
// itself (the host @Native / scalar fallback; the device path is ds_bpermute /
// OpGroupNonUniformRotateKHR). Matches the shuffle/reduce width-1 convention.
uint32_t __cajeta_xpu_wave_rotate_u32(uint32_t value, uint32_t delta) {
    (void)delta; return value;
}

// Quad (2x2) ops (Quad.*). Like the wave ops these are cross-lane on device; the
// host @Native definition is the width-1 (single-lane quad) fallback so the host
// JIT / any CPU @Device call resolves the Quad.cajeta forwarders. The device
// backends lower them inline (OpGroupNonUniformQuad* on Vulkan, ds_bpermute /
// shuffle elsewhere). A lone quad lane: broadcast/swap yield the lane's own
// value; the vote is just this lane's predicate.
uint32_t __cajeta_xpu_quad_broadcast(uint32_t value, uint32_t index) {
    (void)index; return value;
}
uint32_t __cajeta_xpu_quad_swap_horizontal(uint32_t value) { return value; }
uint32_t __cajeta_xpu_quad_swap_vertical(uint32_t value) { return value; }
uint32_t __cajeta_xpu_quad_swap_diagonal(uint32_t value) { return value; }
bool __cajeta_xpu_quad_all(bool predicate) { return predicate; }
bool __cajeta_xpu_quad_any(bool predicate) { return predicate; }

// Per-invocation bit ops (Bits.*). Unlike the wave ops these are NOT cross-lane —
// they are pure scalar functions of one u32, so the host @Native definition is
// the exact same computation the device emits (OpBitReverse / OpBitCount / a
// masked rotate). Provided so the host JIT (and any CPU @Device call) resolves
// the Bits.cajeta forwarders; the device backends lower them inline.
uint32_t __cajeta_xpu_bits_reverse_u32(uint32_t value) {
    value = ((value & 0x55555555u) << 1)  | ((value >> 1)  & 0x55555555u);
    value = ((value & 0x33333333u) << 2)  | ((value >> 2)  & 0x33333333u);
    value = ((value & 0x0F0F0F0Fu) << 4)  | ((value >> 4)  & 0x0F0F0F0Fu);
    value = ((value & 0x00FF00FFu) << 8)  | ((value >> 8)  & 0x00FF00FFu);
    value = (value << 16) | (value >> 16);
    return value;
}
uint32_t __cajeta_xpu_bits_count_u32(uint32_t value) {
    uint32_t c = 0;
    while (value) { value &= value - 1u; ++c; }
    return c;
}
uint32_t __cajeta_xpu_bits_rotate_left_u32(uint32_t value, uint32_t amount) {
    amount &= 31u;
    return amount == 0u ? value : ((value << amount) | (value >> (32u - amount)));
}
uint32_t __cajeta_xpu_bits_rotate_right_u32(uint32_t value, uint32_t amount) {
    amount &= 31u;
    return amount == 0u ? value : ((value >> amount) | (value << (32u - amount)));
}

// --- CPU backend kernel registry -------------------------------------------
// The CPU backend (cajeta-cpu.md) lowers each @Kernel to a host function linked
// into the program. Its registration ctor calls register_cpu_kernel(name, fn)
// at startup; the runtime dispatcher (Increment 4) resolves a launch to the
// stored pointer. Keyed by simple kernel name, matching the device backends'
// name-keyed __cajeta_xpu_register_module. A small fixed table — kernel counts
// are tiny — with last-writer-wins on a duplicate name.
#ifndef CAJETA_XPU_CPU_KERNEL_MAX
#define CAJETA_XPU_CPU_KERNEL_MAX 256
#endif
static struct { const char* name; void* fn; } g_cpu_kernels[CAJETA_XPU_CPU_KERNEL_MAX];
static int g_cpu_kernel_count = 0;

void __cajeta_xpu_register_cpu_kernel(const char* name, void* fn) {
    if (!name || !fn) return;
    for (int i = 0; i < g_cpu_kernel_count; ++i) {
        if (g_cpu_kernels[i].name && strcmp(g_cpu_kernels[i].name, name) == 0) {
            g_cpu_kernels[i].fn = fn;  // last writer wins
            return;
        }
    }
    if (g_cpu_kernel_count < CAJETA_XPU_CPU_KERNEL_MAX) {
        // Own the name: a caller may free the string after registering (notably
        // a JIT'd registration ctor whose module/engine is later torn down — the
        // kname global lives in JIT memory). Keeping the raw pointer leaves a
        // dangling key that the next strcmp() here or in lookup dereferences →
        // crash. strdup so the registry's keys outlive any caller (matching the
        // env-registry above). Process-lifetime table, never freed.
        g_cpu_kernels[g_cpu_kernel_count].name = strdup(name);
        g_cpu_kernels[g_cpu_kernel_count].fn = fn;
        ++g_cpu_kernel_count;
    }
}

// Resolve a registered CPU kernel by name (NULL if absent). Used by the
// dispatcher; exposed now so registration is testable end-to-end.
void* __cajeta_xpu_lookup_cpu_kernel(const char* name) {
    if (!name) return 0;
    for (int i = 0; i < g_cpu_kernel_count; ++i) {
        if (g_cpu_kernels[i].name && strcmp(g_cpu_kernels[i].name, name) == 0) {
            return g_cpu_kernels[i].fn;
        }
    }
    return 0;
}

// --- Backend dispatcher (cajeta-cpu.md Increment 4) -------------------------
// Compiled Cajeta programs launch through THIS C runtime only (the C++
// CudaDriver/HipDriver/VulkanDriver/CpuDriver are compiler/test-only and never
// linked into a user program). A binary can bundle several backends
// (--xpu-backend=vulkan,cpu); at the first device touch we pick the
// highest-priority one that is both BUNDLED (a compile-time manifest of ctors
// calling __cajeta_xpu_register_backend) and AVAILABLE (a runtime probe),
// honoring a CAJETA_XPU_BACKEND force-override, then cache it. Every device
// entry point (buffer_*, launch) routes to that backend. The choice is made
// ONCE — a GPU present at startup but lost mid-run is a hard error, not a silent
// CPU re-run (locked decision #2).
//
// Priority order: CUDA -> HIP -> Vulkan -> CPU. CPU is always available, the
// guaranteed terminal of the chain. Backend ids are the priority order.
enum {
    CAJ_XPU_CUDA   = 0,
    CAJ_XPU_HIP    = 1,
    CAJ_XPU_VULKAN = 2,
    CAJ_XPU_CPU    = 3,
    CAJ_XPU_COUNT  = 4,
    CAJ_XPU_NONE   = -1
};

static unsigned g_xpu_bundled;        // bit i set iff backend i was bundled
static int g_xpu_active = -2;         // -2 unselected, -1 none, else a backend id

static const char* cajeta_xpu_backend_name(int id) {
    switch (id) {
        case CAJ_XPU_CUDA:   return "cuda";
        case CAJ_XPU_HIP:    return "hip";
        case CAJ_XPU_VULKAN: return "vulkan";
        case CAJ_XPU_CPU:    return "cpu";
        default:             return "?";
    }
}

static int cajeta_xpu_backend_id_by_name(const char* s) {
    if (!s) return CAJ_XPU_NONE;
    for (int id = 0; id < CAJ_XPU_COUNT; ++id)
        if (strcmp(s, cajeta_xpu_backend_name(id)) == 0) return id;
    return CAJ_XPU_NONE;
}

// The compiler emits one ctor per bundled backend (Compiler::emitXpuKernels).
void __cajeta_xpu_register_backend(int32_t id) {
    if (id < 0 || id >= CAJ_XPU_COUNT) return;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    g_xpu_bundled |= (1u << id);
    pthread_mutex_unlock(&g_xpu_cuda_lock);
}

// Probe a backend's availability. Caller holds g_xpu_cuda_lock — so this calls
// the *_init_locked variants directly (NOT the locking *_ready wrappers, which
// would deadlock under the held lock). Vulkan lands in Increment 4.3; until then
// it probes unavailable, so a vulkan-only bundle falls through to the precise
// diagnostic (or to CPU if bundled).
static int cajeta_xpu_backend_available_locked(int id) {
    switch (id) {
        case CAJ_XPU_CUDA:   return cajeta_xpu_cuda_init_locked();
        case CAJ_XPU_HIP:    return cajeta_xpu_hip_init_locked();
        case CAJ_XPU_VULKAN: return cajeta_xpu_vulkan_init_locked();
        case CAJ_XPU_CPU:    return 1;
        default:             return 0;
    }
}

// In-process backend force (Device.force — cajeta-llama 11.2): observed by
// select_locked ahead of the env var. -1 = not forced.
static int g_xpu_forced_api = -1;

// Returns 1 when the force landed before selection, 0 when selection has
// already cached a backend (too late — the caller reports it).
int32_t __cajeta_xpu_force_backend(int32_t id) {
    int ok;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    if (g_xpu_active != -2) {
        ok = 0;
    } else {
        g_xpu_forced_api = (id >= 0 && id < CAJ_XPU_COUNT) ? id : -1;
        ok = 1;
    }
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    return ok;
}

// Caller holds g_xpu_cuda_lock. Picks + caches the active backend.
static int cajeta_xpu_select_locked(void) {
    if (g_xpu_active != -2) return g_xpu_active;
    int forced = g_xpu_forced_api >= 0
        ? g_xpu_forced_api
        : cajeta_xpu_backend_id_by_name(getenv("CAJETA_XPU_BACKEND"));
    for (int id = 0; id < CAJ_XPU_COUNT; ++id) {
        if (forced != CAJ_XPU_NONE && id != forced) continue;
        if (!(g_xpu_bundled & (1u << id))) continue;     // not bundled in
        if (cajeta_xpu_backend_available_locked(id)) { g_xpu_active = id; return id; }
    }
    g_xpu_active = CAJ_XPU_NONE;
    // Precise, once: degradation is a build-time contract (locked decision #3).
    char set[128]; size_t n = 0; set[0] = '\0';
    for (int id = 0; id < CAJ_XPU_COUNT; ++id) {
        if (!(g_xpu_bundled & (1u << id))) continue;
        const char* nm = cajeta_xpu_backend_name(id);
        n += (size_t) snprintf(set + n, sizeof(set) - n, "%s%s",
                               set[0] ? ", " : "", nm);
        if (n >= sizeof(set)) break;
    }
    fprintf(stderr,
            "cajeta.xpu: no available backend among {%s}; rebuild with `cpu` "
            "to enable CPU fallback\n", set);
    return CAJ_XPU_NONE;
}

static int cajeta_xpu_active_backend(void) {
    int r;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    r = cajeta_xpu_select_locked();
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    return r;
}

// cajeta.xpu.Device.activeBackend() — which backend the runtime selected.
// Returns the CAJ_XPU_* id, or -1 when none is available. NOTE this SELECTS
// the backend if the selection has not happened yet (it is a device touch),
// which is the same contract Device.supports() already has.
int32_t __cajeta_xpu_active_backend_id(void) {
    return (int32_t) cajeta_xpu_active_backend();
}

// cajeta.xpu.Device.memoryBytes() — the active device's total visible
// memory in bytes, 0 when the backend cannot answer (an absent optional
// symbol, a query failure, or no backend). 0 is "unknown", never a
// budget: callers must treat it as no answer. Like activeBackend, this
// is a device touch.
//   cuda   — cuDeviceTotalMem on the selected device.
//   hip    — hipMemGetInfo's `total`; on a UMA part this is the
//            GTT-visible pool, which is the honest device-visible
//            number (Strix Halo reports ~96 GiB of the 122 GiB RAM).
//   vulkan — the sum of DEVICE_LOCAL heaps from the memory properties
//            cached at init (heap sizes, not budgets: allocation can
//            still fail earlier under pressure).
//   cpu    — total physical RAM (the device IS the host).
// cajeta.xpu.Device's geometry surface — the queried machine shape a kernel
// needs in order to size itself instead of carrying a constant measured on
// somebody else's part (specs/device-geometry-parameterization-spec.md).
//
// Backed by the SAME query the host-side DeviceProfile uses, so a number a
// cajeta program reads and a number `cajeta gpu-profile` prints cannot
// disagree. Cached once: the query dlopens a driver and reads a dozen
// attributes, and a kernel-sizing call site may run per launch.
//
// 0 means UNKNOWN for every key — no device, profiling disabled, or a runtime
// that did not report that fact. A caller must branch on 0 rather than treat
// it as a budget; that substitution is the exact defect this work undoes.
static CajetaXpuRawDevice g_xpu_geo;
static int g_xpu_geo_state = 0;   /* 0 untried, 1 valid, -1 unavailable */

int64_t __cajeta_xpu_device_geometry(int32_t key) {
    if (g_xpu_geo_state == 0) {
        g_xpu_geo_state = cajeta_xpu_query_raw_device(&g_xpu_geo) && g_xpu_geo.valid
                        ? 1 : -1;
    }
    if (g_xpu_geo_state != 1) return 0;
    switch ((CajetaXpuGeometryKey) key) {
        case CAJETA_XPU_GEO_MP_COUNT:              return g_xpu_geo.multiprocessorCount;
        case CAJETA_XPU_GEO_SIMDS_PER_MP:          return cajeta_xpu_simds_per_mp(g_xpu_geo.archName);
        case CAJETA_XPU_GEO_WAVE_SIZE:             return g_xpu_geo.waveSize;
        case CAJETA_XPU_GEO_MAX_THREADS_PER_BLOCK: return g_xpu_geo.maxThreadsPerBlock;
        /* The per-block ceiling falls back to the per-MP budget when the
         * runtime does not report one: on AMD the two are equal, and that
         * fallback is the number AMD has always effectively used. */
        case CAJETA_XPU_GEO_LDS_BYTES_PER_BLOCK:
            return g_xpu_geo.ldsBytesPerBlock ? g_xpu_geo.ldsBytesPerBlock
                                              : g_xpu_geo.ldsBytesPerMP;
        case CAJETA_XPU_GEO_LDS_BYTES_PER_BLOCK_OPTIN:
            return g_xpu_geo.ldsBytesPerBlockOptin;
        case CAJETA_XPU_GEO_LDS_BYTES_PER_MP:      return g_xpu_geo.ldsBytesPerMP;
        case CAJETA_XPU_GEO_MAX_BLOCKS_PER_MP:     return g_xpu_geo.maxBlocksPerMP;
        case CAJETA_XPU_GEO_L2_CACHE_BYTES:        return g_xpu_geo.l2CacheBytes;
        case CAJETA_XPU_GEO_TOTAL_VRAM_BYTES:      return (int64_t) g_xpu_geo.totalGlobalMemBytes;
        case CAJETA_XPU_GEO_INTEGRATED:            return g_xpu_geo.integrated ? 1 : 0;
        case CAJETA_XPU_GEO_REGS_PER_MP:           return g_xpu_geo.regsPerMP;
        case CAJETA_XPU_GEO_THREADS_PER_MP:        return g_xpu_geo.threadsPerMP;
        case CAJETA_XPU_GEO_MAX_GRID_DIM_X:        return g_xpu_geo.maxGridDimX;
        case CAJETA_XPU_GEO_MAX_BLOCK_DIM_X:       return g_xpu_geo.maxBlockDimX;
    }
    return 0;
}

int64_t __cajeta_xpu_device_memory_bytes(void) {
    int be = cajeta_xpu_active_backend();
    switch (be) {
        case CAJ_XPU_CUDA: {
            size_t total = 0;
            if (!g_xpu_cuda.cuDeviceTotalMem) return 0;
            if (g_xpu_cuda.cuDeviceTotalMem(&total, g_xpu_cuda.device) != 0)
                return 0;
            return (int64_t) total;
        }
        case CAJ_XPU_HIP: {
            size_t memfree = 0, total = 0;
            if (!g_xpu_hip.hipMemGetInfo) return 0;
            if (g_xpu_hip.hipMemGetInfo(&memfree, &total) != 0) return 0;
            return (int64_t) total;
        }
        case CAJ_XPU_VULKAN: {
#if defined(CAJETA_RT_HAS_VULKAN)
            int64_t sum = 0;
            for (uint32_t i = 0; i < g_xpu_vk.memProps.memoryHeapCount; ++i) {
                if (g_xpu_vk.memProps.memoryHeaps[i].flags
                        & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    sum += (int64_t) g_xpu_vk.memProps.memoryHeaps[i].size;
            }
            return sum;
#else
            return 0;
#endif
        }
        case CAJ_XPU_CPU: {
#if defined(_WIN32)
            MEMORYSTATUSEX ms;
            ms.dwLength = sizeof(ms);
            if (!GlobalMemoryStatusEx(&ms)) return 0;
            return (int64_t) ms.ullTotalPhys;
#else
            long pages = sysconf(_SC_PHYS_PAGES);
            long psize = sysconf(_SC_PAGE_SIZE);
            if (pages <= 0 || psize <= 0) return 0;
            return (int64_t) pages * (int64_t) psize;
#endif
        }
        default:
            return 0;
    }
}

// Forward decl (the OptiX glue's full extern block is below, near the launch path).
extern int cajeta_xpu_optix_available(void);

// Device.supports(Capability) — does the active device advertise the capability
// natively? The capability heuristic's runtime input (cajeta.xpu.Device).
// `cap` is the Capability ordinal (the stable contract in Capability.cajeta).
// Returns 0/1. Append new capabilities as new cases; never renumber.
int32_t __cajeta_xpu_device_supports(int32_t cap) {
    int be = cajeta_xpu_active_backend();
    switch (cap) {
        case 0:  // RayQueryNative — hardware INLINE ray query
            // g_xpu_vk.rayQuery is detected in the main (Windows-included) Vulkan
            // device init, so this reports the real device capability on Windows
            // too (the RTX 4090's Vulkan driver advertises VK_KHR_ray_query). The
            // old `&& !defined(_WIN32)` hard-zeroed it on Windows — wrong now that
            // the Vulkan ray-query path is exercised there.
            // NOTE: CUDA is intentionally false here — OptiX has NO inline ray
            // query (RT cores are reached only through a pipeline), so the CUDA RT
            // core path is the SEPARATE RayQueryRtCore capability below, not this.
#if defined(CAJETA_RT_HAS_VULKAN)
            return (be == CAJ_XPU_VULKAN && g_xpu_vk.rayQuery) ? 1 : 0;
#else
            (void) be; return 0;
#endif
        case 1:  // RayQueryRtCore — pipeline-based RT-core ray query (NVIDIA OptiX)
            // Distinct from RayQueryNative: this is the CUDA RT-core path reached via
            // the OptiX pipeline (raygen/anyhit/closesthit/intersection/miss + SBT),
            // opt-in at AS-build time with CAJETA_GPU_AS_IMPL=optix. True iff the
            // active device is CUDA and the OptiX engine (nvoptix.dll) loaded.
            return (be == CAJ_XPU_CUDA && cajeta_xpu_optix_available()) ? 1 : 0;
        case 2:  // CoopMatrixBf16F32Acc — a LAUNCHABLE bf16(A/B)+f32(acc)
            // cooperative-matrix GEMM path on the active backend (cajeta-llama
            // 2.2.6). "Launchable" is the contract, not "native silicon": the
            // op layer uses this to decide whether Ewise.matmulBf16Wide can be
            // dispatched at all, so the CPU backend answers 1 (its software
            // tier runs any tile-dtype mix) even though nothing about it is
            // native. Vulkan answers 0 — no driver exposes a bf16 coop-matrix
            // config, the SPIR-V lowering skips the kernel (mixed-tier), and a
            // launch would fail on the missing registration. CUDA needs the
            // bf16 tensor cores (sm_80+); HIP needs RDNA3+ (gfx11xx/gfx12xx —
            // wmma.f32.16x16x16.bf16; CDNA's MFMA path is not wired in the
            // AMDGPU lowering, so it stays conservative-false there).
            switch (be) {
                case CAJ_XPU_CPU:
                    return 1;
                case CAJ_XPU_CUDA: {
                    int major = 0;
                    if (!g_xpu_cuda.cuDeviceGetAttribute) return 0;
                    // 75 = CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR
                    if (g_xpu_cuda.cuDeviceGetAttribute(&major, 75,
                            g_xpu_cuda.device) != 0) return 0;
                    return major >= 8 ? 1 : 0;
                }
                case CAJ_XPU_HIP: {
                    char arch[64];
                    if (!cajeta_xpu_hip_gfx_arch(arch, sizeof(arch))) return 0;
                    return (strncmp(arch, "gfx11", 5) == 0 ||
                            strncmp(arch, "gfx12", 5) == 0) ? 1 : 0;
                }
                default:
                    return 0;
            }
        case 3:  // AtomicInt64 — Buffer<int64|uint64>.atomic* runs natively.
            // The Vulkan init already probes VK_KHR_shader_atomic_int64 and
            // enables shaderBufferInt64Atomics when the device has it; this
            // just surfaces that verdict. Neither Apple driver advertises the
            // extension, so both answer 0 and callers take the degrade path
            // (apple-vulkan spec 4.6). CUDA and HIP have had 64-bit global
            // atomics since forever; CPU serializes and always can.
            switch (be) {
                case CAJ_XPU_CPU:
                case CAJ_XPU_CUDA:
                case CAJ_XPU_HIP:
                    return 1;
                case CAJ_XPU_VULKAN:
#if defined(CAJETA_RT_HAS_VULKAN)
                    return g_xpu_vk.atomicInt64 ? 1 : 0;
#else
                    return 0;
#endif
                default:
                    return 0;
            }
        default: return 0;
    }
}

// Synchronize the active backend (called by stream.sync). active_backend() has
// already initialized the chosen backend, so its fn pointers are valid. CPU is
// synchronous (nothing to drain); none is a no-op.
static void cajeta_xpu_sync_active(void) {
    switch (cajeta_xpu_active_backend()) {
        // After a synchronize every event on the device has completed, so this
        // is where the profiler's brackets resolve promptly instead of waiting
        // for the next launch to poll them.
        case CAJ_XPU_CUDA: g_xpu_cuda.cuCtxSynchronize();
                           caj_cuda_bracket_drain();        break;
        case CAJ_XPU_HIP:  g_xpu_hip.hipDeviceSynchronize(); break;
        case CAJ_XPU_VULKAN: cajeta_xpu_vk_flush();          break;
        default: break;
    }
}

// CPU launch: resolve the kernel's registered launcher thunk and run the
// grid->threads loop (the in-C twin of CpuDriver::launch; cajeta-cpu.md Inc 3),
// 1-D to match the host-source launch boundary. coord = [tid.xyz, ctaid.xyz,
// ntid.xyz]; argv is the kernelParams array shared across work-items.
typedef void (*cajeta_cpu_launch_fn)(void** argv, const int32_t* coord);

// One worker's slice of the grid: linear block indices [bStart, bEnd) of a
// gx*gy*gz block grid, each block sized (bx,by,bz) work-items.
struct cajeta_cpu_grid_slice {
    cajeta_cpu_launch_fn fn;
    void** argv;
    int32_t bx, by, bz;   // block (workgroup) dims → ntid.xyz
    int32_t gx, gy, gz;   // grid dims (in blocks) → for decoding ctaid.xyz
    int32_t bStart;       // linear block index range [bStart, bEnd)
    int32_t bEnd;
    int32_t dynShared;    // dynamic shared-memory byte count (coord[12])
    int32_t specCount;        // host spec-constant overrides (0 = none)
    const int32_t* specValues;  // slot-indexed raw words; lives for the launch
};

// Host spec-constant override state for the CPU backend (Stage 11/12, hybrid
// decision: CPU honors an override by READING the supplied value at runtime —
// no per-value recompile; folding is a perf detail irrelevant on the oracle
// path). Thread-local because the grid fans out across worker threads: each
// worker's run_slice sets its OWN copy from its slice before invoking the
// per-block wrapper, so the spec helpers (called from the inlined kernel on that
// same thread) read the right values race-free. Set fresh per run_slice, so no
// stale reads across launches.
static __thread int32_t g_cpu_spec_count = 0;
static __thread const int32_t* g_cpu_spec_values = NULL;

// Read user spec slot `slot` (CPU): the host override if supplied, else the
// kernel's compile-time `def`. The CPU kernel lowering emits calls to these
// (CpuKernelLowering::specConstant{I32,F32}) instead of baking the default. The
// f32 form reinterprets the raw override word (the transport is type-agnostic).
int32_t __cajeta_xpu_cpu_spec_i32(int32_t slot, int32_t def) {
    if (g_cpu_spec_values && slot >= 0 && slot < g_cpu_spec_count)
        return g_cpu_spec_values[slot];
    return def;
}
float __cajeta_xpu_cpu_spec_f32(int32_t slot, float def) {
    if (g_cpu_spec_values && slot >= 0 && slot < g_cpu_spec_count) {
        float f;
        memcpy(&f, &g_cpu_spec_values[slot], sizeof(float));
        return f;
    }
    return def;
}

// Run a contiguous slice of blocks. The launcher thunk is the per-BLOCK wrapper
// (Inc 5B): it loops the block's work-items internally (vectorized), so we call
// it ONCE PER BLOCK, setting ctaid.xyz + ntid.xyz + nctaid.xyz. coord =
// [tid.xyz (the wrapper's loop var), ctaid.xyz, ntid.xyz, nctaid.xyz, dynShared].
// nctaid (grid block-count = gx,gy,gz) lets the kernel compute the grid-stride
// for-each stride gridSize = nctaid·ntid (Item 6 Stage 2). Each worker owns its
// coord (no sharing); a data-parallel CPU kernel writes disjoint elements, so
// the fan-out is race-free for any kernel correct on a GPU. The 3-D grid is
// linearized (x fastest) and decoded back to ctaid.xyz per block.
static void cajeta_xpu_cpu_run_slice(const struct cajeta_cpu_grid_slice* s) {
    // Publish this worker's spec overrides for the kernel's spec helpers (TLS;
    // see g_cpu_spec_*). Set every call, so a launch with no override (count 0)
    // correctly reads defaults even after a prior overridden launch on this thread.
    g_cpu_spec_count = s->specCount;
    g_cpu_spec_values = s->specValues;
    int32_t coord[13] = {0, 0, 0, 0, 0, 0, s->bx, s->by, s->bz,
                         s->gx, s->gy, s->gz, s->dynShared};
    int64_t gxy = (int64_t) s->gx * s->gy;   // M9: 64-bit — gx*gy can exceed i32,
                                             // wrapping to 0 -> divide-by-zero below
    for (int32_t lin = s->bStart; lin < s->bEnd; ++lin) {
        coord[3] = lin % s->gx;            // ctaid.x
        coord[4] = (lin / s->gx) % s->gy;  // ctaid.y
        coord[5] = (int32_t) (lin / gxy);  // ctaid.z
        s->fn(s->argv, coord);   // per-block; the wrapper loops work-items
    }
}

static void* cajeta_xpu_cpu_worker(void* arg) {
    cajeta_xpu_cpu_run_slice((const struct cajeta_cpu_grid_slice*) arg);
    return NULL;
}

// CPU launch (cajeta-cpu.md Inc 3 + Inc 5A). Resolve the kernel's launcher thunk
// and run the grid->threads loop, parallelized across cores: the gridX blocks
// are chunked across min(gridX, cores) worker threads (the calling thread runs
// the last slice while the others fan out). Below a work-item threshold — or
// with one core / one block — it runs serially, since thread fan-out costs more
// than a small launch saves. Workgroup barriers are safe here even though they
// make work-items rendezvous: fission (Inc 6) realizes a barrier *within* a
// single per-block wrapper call, and each wrapper call runs on one worker — so
// the grid of blocks stays embarrassingly parallel (work-items of a block never
// split across threads). True wave=SIMD-lane vectorization (Inc 5B) layers on
// top of each work-item call.
// The parallel cutover. With the persistent pool (below) a dispatch is a
// broadcast + barrier — no per-launch thread spawn — so the old 4096 figure
// (chosen when each launch paid pthread_create/join) is far too conservative:
// a kernel doing real per-item work parallelizes profitably well below it.
// Overridable at runtime via CAJETA_XPU_CPU_PARALLEL_THRESHOLD for tuning.
#ifndef CAJETA_XPU_CPU_PARALLEL_THRESHOLD
#define CAJETA_XPU_CPU_PARALLEL_THRESHOLD 256   /* work-items */
#endif
#define CAJETA_XPU_CPU_MAX_WORKERS 256

// Spin budgets before falling back to a futex sleep. Split deliberately into a
// WORKER (wakeup) budget and a JOIN (completion) budget because they have
// opposite safety profiles:
//
//   * JOIN spin is caller-side: the launching thread busy-waits for the workers'
//     done-counter to hit zero instead of sleeping on a condvar. Pure latency
//     win, no effect on how the workers run. Safe to spin hard.
//
//   * WORKER spin would have every pool worker busy-wait on the generation
//     counter, so a dispatch broadcast releases them all within nanoseconds —
//     TRULY simultaneous entry into the kernel. That tripped a latent
//     high-concurrency race in barrier-fission kernels (>8 work-items entering
//     the freshly-emitted wrapper at the exact same instant corrupts the stack
//     -> SIGSEGV). Staggered condvar wakeup (the legacy per-launch behavior)
//     never hit it. So WORKER spin defaults to 0 (condvar wakeup, naturally
//     staggered) until that fission race is fixed; JOIN spin carries the perf.
//
// ~2^18 pauses ~= a couple ms ceiling: kernels shorter than that never pay the
// join's futex round-trip.
#ifndef CAJETA_XPU_CPU_JOIN_SPIN
#define CAJETA_XPU_CPU_JOIN_SPIN 262144
#endif
#ifndef CAJETA_XPU_CPU_WORKER_SPIN
#define CAJETA_XPU_CPU_WORKER_SPIN 0
#endif
#if defined(__x86_64__) || defined(__i386__)
#define CAJ_CPU_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define CAJ_CPU_PAUSE() __asm__ __volatile__("yield")
#else
#define CAJ_CPU_PAUSE() ((void) 0)
#endif

// --- Persistent CPU-kernel worker pool --------------------------------------
// A data-parallel @Kernel launch fans its blocks across cores. Spawning fresh
// pthreads per launch (the original path) costs ~5-15us/thread in create+join
// — ruinous for small, frequently-launched kernels (an iterative solver, a
// per-frame image pass, matmul timed best-of-N). This pool creates the worker
// threads ONCE; each subsequent launch is a generation bump + condvar
// broadcast (fork) and a done-counter wait (join). Workers sleep between
// launches, so idle cost is zero. The calling thread always runs the last
// slice itself, so a P-way launch uses P-1 pool workers + the caller.
struct caj_kpool_slice_ref { const struct cajeta_cpu_grid_slice* s; };
static struct {
    int            started;
    int            nthreads;                 // persistent workers (= caller cap - 1)
    pthread_t      threads[CAJETA_XPU_CPU_MAX_WORKERS];
    pthread_mutex_t mu;
    pthread_cond_t  go;                      // workers wait for a new generation
    pthread_cond_t  done;                    // caller waits for active==0
    uint64_t        generation;              // bumped per dispatch
    int             active;                  // pool workers still running this job
    int             njobs;                   // pool slices dispatched this gen
    int             shutdown;
    const struct cajeta_cpu_grid_slice* slices;  // slices[0..njobs-1] for workers
} g_caj_kpool = {
    // The pthread primitives MUST carry their static initializers explicitly.
    // On glibc PTHREAD_*_INITIALIZER is all-zero, so plain static zero-init
    // happened to work there — but on winpthreads (MSYS2/MinGW) the
    // initializers are -1 sentinels that trigger lazy self-init, and a ZEROED
    // mutex/condvar is an invalid object: pthread_cond_wait on it never
    // returns (return codes here are unchecked), so the FIRST CPU-rung kernel
    // launch on Windows hung forever in caj_kpool_wait_gen/caj_kpool_join
    // (specs/windows-vulkan-cpu-forced-hang-spec.md). Bit-identical to the
    // old zero-init on glibc; correct everywhere.
    .mu   = PTHREAD_MUTEX_INITIALIZER,
    .go   = PTHREAD_COND_INITIALIZER,
    .done = PTHREAD_COND_INITIALIZER,
};

// Wait for generation to advance past `seen`. Spin on the atomic generation
// first (ACQUIRE pairs with dispatch's RELEASE store, so slices/njobs/active are
// visible once we see the new value); fall back to a cond_wait under mu after the
// spin budget. Returns the new generation, or 0 on shutdown.
// Effective spin budgets, resolved once from the environment (overriding the
// compile-time defaults) by caj_kpool_ensure before any worker spins.
//   CAJETA_XPU_CPU_WORKER_SPIN  — workers busy-wait for dispatch (default 0:
//       condvar wakeup, staggered, safe for barrier-fission kernels). Set >0 to
//       beat BLAS on barrier-free kernels (matmul) via hot, simultaneous start.
//   CAJETA_XPU_CPU_JOIN_SPIN    — caller busy-waits for completion (default on).
static int caj_worker_spin = -1;
static int caj_join_spin = -1;
static void caj_kpool_resolve_spin(void) {
    if (caj_worker_spin < 0) {
        const char* e = getenv("CAJETA_XPU_CPU_WORKER_SPIN");
        caj_worker_spin = e ? atoi(e) : CAJETA_XPU_CPU_WORKER_SPIN;
        if (caj_worker_spin < 0) caj_worker_spin = 0;
    }
    if (caj_join_spin < 0) {
        const char* e = getenv("CAJETA_XPU_CPU_JOIN_SPIN");
        caj_join_spin = e ? atoi(e) : CAJETA_XPU_CPU_JOIN_SPIN;
        if (caj_join_spin < 0) caj_join_spin = 0;
    }
}

// --- Worker cap + last-launch observability ---------------------------------
//
// `nworkers` was min(nblocks, cores) with no way to ask for fewer and no way
// to find out what was chosen. A scaling curve needs both: a timing whose
// worker count is unknown is not a data point, and a 1/2/4/8/16 sweep cannot
// be run without a bound.
//
// The cap is settable in-process as well as from the environment, because the
// measurement discipline here requires ALTERNATING arm order inside one run —
// a fixed order let a decaying background load read as a speedup once
// already. A process-lifetime environment variable cannot alternate. 0 means
// unlimited, and setting it back to 0 must restore that: a one-way cap would
// silently pin every later launch in the process.
static int caj_worker_cap = -1;                 /* -1 = not yet resolved */
static int32_t caj_last_workers = 0;            /* what the last launch used */

static int caj_resolve_worker_cap(void) {
    int c = __atomic_load_n(&caj_worker_cap, __ATOMIC_ACQUIRE);
    if (c >= 0) return c;
    const char* e = getenv("CAJETA_XPU_CPU_WORKERS");
    c = e ? atoi(e) : 0;
    if (c < 0) c = 0;
    __atomic_store_n(&caj_worker_cap, c, __ATOMIC_RELEASE);
    return c;
}

void __cajeta_xpu_cpu_set_worker_cap(int32_t n) {
    __atomic_store_n(&caj_worker_cap, n < 0 ? 0 : (int) n, __ATOMIC_RELEASE);
}

int32_t __cajeta_xpu_cpu_worker_cap(void) {
    return (int32_t) caj_resolve_worker_cap();
}

int32_t __cajeta_xpu_cpu_last_workers(void) {
    return __atomic_load_n(&caj_last_workers, __ATOMIC_ACQUIRE);
}

static uint64_t caj_kpool_wait_gen(uint64_t seen) {
    for (int spins = 0; spins < caj_worker_spin; ++spins) {
        uint64_t g = __atomic_load_n(&g_caj_kpool.generation, __ATOMIC_ACQUIRE);
        if (g != seen) return g;
        if (__atomic_load_n(&g_caj_kpool.shutdown, __ATOMIC_ACQUIRE)) return 0;
        CAJ_CPU_PAUSE();
    }
    // Slow path: re-check under the mutex to close the lost-wakeup window with
    // dispatch (which bumps generation + broadcasts go while holding mu).
    pthread_mutex_lock(&g_caj_kpool.mu);
    uint64_t g;
    while ((g = __atomic_load_n(&g_caj_kpool.generation, __ATOMIC_ACQUIRE)) == seen
           && !__atomic_load_n(&g_caj_kpool.shutdown, __ATOMIC_ACQUIRE))
        pthread_cond_wait(&g_caj_kpool.go, &g_caj_kpool.mu);
    pthread_mutex_unlock(&g_caj_kpool.mu);
    return __atomic_load_n(&g_caj_kpool.shutdown, __ATOMIC_ACQUIRE) ? 0 : g;
}

// cajeta-profiler 3.2.d — a pool worker is a host thread running program work
// (the kernel body), so §2.1 requires the sampler to see it. Registered around
// the loop rather than inside it, matching the carrier/timer/reactor wrappers:
// every return path unregisters, including the shutdown break below, so a
// module teardown cannot leave a dead handle in the registry for the sampler to
// dereference.
//
// This file is textually part of the runtime TU (cajeta_xpu.c is #included
// after cajeta_rt_core.c), so the registry is a direct call — the plan's note
// about needing an extern declaration was wrong.
static void* caj_kpool_worker_body(void* arg);

static void* caj_kpool_worker_main(void* arg) {
    __cajeta_prof_thread_register();
    void* r = caj_kpool_worker_body(arg);
    __cajeta_prof_thread_unregister();
    return r;
}

static void* caj_kpool_worker_body(void* arg) {
    long myid = (long) (intptr_t) arg;
    // Baseline below the first dispatchable generation (see caj_kpool_dispatch):
    // generation starts at 0, first dispatch bumps it to 1. Starting at 0 makes a
    // dispatch that races ahead of our first wait still register (g != seen),
    // where capturing the live value could lose it -> active stuck -> join hangs.
    uint64_t seen = 0;
    for (;;) {
        uint64_t g = caj_kpool_wait_gen(seen);
        if (g == 0) break;                       // shutdown
        seen = g;
        if (myid < g_caj_kpool.njobs) {
            cajeta_xpu_cpu_run_slice(&g_caj_kpool.slices[myid]);
            // ACQ_REL so the joiner that reads active==0 sees our slice's stores.
            if (__atomic_sub_fetch(&g_caj_kpool.active, 1, __ATOMIC_ACQ_REL) == 0) {
                // Wake a possibly-sleeping joiner. Take mu so the signal can't
                // slip between the joiner's under-lock active check and its wait.
                pthread_mutex_lock(&g_caj_kpool.mu);
                pthread_cond_signal(&g_caj_kpool.done);
                pthread_mutex_unlock(&g_caj_kpool.mu);
            }
        }
    }
    return NULL;
}

// How many pool workers exist right now. A diagnostic, and the only way a test
// can tell "the registry did not grow" from "the launch never forked anything"
// — the second reads as a pass on every assertion that matters.
int32_t __cajeta_xpu_cpu_pool_threads(void) {
    return (int32_t) g_caj_kpool.nthreads;
}

// Lazily create `cap-1` persistent workers (cap = chosen worker count). Grows
// the pool if a later launch wants more workers than exist; never shrinks.
// Caller must NOT hold g_caj_kpool.mu.
static void caj_kpool_ensure(int cap) {
    int want = cap - 1;                       // caller runs one slice itself
    if (want < 0) want = 0;
    if (want > CAJETA_XPU_CPU_MAX_WORKERS) want = CAJETA_XPU_CPU_MAX_WORKERS;
    caj_kpool_resolve_spin();
    pthread_mutex_lock(&g_caj_kpool.mu);
    // mu/go/done carry PTHREAD_*_INITIALIZER via g_caj_kpool's designated
    // initializer (NOT plain zero-init — that is glibc-only and hung Windows);
    // do NOT pthread_mutex_init(&mu) here -- we hold it.
    g_caj_kpool.started = 1;
    __cajeta_live_set_go_multithreaded();     // second-thread barrier (see live-set)
    while (g_caj_kpool.nthreads < want) {
        long id = g_caj_kpool.nthreads;
        if (pthread_create(&g_caj_kpool.threads[id], NULL,
                           caj_kpool_worker_main, (void*) (intptr_t) id) != 0)
            break;                            // spawn failed: cap the pool here
        g_caj_kpool.nthreads++;
    }
    pthread_mutex_unlock(&g_caj_kpool.mu);
}

// Fork-join dispatch: hand slices[0..njobs-1] to pool workers, return after
// they finish. The caller separately runs its own (last) slice between fork
// and join to overlap. njobs must be <= g_caj_kpool.nthreads.
static void caj_kpool_dispatch(const struct cajeta_cpu_grid_slice* slices,
                               int njobs) {
    // Publish the job payload, then RELEASE-store generation: a worker that
    // ACQUIRE-sees the new generation is guaranteed to see slices/njobs/active.
    g_caj_kpool.slices = slices;
    g_caj_kpool.njobs  = njobs;
    __atomic_store_n(&g_caj_kpool.active, njobs, __ATOMIC_RELAXED);
    // Hold mu across the generation bump + broadcast so a worker on the slow
    // (cond_wait) path can't miss the wake. Only this (single) thread writes
    // generation, so a plain read of the current value is fine.
    pthread_mutex_lock(&g_caj_kpool.mu);
    __atomic_store_n(&g_caj_kpool.generation, g_caj_kpool.generation + 1,
                     __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_caj_kpool.go);
    pthread_mutex_unlock(&g_caj_kpool.mu);
}

static void caj_kpool_join(void) {
    for (int spins = 0; spins < caj_join_spin; ++spins) {
        if (__atomic_load_n(&g_caj_kpool.active, __ATOMIC_ACQUIRE) == 0) return;
        CAJ_CPU_PAUSE();
    }
    pthread_mutex_lock(&g_caj_kpool.mu);
    while (__atomic_load_n(&g_caj_kpool.active, __ATOMIC_ACQUIRE) != 0)
        pthread_cond_wait(&g_caj_kpool.done, &g_caj_kpool.mu);
    pthread_mutex_unlock(&g_caj_kpool.mu);
}

// Join and dismantle the persistent pool — the runtime-teardown hook, called
// from __cajeta_task_shutdown. A JIT'd module's pool workers park between
// launches on g_caj_kpool.go, a condvar in MODULE memory; leaving them parked
// past module unload lets a later allocation recycle that address and corrupt
// the futex (glibc's "The futex facility returned an unexpected error code"
// abort mid-suite). Same hazard class as the carrier/timer/reactor joins
// (R9.1/R9.4); the pool was the one thread family without a teardown. No-op
// when the pool never started; the state reset lets a subsequent launch in
// the SAME module (shutdown is also safe to call more than once) lazily
// rebuild a fresh pool via caj_kpool_ensure.
void __cajeta_xpu_kpool_shutdown(void) {
    pthread_mutex_lock(&g_caj_kpool.mu);
    if (!g_caj_kpool.started) {
        pthread_mutex_unlock(&g_caj_kpool.mu);
        return;
    }
    __atomic_store_n(&g_caj_kpool.shutdown, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_caj_kpool.go);
    int n = g_caj_kpool.nthreads;
    pthread_mutex_unlock(&g_caj_kpool.mu);
    for (int i = 0; i < n; ++i)
        pthread_join(g_caj_kpool.threads[i], NULL);
    // Reset for a clean lazy restart: no workers exist now, so clearing the
    // dispatch bookkeeping (generation included — a fresh worker's seen=0
    // baseline must not "see" a stale generation and re-run stale slices)
    // races nothing.
    pthread_mutex_lock(&g_caj_kpool.mu);
    g_caj_kpool.nthreads = 0;
    g_caj_kpool.started = 0;
    g_caj_kpool.njobs = 0;
    g_caj_kpool.slices = NULL;
    __atomic_store_n(&g_caj_kpool.active, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_caj_kpool.generation, (uint64_t) 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_caj_kpool.shutdown, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_caj_kpool.mu);
}

static void cajeta_xpu_launch_cpu(const char* name,
                                  int32_t gridX, int32_t gridY, int32_t gridZ,
                                  int32_t blockX, int32_t blockY, int32_t blockZ,
                                  int32_t sharedBytes, void* argv,
                                  int32_t specCount, const int32_t* specValues) {
    void* p = __cajeta_xpu_lookup_cpu_kernel(name);
    if (!p) {
        fprintf(stderr, "cajeta.xpu: no registered CPU kernel '%s' to launch\n",
                name);
        return;
    }
    cajeta_cpu_launch_fn fn = (cajeta_cpu_launch_fn) p;
    if (gridX < 1) gridX = 1; if (gridY < 1) gridY = 1; if (gridZ < 1) gridZ = 1;
    // L6: sharedBytes becomes a per-block alloca on the worker's stack; an absurd
    // value (the launch's sharedBytes round-tripped through the spec constant)
    // would blow the stack. Bound it — real GPU shared memory is well under this.
    if (sharedBytes < 0 || (uint32_t) sharedBytes > (16u << 20)) {
        fprintf(stderr, "cajeta.xpu: CPU launch sharedBytes %d out of range "
                "(max 16 MiB); not launching '%s'\n", sharedBytes, name);
        return;
    }


    // CAJETA_XPU_CPU_SERIAL forces single-threaded execution — a deterministic
    // debug/oracle mode and the serial baseline for benchmarking. Read once.
    static int force_serial = -1;
    if (force_serial < 0) force_serial = getenv("CAJETA_XPU_CPU_SERIAL") ? 1 : 0;

    // Blocks fan out across threads (never the work-items of one block); the
    // 3-D grid is flattened to nblocks linear indices, decoded to ctaid.xyz in
    // run_slice.
    // M9: compute in 64-bit — gridX*gridY*gridZ in int32 wraps (negative ->
    // serial loop never runs, a silent no-op; or to 0 -> divide-by-zero in
    // run_slice). The CPU path indexes blocks with int32, so clamp an absurd grid
    // (>2^31 blocks runs serially anyway) with a diagnostic rather than wrap.
    int64_t nblocks64 = (int64_t) gridX * (int64_t) gridY * (int64_t) gridZ;
    if (nblocks64 > INT32_MAX) {
        fprintf(stderr, "cajeta.xpu: CPU grid block count %lld exceeds INT32_MAX; "
                "clamping to %d\n", (long long) nblocks64, INT32_MAX);
        nblocks64 = INT32_MAX;
    }
    int32_t nblocks = (int32_t) nblocks64;
    int64_t blockSize = (int64_t) (blockX > 0 ? blockX : 1) *
                        (int64_t) (blockY > 0 ? blockY : 1) *
                        (int64_t) (blockZ > 0 ? blockZ : 1);
    int64_t total = (int64_t) nblocks * blockSize;
#if defined(_WIN32)
    // sysconf/_SC_NPROCESSORS_ONLN is POSIX; on Windows ask the Win32 API
    // (windows.h is included at file scope above for the fiber/file-lock paths).
    SYSTEM_INFO cpu_si;
    GetSystemInfo(&cpu_si);
    long cores = (long) cpu_si.dwNumberOfProcessors;
#else
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (cores < 1) cores = 1;
    int32_t nworkers = (int32_t) ((long) nblocks < cores ? (long) nblocks : cores);
    if (nworkers > CAJETA_XPU_CPU_MAX_WORKERS) nworkers = CAJETA_XPU_CPU_MAX_WORKERS;
    // The cap bounds it; 0 leaves it alone.
    int wcap = caj_resolve_worker_cap();
    if (wcap > 0 && nworkers > (int32_t) wcap) nworkers = (int32_t) wcap;

    // Serial path: forced, tiny launch, single core, or a single block.
    if (force_serial || nblocks <= 1 || nworkers <= 1 ||
        total < CAJETA_XPU_CPU_PARALLEL_THRESHOLD) {
        // Record 1, not `nworkers`: the caller wants what RAN, and every one
        // of these conditions means one thread ran the whole grid.
        __atomic_store_n(&caj_last_workers, 1, __ATOMIC_RELEASE);
        struct cajeta_cpu_grid_slice all = {fn, (void**) argv,
                                            blockX, blockY, blockZ,
                                            gridX, gridY, gridZ,
                                            0, nblocks, sharedBytes,
                                            specCount, specValues};
        cajeta_xpu_cpu_run_slice(&all);
        return;
    }

    // Parallel fan-out: chunk the nblocks linear block indices across `nworkers`.
    // Dispatch slices[0..nworkers-2] to the persistent pool; the calling thread
    // runs slices[nworkers-1] itself (overlapping the workers), then joins.
    __atomic_store_n(&caj_last_workers, nworkers, __ATOMIC_RELEASE);
    struct cajeta_cpu_grid_slice slices[CAJETA_XPU_CPU_MAX_WORKERS];
    int32_t base = nblocks / nworkers, rem = nblocks % nworkers, cx = 0;
    for (int32_t i = 0; i < nworkers; ++i) {
        int32_t count = base + (i < rem ? 1 : 0);
        slices[i].fn = fn;
        slices[i].argv = (void**) argv;
        slices[i].bx = blockX; slices[i].by = blockY; slices[i].bz = blockZ;
        slices[i].gx = gridX;  slices[i].gy = gridY;  slices[i].gz = gridZ;
        slices[i].bStart = cx;
        slices[i].bEnd = cx + count;
        slices[i].dynShared = sharedBytes;
        slices[i].specCount = specCount;
        slices[i].specValues = specValues;
        cx += count;
    }
    caj_kpool_ensure(nworkers);
    // If the pool couldn't spawn enough workers, run the surplus slices inline
    // after the caller's slice (correctness over parallelism). njobs is capped
    // to the live pool size; slices [njobs .. nworkers-2] (if any) plus the last
    // are run by the calling thread.
    int njobs = nworkers - 1;
    if (njobs > g_caj_kpool.nthreads) njobs = g_caj_kpool.nthreads;
    caj_kpool_dispatch(slices, njobs);
    cajeta_xpu_cpu_run_slice(&slices[nworkers - 1]);
    for (int32_t i = njobs; i < nworkers - 1; ++i)
        cajeta_xpu_cpu_run_slice(&slices[i]);   // pool-short surplus, inline
    caj_kpool_join();
}

// --- Buffer<T> device memory (backend-dispatched) ---------------------------
// The Buffer<T> stdlib methods (alloc/upload/download/free) are ordinary
// Cajeta now; they construct the handle via `heap`/`stack` + the generated
// constructor and forward byte-sized primitives here. The element byte size
// is supplied by the compiler (Buffer<T>.elementBytes() intrinsic), so these
// symbols are monomorphism-independent: they speak only int64 handles and
// byte counts. The int64 handle is the active backend's device pointer (CUDA/
// HIP), buffer-table index (Vulkan), or host block (CPU) — consistent within a
// run because the backend is fixed at first device touch.
//
// `host` is a Cajeta T[] header — { i64 count, [count x T] data } laid out
// contiguously — so the element bytes begin at offset 8 (matches
// __cajeta_new_array_header). byteCount is count * sizeof(T), already
// computed caller-side.
// `self` is the Buffer instance pointer the instance-method forwarder passes;
// the device side is keyed on the int64 handle, so self is ignored.
// Buffer MemoryKind ordinals — the stable native contract; MUST match
// runtime/src/cajeta/xpu/core/MemoryKind.cajeta. Device = device-local memory
// with explicit upload/download (the default, original behaviour); Pinned =
// page-locked, device-accessible host memory; Unified = managed memory one
// pointer host AND device see (zero-copy on an integrated GPU).
enum {
    CAJ_MEMKIND_DEVICE  = 0,
    CAJ_MEMKIND_PINNED  = 1,
    CAJ_MEMKIND_UNIFIED = 2
};
int64_t __cajeta_xpu_buffer_alloc(void* self, uint64_t byteCount, int32_t kind) {
    (void) self;
    if (byteCount == 0) return 0;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA: {
            cajeta_cudeviceptr p = 0;
            if (kind == CAJ_MEMKIND_UNIFIED && g_xpu_cuda.cuMemAllocManaged) {
                // CU_MEM_ATTACH_GLOBAL = 1
                if (g_xpu_cuda.cuMemAllocManaged(&p, (size_t) byteCount, 1) != 0) return 0;
                return (int64_t) p;
            }
            if (kind == CAJ_MEMKIND_PINNED && g_xpu_cuda.cuMemHostAlloc) {
                void* hp = NULL;
                // CU_MEMHOSTALLOC_DEVICEMAP = 2 → device-accessible
                if (g_xpu_cuda.cuMemHostAlloc(&hp, (size_t) byteCount, 2) != 0) return 0;
                return (int64_t) (intptr_t) hp;
            }
            if (g_xpu_cuda.cuMemAlloc(&p, (size_t) byteCount) != 0) return 0;
            return (int64_t) p;
        }
        case CAJ_XPU_HIP: {
            void* p = NULL;
            if (kind == CAJ_MEMKIND_UNIFIED && g_xpu_hip.hipMallocManaged) {
                // hipMemAttachGlobal = 1
                if (g_xpu_hip.hipMallocManaged(&p, (size_t) byteCount, 1) != 0) return 0;
                return (int64_t) (intptr_t) p;
            }
            if (kind == CAJ_MEMKIND_PINNED && g_xpu_hip.hipHostMalloc) {
                // hipHostMallocMapped = 0x2 → device-accessible
                if (g_xpu_hip.hipHostMalloc(&p, (size_t) byteCount, 0x2) != 0) return 0;
                return (int64_t) (intptr_t) p;
            }
            if (g_xpu_hip.hipMalloc(&p, (size_t) byteCount) != 0) return 0;
            return (int64_t) (intptr_t) p;
        }
        case CAJ_XPU_VULKAN:
            // Vulkan buffers are already host-visible + coherent on this device
            // (effectively unified); kind needs no distinct path. handle =
            // buffer-table index.
            (void) kind;
            return cajeta_xpu_vk_alloc(byteCount);
        case CAJ_XPU_CPU: {
            // CPU "device" memory = host; every kind is host-accessible already.
            void* p = malloc((size_t) byteCount);
            return (int64_t) (intptr_t) p;
        }
        default: return 0;   // none: diagnostic emitted
    }
}
// Direct host access to a host-accessible buffer (Pinned/Unified, or CPU/Vulkan
// mapped) with NO device-transfer API — a plain memcpy in the shared address
// space (zero-copy: no PCIe copy / no managed migration on a discrete GPU, a
// host memcpy on an APU). dir != 0 stores host[]→buffer; dir == 0 loads
// buffer→host[]. A plain Device buffer on a discrete GPU has no host mapping, so
// this no-ops (use upload/download there). `host` is a cajeta array (8-byte
// header skipped); kind selects whether the HIP/CUDA handle is host-accessible.
void __cajeta_xpu_buffer_host_copy(void* self, int64_t handle, void* host,
                                   uint64_t byteCount, int32_t dir, int32_t kind) {
    (void) self;
    if (!handle || !host || byteCount == 0) return;
    void* hostArr = (void*) ((char*) host + 8);   // skip cajeta array header
    void* hp = NULL;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU:
            hp = (void*) (intptr_t) handle;
            break;
        case CAJ_XPU_HIP:
        case CAJ_XPU_CUDA:
            // managed (Unified) and pinned host handles are host-accessible
            // pointers; plain Device memory is not.
            if (kind == CAJ_MEMKIND_UNIFIED || kind == CAJ_MEMKIND_PINNED)
                hp = (void*) (intptr_t) handle;
            break;
        case CAJ_XPU_VULKAN:
            cajeta_xpu_vk_note_host_access(handle);   // order vs the open batch
            if (dir) {
                hp = cajeta_xpu_vk_mapped(handle);   // WC writes stream fine
            } else {
                // Reads through a write-combined mapping crawl; the read
                // helper routes non-cached sources via cached staging.
                cajeta_xpu_vk_read(handle, hostArr, byteCount);
                return;
            }
            break;
        default:
            break;
    }
    if (!hp) return;   // not host-accessible (Device on a discrete GPU)
    if (dir) memcpy(hp, hostArr, (size_t) byteCount);
    else     memcpy(hostArr, hp, (size_t) byteCount);
}
// Async host↔device copies on a stream (Buffer.uploadAsync/downloadAsync). The
// copy is enqueued on `stream` (a Stream handle; 0 = the default stream) and
// completes by the next sync of that stream — so it overlaps other work queued
// elsewhere. CUDA/HIP issue the real async memcpy (best paired with pinned/
// unified host memory); CPU and the Vulkan host-coherent map copy synchronously
// (no async path, but semantically correct — done by the time sync returns).
void __cajeta_xpu_buffer_upload_async(void* self, int64_t handle, void* host,
                                      uint64_t byteCount, int64_t stream) {
    (void) self;
    if (!handle || !host || byteCount == 0) return;
    const void* data = (const void*) ((const char*) host + 8);
    void* st = (void*) (intptr_t) stream;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            if (g_xpu_cuda.cuMemcpyHtoDAsync)
                g_xpu_cuda.cuMemcpyHtoDAsync((cajeta_cudeviceptr) handle, data,
                                             (size_t) byteCount, st);
            else
                g_xpu_cuda.cuMemcpyHtoD((cajeta_cudeviceptr) handle, data,
                                        (size_t) byteCount);
            return;
        case CAJ_XPU_HIP:
            if (g_xpu_hip.hipMemcpyHtoDAsync)
                g_xpu_hip.hipMemcpyHtoDAsync((void*) (intptr_t) handle, data,
                                             (size_t) byteCount, st);
            else
                g_xpu_hip.hipMemcpyHtoD((void*) (intptr_t) handle, data,
                                        (size_t) byteCount);
            return;
        case CAJ_XPU_VULKAN: {
            cajeta_xpu_vk_note_host_access(handle);   // order vs the open batch
            void* m = cajeta_xpu_vk_mapped(handle);   // coherent map: immediate
            if (m) memcpy(m, data, (size_t) byteCount);
            return;
        }
        case CAJ_XPU_CPU:
            memcpy((void*) (intptr_t) handle, data, (size_t) byteCount);
            return;
        default: return;
    }
}
void __cajeta_xpu_buffer_download_async(void* self, int64_t handle, void* host,
                                        uint64_t byteCount, int64_t stream) {
    (void) self;
    if (!handle || !host || byteCount == 0) return;
    void* data = (void*) ((char*) host + 8);
    void* st = (void*) (intptr_t) stream;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            if (g_xpu_cuda.cuMemcpyDtoHAsync)
                g_xpu_cuda.cuMemcpyDtoHAsync(data, (cajeta_cudeviceptr) handle,
                                             (size_t) byteCount, st);
            else
                g_xpu_cuda.cuMemcpyDtoH(data, (cajeta_cudeviceptr) handle,
                                        (size_t) byteCount);
            return;
        case CAJ_XPU_HIP:
            if (g_xpu_hip.hipMemcpyDtoHAsync)
                g_xpu_hip.hipMemcpyDtoHAsync(data, (void*) (intptr_t) handle,
                                             (size_t) byteCount, st);
            else
                g_xpu_hip.hipMemcpyDtoH(data, (void*) (intptr_t) handle,
                                        (size_t) byteCount);
            return;
        case CAJ_XPU_VULKAN: {
            cajeta_xpu_vk_note_host_access(handle);   // order vs the open batch
            cajeta_xpu_vk_read(handle, data, byteCount);
            return;
        }
        case CAJ_XPU_CPU:
            memcpy(data, (const void*) (intptr_t) handle, (size_t) byteCount);
            return;
        default: return;
    }
}
// Stream create/sync/destroy — defined here (not with stream_current above)
// because they switch on the backend enum + cajeta_xpu_active_backend(), which
// are declared further down. Handle 0 = the default stream (the v1 behaviour).
int64_t __cajeta_xpu_stream_create(void) {
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA: {
            void* s = NULL;
            if (g_xpu_cuda.cuStreamCreate &&
                g_xpu_cuda.cuStreamCreate(&s, 0) == 0)
                return (int64_t) (intptr_t) s;
            return 0;   // no driver entry → fall back to the default stream
        }
        case CAJ_XPU_HIP: {
            void* s = NULL;
            if (g_xpu_hip.hipStreamCreate &&
                g_xpu_hip.hipStreamCreate(&s) == 0)
                return (int64_t) (intptr_t) s;
            return 0;
        }
        default: return 0;   // CPU/Vulkan: synchronous; the default stream
    }
}
void __cajeta_xpu_stream_sync(void* self, int64_t handle) {
    (void) self;
    void* st = (void*) (intptr_t) handle;
    if (st) {
        // Drain just this stream (its async copies + launches).
        switch (cajeta_xpu_active_backend()) {
            case CAJ_XPU_CUDA:
                if (g_xpu_cuda.cuStreamSynchronize) {
                    g_xpu_cuda.cuStreamSynchronize(st);
                    caj_cuda_bracket_drain();
                    return;
                }
                break;
            case CAJ_XPU_HIP:
                if (g_xpu_hip.hipStreamSynchronize) {
                    g_xpu_hip.hipStreamSynchronize(st); return;
                }
                break;
            default: break;
        }
    }
    cajeta_xpu_sync_active();   // default stream (0) or no per-stream entry
}
void __cajeta_xpu_stream_destroy(void* self, int64_t handle) {
    (void) self;
    void* st = (void*) (intptr_t) handle;
    if (!st) return;   // the default stream is not destroyed
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            if (g_xpu_cuda.cuStreamDestroy) g_xpu_cuda.cuStreamDestroy(st);
            return;
        case CAJ_XPU_HIP:
            if (g_xpu_hip.hipStreamDestroy) g_xpu_hip.hipStreamDestroy(st);
            return;
        default: return;
    }
}

// --- Event -----------------------------------------------------------------
// Cross-stream + host synchronisation. The handle (int64) IS the backend event
// object; create() returns it (0 = unavailable). On CUDA/HIP these wrap a real
// cuEvent/hipEvent so a second stream can wait on a first stream's recorded
// point device-side; on CPU/Vulkan work is synchronous, so an event is a
// sentinel (handle 1) that is always already-signaled (record/wait no-op, query
// true). Event and Fence share the backend mechanism — Event is the device-
// facing surface (Stream.waitFor), Fence the host-facing one.
int64_t __cajeta_xpu_event_create(void) {
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA: {
            void* e = NULL;
            if (g_xpu_cuda.cuEventCreate &&
                g_xpu_cuda.cuEventCreate(&e, 0) == 0)
                return (int64_t) (intptr_t) e;
            return 0;
        }
        case CAJ_XPU_HIP: {
            void* e = NULL;
            if (g_xpu_hip.hipEventCreate &&
                g_xpu_hip.hipEventCreate(&e) == 0)
                return (int64_t) (intptr_t) e;
            return 0;
        }
        default: return 1;   // CPU/Vulkan: synchronous, always-signaled sentinel
    }
}
void __cajeta_xpu_event_record(void* self, int64_t handle, int64_t streamHandle) {
    (void) self;
    void* e = (void*) (intptr_t) handle;
    void* st = (void*) (intptr_t) streamHandle;   // 0 = default stream
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            if (e && g_xpu_cuda.cuEventRecord) g_xpu_cuda.cuEventRecord(e, st);
            return;
        case CAJ_XPU_HIP:
            if (e && g_xpu_hip.hipEventRecord) g_xpu_hip.hipEventRecord(e, st);
            return;
        case CAJ_XPU_VULKAN:
            // The sentinel event's contract is "already signaled": everything
            // before the record is complete. Batched submission makes that a
            // promise the record must KEEP — land the open batch here.
            cajeta_xpu_vk_flush();
            return;
        default: return;   // CPU: nothing to record (synchronous)
    }
}
void __cajeta_xpu_event_wait(void* self, int64_t handle) {
    (void) self;
    void* e = (void*) (intptr_t) handle;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            if (e && g_xpu_cuda.cuEventSynchronize)
                g_xpu_cuda.cuEventSynchronize(e);
            return;
        case CAJ_XPU_HIP:
            if (e && g_xpu_hip.hipEventSynchronize)
                g_xpu_hip.hipEventSynchronize(e);
            return;
        default: return;   // CPU/Vulkan: work already done
    }
}
bool __cajeta_xpu_event_query(void* self, int64_t handle) {
    (void) self;
    void* e = (void*) (intptr_t) handle;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            if (e && g_xpu_cuda.cuEventQuery)
                return g_xpu_cuda.cuEventQuery(e) == 0;
            return true;
        case CAJ_XPU_HIP:
            if (e && g_xpu_hip.hipEventQuery)
                return g_xpu_hip.hipEventQuery(e) == 0;
            return true;
        default: return true;   // CPU/Vulkan: always complete
    }
}
void __cajeta_xpu_event_destroy(void* self, int64_t handle) {
    (void) self;
    void* e = (void*) (intptr_t) handle;
    if (!e) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            if (g_xpu_cuda.cuEventDestroy) g_xpu_cuda.cuEventDestroy(e);
            return;
        case CAJ_XPU_HIP:
            if (g_xpu_hip.hipEventDestroy) g_xpu_hip.hipEventDestroy(e);
            return;
        default: return;
    }
}

// Stream.waitFor(event): insert a device-side wait on `event` into `stream`, so
// future launches on `stream` start only after `event` is signaled on its source
// stream. Synchronous backends (CPU/Vulkan) need no wait — ordering already holds.
void __cajeta_xpu_stream_wait_for(void* self, int64_t streamHandle,
                                  int64_t eventHandle) {
    (void) self;
    void* st = (void*) (intptr_t) streamHandle;   // 0 = default stream
    void* e = (void*) (intptr_t) eventHandle;
    if (!e) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            if (g_xpu_cuda.cuStreamWaitEvent)
                g_xpu_cuda.cuStreamWaitEvent(st, e, 0);
            return;
        case CAJ_XPU_HIP:
            if (g_xpu_hip.hipStreamWaitEvent)
                g_xpu_hip.hipStreamWaitEvent(st, e, 0);
            return;
        default: return;
    }
}

// --- Fence -----------------------------------------------------------------
// Host-observable signal. v1 backs Fence with the same backend event object as
// Event (an event IS host-waitable via cuEvent/hipEventSynchronize/Query):
// signal(stream) records the event at the stream's tail; waitHost()/query()
// block/poll the host on it. On CPU/Vulkan the synchronous sentinel applies.
int64_t __cajeta_xpu_fence_create(void) { return __cajeta_xpu_event_create(); }
void __cajeta_xpu_fence_signal(void* self, int64_t handle, int64_t streamHandle) {
    __cajeta_xpu_event_record(self, handle, streamHandle);
}
void __cajeta_xpu_fence_wait(void* self, int64_t handle) {
    __cajeta_xpu_event_wait(self, handle);
}
bool __cajeta_xpu_fence_query(void* self, int64_t handle) {
    return __cajeta_xpu_event_query(self, handle);
}
void __cajeta_xpu_fence_destroy(void* self, int64_t handle) {
    __cajeta_xpu_event_destroy(self, handle);
}
void __cajeta_xpu_buffer_upload(void* self, int64_t handle, void* host,
                                uint64_t byteCount) {
    (void) self;
    if (!handle || !host || byteCount == 0) return;
    const void* data = (const void*) ((const char*) host + 8);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            g_xpu_cuda.cuMemcpyHtoD((cajeta_cudeviceptr) handle, data,
                                    (size_t) byteCount);
            return;
        case CAJ_XPU_HIP:
            g_xpu_hip.hipMemcpyHtoD((void*) (intptr_t) handle, data,
                                    (size_t) byteCount);
            return;
        case CAJ_XPU_VULKAN: {
            cajeta_xpu_vk_note_host_access(handle);   // order vs the open batch
            void* m = cajeta_xpu_vk_mapped(handle);   // host-coherent mapping
            if (m) memcpy(m, data, (size_t) byteCount);
            return;
        }
        case CAJ_XPU_CPU:
            memcpy((void*) (intptr_t) handle, data, (size_t) byteCount);
            return;
        default: return;
    }
}
void __cajeta_xpu_buffer_download(void* self, int64_t handle, void* host,
                                  uint64_t byteCount) {
    (void) self;
    if (!handle || !host || byteCount == 0) return;
    void* data = (void*) ((char*) host + 8);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            g_xpu_cuda.cuMemcpyDtoH(data, (cajeta_cudeviceptr) handle,
                                    (size_t) byteCount);
            return;
        case CAJ_XPU_HIP:
            g_xpu_hip.hipMemcpyDtoH(data, (void*) (intptr_t) handle,
                                    (size_t) byteCount);
            return;
        case CAJ_XPU_VULKAN: {
            cajeta_xpu_vk_note_host_access(handle);   // order vs the open batch
            cajeta_xpu_vk_read(handle, data, byteCount);
            return;
        }
        case CAJ_XPU_CPU:
            memcpy(data, (const void*) (intptr_t) handle, (size_t) byteCount);
            return;
        default: return;
    }
}
void __cajeta_xpu_buffer_free(void* self, int64_t handle, int32_t kind) {
    (void) self;
    if (!handle) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            // Pinned host memory frees with cuMemFreeHost; device + managed
            // (Unified) free with cuMemFree.
            if (kind == CAJ_MEMKIND_PINNED && g_xpu_cuda.cuMemFreeHost)
                g_xpu_cuda.cuMemFreeHost((void*) (intptr_t) handle);
            else
                g_xpu_cuda.cuMemFree((cajeta_cudeviceptr) handle);
            return;
        case CAJ_XPU_HIP:
            // Pinned host memory frees with hipHostFree; device + managed
            // (Unified) free with hipFree.
            if (kind == CAJ_MEMKIND_PINNED && g_xpu_hip.hipHostFree)
                g_xpu_hip.hipHostFree((void*) (intptr_t) handle);
            else
                g_xpu_hip.hipFree((void*) (intptr_t) handle);
            return;
        case CAJ_XPU_VULKAN: (void) kind; cajeta_xpu_vk_free(handle); return;
        case CAJ_XPU_CPU:    (void) kind; free((void*) (intptr_t) handle); return;
        default: return;
    }
}
// Buffer.slice: resolve a sub-range base from a parent handle + byte offset.
// Pointer backends (CUDA/HIP/CPU) fold the offset into the device pointer; the
// returned handle indexes the slice's first element exactly like a base buffer,
// so the launch-arg and upload/download paths need no offset-awareness. Vulkan
// (handle = buffer-table index) allocates a borrowing view slot that carries
// the descriptor offset. The returned handle is non-owning on every backend —
// Buffer.owned is false for a view, so its drop never frees this.
int64_t __cajeta_xpu_buffer_slice(void* self, int64_t handle, uint64_t byteOffset) {
    (void) self;
    if (!handle) return 0;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
        case CAJ_XPU_HIP:
        case CAJ_XPU_CPU:
            return handle + (int64_t) byteOffset;   // pointer + byte offset
        case CAJ_XPU_VULKAN:
            return cajeta_xpu_vk_slice(handle, byteOffset);
        default: return 0;
    }
}

// Buffer.slice release: drop a view's backend record. Pointer backends fold
// the offset into the handle (nothing was allocated — no-op); Vulkan allocated
// a borrowing view slot in its buffer table, which is cleared here. Called by
// the view KernelBuffer's drop/free, never for owning handles.
void __cajeta_xpu_buffer_slice_release(void* self, int64_t handle) {
    (void) self;
    if (!handle) return;
    if (cajeta_xpu_active_backend() == CAJ_XPU_VULKAN)
        cajeta_xpu_vk_view_release(handle);
}

// --- HIP texture helpers (Item 8 Stage C) -----------------------------------
