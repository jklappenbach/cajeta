// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// --- registered kernel modules (device images keyed by entry name + backend) -
// Each backend's ctor registers ITS image per @Kernel; the launch path resolves
// by (name, active backend). backend == -1 is legacy and matches any requester.
struct cajeta_xpu_module {
    char name[256];
    int backend;      // CAJ_XPU_* id of the image's consumer, or -1 (legacy/any)
    const void* image;
    uint64_t len;     // image byte length (SPIR-V needs it; CUDA/HIP ignore it)
    void* module;     // CUmodule/hipModule, lazily loaded
    void* function;   // CUfunction/hipFunction, lazily resolved
};
// An overflow is reported at registration (cajeta_xpu_register_module_impl).
#define CAJETA_XPU_MAX_MODULES 1024
static struct cajeta_xpu_module g_xpu_modules[CAJETA_XPU_MAX_MODULES];
static int g_xpu_module_count;

// Find a registered image for `name` under `backend` (-1 = any): an exact
// backend match wins, a legacy (-1) entry serves anyone. Caller holds the lock.
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
// The Stream handle (int64) is the backend stream object: 0 = the default
// stream; create() makes a real one so copies and launches queue independently.
static void cajeta_xpu_sync_active(void);

int64_t __cajeta_xpu_stream_current(void) { return 0; }   // the default stream

// --- Thread / Workgroup coordinate readers ---------------------------------
// Zero on the host emulation; device lowerings never call these.
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
// Wave / Group / Quad host stubs: the CPU emulation is a width-1 wave, so every
// cross-lane op is the identity and the device lowerings fold them instead.
uint32_t __cajeta_xpu_wave_width(void) { return 1; }
uint32_t __cajeta_xpu_group_width(void) { return 1; }
uint32_t __cajeta_xpu_group_lane_id(void) { return 0; }
uint32_t __cajeta_xpu_group_row_id(void) { return 0; }
// Group.mac int8 tier: the lowerer intercepts this, but the symbol must exist
// for JIT materialization with the @Native arity/types (i32, v16i8, v16i8).
typedef int8_t cajeta_v16i8 __attribute__((vector_size(16)));
int32_t __cajeta_xpu_group_mac_i8(int32_t acc, cajeta_v16i8 a, cajeta_v16i8 b) {
    (void) a; (void) b;
    return acc;
}
// NOT the wave reduce: on CPU the SIMD lanes carry independent rows.
float __cajeta_xpu_group_reduce_f32(int32_t op, float value) {
    (void) op;
    return value;
}
float __cajeta_xpu_group_reduce_f32_seg(int32_t segment, int32_t op, float value) {
    (void) segment; (void) op;
    return value;
}
int32_t __cajeta_xpu_group_stripe(int32_t n) { return n; }
uint32_t __cajeta_xpu_wave_lane_id(void) { return 0; }
bool __cajeta_xpu_wave_is_first_lane(void) { return true; }
uint32_t __cajeta_xpu_wave_shuffle_sync_u32(uint32_t value, uint32_t srcLane) {
    (void)srcLane; return value;
}
uint64_t __cajeta_xpu_wave_ballot_sync(bool predicate) {
    return predicate ? 1ULL : 0ULL;
}
uint32_t __cajeta_xpu_wave_reduce_sum_u32(uint32_t value) { return value; }
uint32_t __cajeta_xpu_wave_reduce_max_u32(uint32_t value) { return value; }
uint32_t __cajeta_xpu_wave_reduce_min_u32(uint32_t value) { return value; }
uint32_t __cajeta_xpu_wave_reduce_and_u32(uint32_t value) { return value; }
uint32_t __cajeta_xpu_wave_reduce_or_u32(uint32_t value) { return value; }
uint32_t __cajeta_xpu_wave_reduce_xor_u32(uint32_t value) { return value; }
// Mask-as-data spellings (compiler-generated): the guard travels as an explicit
// lane-active argument so LoopVectorize cannot scalarize the cross-lane op.
float __cajeta_xpu_wave_reduce_sum_f32(float value) { return value; }
float __cajeta_xpu_wave_reduce_max_f32(float value) { return value; }
float __cajeta_xpu_wave_reduce_sum_f32_seg(float value, uint32_t segment) { (void) segment; return value; }
float __cajeta_xpu_wave_reduce_max_f32_seg(float value, uint32_t segment) { (void) segment; return value; }
uint32_t __cajeta_xpu_wave_reduce_sum_u32_m(uint32_t value, _Bool active) { return active ? value : 0u; }
uint32_t __cajeta_xpu_wave_reduce_max_u32_m(uint32_t value, _Bool active) { return active ? value : 0u; }
uint32_t __cajeta_xpu_wave_reduce_min_u32_m(uint32_t value, _Bool active) { return active ? value : 0xFFFFFFFFu; }
uint32_t __cajeta_xpu_wave_reduce_and_u32_m(uint32_t value, _Bool active) { return active ? value : 0xFFFFFFFFu; }
uint32_t __cajeta_xpu_wave_reduce_or_u32_m(uint32_t value, _Bool active) { return active ? value : 0u; }
uint32_t __cajeta_xpu_wave_reduce_xor_u32_m(uint32_t value, _Bool active) { return active ? value : 0u; }
float __cajeta_xpu_wave_reduce_sum_f32_m(float value, _Bool active) { return active ? value : 0.0f; }
float __cajeta_xpu_wave_reduce_max_f32_m(float value, _Bool active) { return active ? value : -3.402823466e38f; }
// Width-1 fallback: lane 0's exclusive prefix is the identity (0 sum, 1 product).
uint32_t __cajeta_xpu_wave_prefix_sum_u32(uint32_t value) { (void)value; return 0; }
uint32_t __cajeta_xpu_wave_prefix_product_u32(uint32_t value) { (void)value; return 1; }
uint32_t __cajeta_xpu_wave_rotate_u32(uint32_t value, uint32_t delta) {
    (void)delta; return value;
}

uint32_t __cajeta_xpu_quad_broadcast(uint32_t value, uint32_t index) {
    (void)index; return value;
}
uint32_t __cajeta_xpu_quad_swap_horizontal(uint32_t value) { return value; }
uint32_t __cajeta_xpu_quad_swap_vertical(uint32_t value) { return value; }
uint32_t __cajeta_xpu_quad_swap_diagonal(uint32_t value) { return value; }
bool __cajeta_xpu_quad_all(bool predicate) { return predicate; }
bool __cajeta_xpu_quad_any(bool predicate) { return predicate; }

// Per-invocation bit ops: NOT cross-lane — pure scalar functions of one u32, so
// this host code is exactly what the device emits inline.
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
// The CPU backend lowers each @Kernel to a host function; its registration ctor
// calls register_cpu_kernel(name, fn) and the launch path resolves by name.
#ifndef CAJETA_XPU_CPU_KERNEL_MAX
#define CAJETA_XPU_CPU_KERNEL_MAX 256
#endif
static struct { const char* name; void* fn; } g_cpu_kernels[CAJETA_XPU_CPU_KERNEL_MAX];
static int g_cpu_kernel_count = 0;

// Register a CPU-backend kernel under `name`; last writer wins. The name is
// strdup'd, since a JIT'd ctor's string dies with its module.
void __cajeta_xpu_register_cpu_kernel(const char* name, void* fn) {
    if (!name || !fn) return;
    for (int i = 0; i < g_cpu_kernel_count; ++i) {
        if (g_cpu_kernels[i].name && strcmp(g_cpu_kernels[i].name, name) == 0) {
            g_cpu_kernels[i].fn = fn;
            return;
        }
    }
    if (g_cpu_kernel_count < CAJETA_XPU_CPU_KERNEL_MAX) {
        g_cpu_kernels[g_cpu_kernel_count].name = strdup(name);
        g_cpu_kernels[g_cpu_kernel_count].fn = fn;
        ++g_cpu_kernel_count;
    }
}

// Resolve a registered CPU kernel by name; NULL when absent.
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
// A binary can bundle several backends; the first device touch selects and
// caches the highest-priority BUNDLED and AVAILABLE one (the ids are that order).
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

// Probe a backend's availability. Caller holds g_xpu_cuda_lock, so this calls
// the *_init_locked variants — the locking *_ready wrappers would deadlock.
static int cajeta_xpu_backend_available_locked(int id) {
    switch (id) {
        case CAJ_XPU_CUDA:   return cajeta_xpu_cuda_init_locked();
        case CAJ_XPU_HIP:    return cajeta_xpu_hip_init_locked();
        case CAJ_XPU_VULKAN: return cajeta_xpu_vulkan_init_locked();
        case CAJ_XPU_CPU:    return 1;
        default:             return 0;
    }
}

// In-process backend force (Device.force), read ahead of the env var. -1 = none.
static int g_xpu_forced_api = -1;

// Returns 1 when the force landed before selection, 0 when it is already too late.
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

// Device.activeBackend() — the selected backend id, or -1 when none is
// available. NOTE this SELECTS if selection has not happened (a device touch).
int32_t __cajeta_xpu_active_backend_id(void) {
    return (int32_t) cajeta_xpu_active_backend();
}

// Device geometry: the machine shape a kernel needs to size itself, from the
// same query DeviceProfile uses, cached once. 0 = UNKNOWN, never a budget.
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

// Device.memoryBytes() — the active device's total visible memory, 0 when the
// backend cannot answer. A device touch; on a UMA part HIP reports the GTT pool.
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

extern int cajeta_xpu_optix_available(void);

// Device.supports(Capability) — does the active device advertise `cap` natively?
// `cap` is the Capability.cajeta ordinal; append cases, never renumber.
int32_t __cajeta_xpu_device_supports(int32_t cap) {
    int be = cajeta_xpu_active_backend();
    switch (cap) {
        case 0:  // RayQueryNative — hardware INLINE ray query
            // CUDA is intentionally false: OptiX has no inline ray query (RT
            // cores are reached only through a pipeline) — that is case 1.
#if defined(CAJETA_RT_HAS_VULKAN)
            return (be == CAJ_XPU_VULKAN && g_xpu_vk.rayQuery) ? 1 : 0;
#else
            (void) be; return 0;
#endif
        case 1:  // RayQueryRtCore — pipeline-based RT-core ray query (NVIDIA OptiX)
            // The CUDA RT-core path, opt-in with CAJETA_GPU_AS_IMPL=optix.
            return (be == CAJ_XPU_CUDA && cajeta_xpu_optix_available()) ? 1 : 0;
        case 2:  // CoopMatrixBf16F32Acc — a LAUNCHABLE bf16(A/B)+f32(acc)
            // "Launchable", not "native silicon": CPU answers 1, Vulkan 0 (no
            // driver config), CUDA needs sm_80+, HIP gfx11xx/gfx12xx wmma.
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
            // Vulkan's init probes the extension; CUDA/HIP always have it.
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

// Synchronize the active backend (stream.sync); CPU is already synchronous.
static void cajeta_xpu_sync_active(void) {
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA: g_xpu_cuda.cuCtxSynchronize();
                           caj_cuda_bracket_drain();        break;
        case CAJ_XPU_HIP:  g_xpu_hip.hipDeviceSynchronize(); break;
        case CAJ_XPU_VULKAN: cajeta_xpu_vk_flush();          break;
        default: break;
    }
}

// CPU launch thunk: coord = [tid.xyz, ctaid.xyz, ntid.xyz, ...]; argv is shared.
typedef void (*cajeta_cpu_launch_fn)(void** argv, const int32_t* coord);

// One worker's slice: linear block indices [bStart, bEnd) of the block grid.
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

// Host spec-constant overrides are READ at runtime, never recompiled. Thread-
// local: each worker publishes its own copy before the per-block wrapper runs.
static __thread int32_t g_cpu_spec_count = 0;
static __thread const int32_t* g_cpu_spec_values = NULL;

// Read user spec slot `slot`: the host override when supplied, else the
// kernel's compile-time `def`. The f32 form reinterprets the raw word.
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

// Run a contiguous slice of blocks. The thunk is the per-BLOCK wrapper (it loops
// the work-items), so it runs once per block; the grid is linearized x-fastest.
static void cajeta_xpu_cpu_run_slice(const struct cajeta_cpu_grid_slice* s) {
    // Set every call, so a launch with no override still reads defaults.
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

// The parallel cutover. A pooled dispatch is a broadcast + barrier, not a thread
// spawn, so it pays off well below the old figure. Overridable in the env.
#ifndef CAJETA_XPU_CPU_PARALLEL_THRESHOLD
#define CAJETA_XPU_CPU_PARALLEL_THRESHOLD 256   /* work-items */
#endif
#define CAJETA_XPU_CPU_MAX_WORKERS 256

// Spin budgets before a futex sleep. JOIN spin is caller-side and safe; WORKER
// spin trips a latent barrier-fission race, so it defaults to 0 (condvar).
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
// Spawning pthreads per launch costs ~5-15us/thread, which dominates small,
// frequent kernels. This pool creates its workers ONCE and reuses them.
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
    // These pthread primitives MUST carry their static initializers explicitly:
    // on winpthreads the initializers are -1 sentinels, and a ZEROED mutex or
    // condvar is invalid — the first CPU launch on Windows hung forever.
    .mu   = PTHREAD_MUTEX_INITIALIZER,
    .go   = PTHREAD_COND_INITIALIZER,
    .done = PTHREAD_COND_INITIALIZER,
};

// Effective spin budgets, resolved once from the environment before any spin.
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
// Settable in-process as well as from the env: a measurement must alternate.
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

// Wait for the generation to advance past `seen`: spin (ACQUIRE pairs with
// dispatch's RELEASE store), then cond_wait under mu. 0 means shutdown.
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

// A pool worker runs program work: every return path unregisters it.
static void* caj_kpool_worker_body(void* arg);

static void* caj_kpool_worker_main(void* arg) {
    __cajeta_prof_thread_register();
    void* r = caj_kpool_worker_body(arg);
    __cajeta_prof_thread_unregister();
    return r;
}

static void* caj_kpool_worker_body(void* arg) {
    long myid = (long) (intptr_t) arg;
    // Baseline below the first dispatchable generation: a dispatch that races
    // ahead of the first wait must still register (g != seen), or join hangs.
    uint64_t seen = 0;
    for (;;) {
        uint64_t g = caj_kpool_wait_gen(seen);
        if (g == 0) break;                       // shutdown
        seen = g;
        if (myid < g_caj_kpool.njobs) {
            cajeta_xpu_cpu_run_slice(&g_caj_kpool.slices[myid]);
            // ACQ_REL so the joiner that reads active==0 sees our slice's stores.
            if (__atomic_sub_fetch(&g_caj_kpool.active, 1, __ATOMIC_ACQ_REL) == 0) {
                // Take mu so the signal cannot slip past the joiner's check.
                pthread_mutex_lock(&g_caj_kpool.mu);
                pthread_cond_signal(&g_caj_kpool.done);
                pthread_mutex_unlock(&g_caj_kpool.mu);
            }
        }
    }
    return NULL;
}

// How many pool workers exist right now — a diagnostic for tests.
int32_t __cajeta_xpu_cpu_pool_threads(void) {
    return (int32_t) g_caj_kpool.nthreads;
}

// Lazily create `cap-1` persistent workers (the caller runs one slice itself);
// grows when a later launch wants more, never shrinks. Must NOT hold mu.
static void caj_kpool_ensure(int cap) {
    int want = cap - 1;                       // caller runs one slice itself
    if (want < 0) want = 0;
    if (want > CAJETA_XPU_CPU_MAX_WORKERS) want = CAJETA_XPU_CPU_MAX_WORKERS;
    caj_kpool_resolve_spin();
    pthread_mutex_lock(&g_caj_kpool.mu);
    // mu/go/done carry PTHREAD_*_INITIALIZER from the designated initializer
    // above; do NOT pthread_mutex_init(&mu) here — we hold it.
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

// Fork-join dispatch: hand slices[0..njobs-1] to pool workers, then return.
static void caj_kpool_dispatch(const struct cajeta_cpu_grid_slice* slices,
                               int njobs) {
    // Publish the job payload, then RELEASE-store generation: a worker that
    // ACQUIRE-sees the new generation is guaranteed to see slices/njobs/active.
    g_caj_kpool.slices = slices;
    g_caj_kpool.njobs  = njobs;
    __atomic_store_n(&g_caj_kpool.active, njobs, __ATOMIC_RELAXED);
    // Hold mu across the bump + broadcast so a sleeping worker cannot miss it.
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

// Join and dismantle the pool (the teardown hook). Workers park on a condvar in
// MODULE memory; parked past unload, it corrupts the futex. Rebuilds lazily.
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
    // dispatch bookkeeping (generation included) races nothing.
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

// CPU launch: resolve the registered thunk and run the grid, chunked across
// min(blocks, cores) workers — blocks fan out, never one block's work-items.
static void cajeta_xpu_launch_cpu(const char* name,
                                  int32_t gridX, int32_t gridY, int32_t gridZ,
                                  int32_t blockX, int32_t blockY, int32_t blockZ,
                                  int32_t sharedBytes, void* argv,
                                  int32_t specCount, const int32_t* specValues) {
    void* p = __cajeta_xpu_lookup_cpu_kernel(name);
    if (!p) {
        // Counted like every dispatch that did not run (Device.launchFailures()).
        cajeta_xpu_note_launch_failure();
        fprintf(stderr, "cajeta.xpu: no registered CPU kernel '%s' to launch\n",
                name);
        return;
    }
    cajeta_cpu_launch_fn fn = (cajeta_cpu_launch_fn) p;
    if (gridX < 1) gridX = 1; if (gridY < 1) gridY = 1; if (gridZ < 1) gridZ = 1;
    // sharedBytes becomes a per-block alloca on the worker's stack; bound it so
    // an absurd value cannot blow the stack.
    if (sharedBytes < 0 || (uint32_t) sharedBytes > (16u << 20)) {
        fprintf(stderr, "cajeta.xpu: CPU launch sharedBytes %d out of range "
                "(max 16 MiB); not launching '%s'\n", sharedBytes, name);
        return;
    }


    // CAJETA_XPU_CPU_SERIAL forces single-threaded execution. Read once.
    static int force_serial = -1;
    if (force_serial < 0) force_serial = getenv("CAJETA_XPU_CPU_SERIAL") ? 1 : 0;

    // 64-bit: gridX*gridY*gridZ wraps in int32 (a silent no-op, or 0 ->
    // divide-by-zero in run_slice), so clamp an absurd grid with a diagnostic.
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
    // sysconf(_SC_NPROCESSORS_ONLN) is POSIX; ask the Win32 API here.
    SYSTEM_INFO cpu_si;
    GetSystemInfo(&cpu_si);
    long cores = (long) cpu_si.dwNumberOfProcessors;
#else
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (cores < 1) cores = 1;
    int32_t nworkers = (int32_t) ((long) nblocks < cores ? (long) nblocks : cores);
    if (nworkers > CAJETA_XPU_CPU_MAX_WORKERS) nworkers = CAJETA_XPU_CPU_MAX_WORKERS;
    int wcap = caj_resolve_worker_cap();
    if (wcap > 0 && nworkers > (int32_t) wcap) nworkers = (int32_t) wcap;

    if (force_serial || nblocks <= 1 || nworkers <= 1 ||
        total < CAJETA_XPU_CPU_PARALLEL_THRESHOLD) {
        // Record 1, not nworkers: one thread ran the whole grid.
        __atomic_store_n(&caj_last_workers, 1, __ATOMIC_RELEASE);
        struct cajeta_cpu_grid_slice all = {fn, (void**) argv,
                                            blockX, blockY, blockZ,
                                            gridX, gridY, gridZ,
                                            0, nblocks, sharedBytes,
                                            specCount, specValues};
        cajeta_xpu_cpu_run_slice(&all);
        return;
    }

    // Chunk the blocks across nworkers; the caller runs the last slice itself.
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
    // If the pool could not spawn enough workers, the surplus slices run inline
    // after the caller's own — correctness over parallelism.
    int njobs = nworkers - 1;
    if (njobs > g_caj_kpool.nthreads) njobs = g_caj_kpool.nthreads;
    caj_kpool_dispatch(slices, njobs);
    cajeta_xpu_cpu_run_slice(&slices[nworkers - 1]);
    for (int32_t i = njobs; i < nworkers - 1; ++i)
        cajeta_xpu_cpu_run_slice(&slices[i]);   // pool-short surplus, inline
    caj_kpool_join();
}

// --- Buffer<T> device memory (backend-dispatched) ---------------------------
// Buffer<T>'s stdlib methods forward byte-sized primitives here: the int64
// handle is the backend's device pointer, buffer-table index, or host block.
// Buffer MemoryKind ordinals — the stable native contract; MUST match
// runtime/src/cajeta/xpu/core/MemoryKind.cajeta.
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
            // Host-visible + coherent already; handle = buffer-table index.
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
// Direct host access to a host-accessible buffer (Pinned/Unified, CPU, Vulkan
// map): a plain memcpy. dir != 0 stores host[]->buffer, 0 loads back.
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
            // Unified/pinned handles are host-accessible; Device memory is not.
            if (kind == CAJ_MEMKIND_UNIFIED || kind == CAJ_MEMKIND_PINNED)
                hp = (void*) (intptr_t) handle;
            break;
        case CAJ_XPU_VULKAN:
            cajeta_xpu_vk_note_host_access(handle);   // order vs the open batch
            if (dir) {
                hp = cajeta_xpu_vk_mapped(handle);   // WC writes stream fine
            } else {
                // Reads through a write-combined mapping crawl: use staging.
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
// Async host<->device copies on a stream (0 = default), complete by that
// stream's next sync. CPU and the Vulkan coherent map copy synchronously.
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
// Stream create/sync/destroy, here rather than above because they switch on the
// backend enum. Handle 0 = the default stream.
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
// Cross-stream + host synchronisation: the handle IS the backend event object
// (0 = unavailable); CPU/Vulkan use the always-signaled sentinel 1.
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
            // The sentinel promises "already signaled": land the open batch.
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

// Stream.waitFor(event): a device-side wait so later launches on `stream` start
// only after `event` signals. Synchronous backends need none.
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
// Host-observable signal, backed by the same backend event object as Event:
// signal() records at the stream's tail; waitHost()/query() block/poll it.
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
// Synchronous host->device copy of a cajeta array's bytes (header skipped).
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
// Synchronous device->host copy into a cajeta array's bytes.
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
// Free a buffer handle that was allocated with MemoryKind `kind`.
void __cajeta_xpu_buffer_free(void* self, int64_t handle, int32_t kind) {
    (void) self;
    if (!handle) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            // Pinned frees with cuMemFreeHost; device + managed with cuMemFree.
            if (kind == CAJ_MEMKIND_PINNED && g_xpu_cuda.cuMemFreeHost)
                g_xpu_cuda.cuMemFreeHost((void*) (intptr_t) handle);
            else
                g_xpu_cuda.cuMemFree((cajeta_cudeviceptr) handle);
            return;
        case CAJ_XPU_HIP:
            // Pinned frees with hipHostFree; device + managed with hipFree.
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
// Buffer.slice: a sub-range base from a parent handle + byte offset. Pointer
// backends fold it in, Vulkan takes a view slot; the handle is non-owning.
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

// Release a slice view's backend record: a no-op on pointer backends, clears
// the borrowing view slot on Vulkan. Never called for an owning handle.
void __cajeta_xpu_buffer_slice_release(void* self, int64_t handle) {
    (void) self;
    if (!handle) return;
    if (cajeta_xpu_active_backend() == CAJ_XPU_VULKAN)
        cajeta_xpu_vk_view_release(handle);
}

// --- HIP texture helpers (Item 8 Stage C) -----------------------------------
