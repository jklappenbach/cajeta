#ifndef CAJETA_XPU_ABI_H
#define CAJETA_XPU_ABI_H

/* Cajeta XPU — stable C ABI for kernel registration + launch.
 *
 * This header is the SINGLE SOURCE OF TRUTH for the compute FFI contract:
 * the compiler emit side (src/cajeta/xpu/...), the C runtime
 * (runtime/native/cajeta_runtime.c), and any external port (numerics /
 * PyTorch / Caramelo-SPELA) all agree through the declarations here. Before
 * this header existed, the per-parameter kind values were hand-synced
 * between KernelLowering.h and the runtime's CAJETA_KP_* literals; the enum
 * below makes those numbers exist exactly once.
 *
 * The frozen contract — argv layout per parameter kind, the byteSize
 * semantics, the launch + register entry points, the device-targeting and
 * buffer-affinity rules, and which symbols are stable vs internal — is
 * documented in docs/gpu/xpu/CajetaXPU-FFI.md. Includable from both C and
 * C++ (C-linkage).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABI version. Bumped when the contract changes in a non-backward-compatible
 * way. A launch field is added as a new symbol suffix
 * (__cajeta_xpu_launch_v2, _v3, …), never by repurposing an existing
 * argument; the parameter-kind enum is append-only (never renumbered). An
 * external caller checks __cajeta_xpu_abi_version() against this macro before
 * dispatching. */
#define CAJETA_XPU_ABI_VERSION 3

/* Per-kernel-parameter kind, in declaration order. The launch site packs one
 * argv slot per parameter; this kind tells the runtime how to read that slot
 * (and the Vulkan rung how to bind it). The numeric values are part of the
 * frozen ABI — append new kinds at the end, never renumber existing ones. */
typedef enum CajetaXpuParamKind {
    CAJETA_XPU_KP_SCALAR       = 0, /* by-value primitive/POD; byteSize bytes  */
    CAJETA_XPU_KP_BUFFER       = 1, /* Buffer<T> device handle (int64)         */
    CAJETA_XPU_KP_TEXTURE      = 2, /* Texture{1D,2D,3D,Cube,2DArray} handle   */
    CAJETA_XPU_KP_SAMPLER      = 3, /* Sampler POD {i32 filter, i32 address}   */
    CAJETA_XPU_KP_ACCEL        = 4, /* AccelerationStructure POD               */
    CAJETA_XPU_KP_IMAGE        = 5, /* Image2D storage-image handle (int64)    */
    CAJETA_XPU_KP_BUFFER_ARRAY = 6  /* Buffer<T>[] bindless: slot = [count,h0…]*/
} CajetaXpuParamKind;

/* Returns CAJETA_XPU_ABI_VERSION as compiled into the runtime — the
 * header/runtime handshake an external caller checks before dispatch. */
int32_t __cajeta_xpu_abi_version(void);

/* Launch a registered kernel (ABI v3). `argv` points to an array of one
 * pointer per kernel parameter, in declaration order, each marshalled per its
 * CajetaXpuParamKind (the runtime reads the per-kernel param metadata recorded
 * by __cajeta_xpu_register_kernel_params). `streamHandle` orders the launch on
 * a stream (0 = default). `deviceId` selects the target device: -1 = the
 * current active device; >= 0 = an index into the active backend's enumerated
 * devices (handles originate from cajeta-gpu enumeration).
 *
 * `specValues` supplies host overrides for the kernel's user specialization
 * constants (Spec.geti/getf): entry `i` overrides spec slot `i` (Vulkan SpecId
 * kFirstUserSpecId+i); `specCount` is how many leading slots are supplied
 * (trailing/unset slots keep their compile-time default). Each value is a raw
 * 4-byte word (i32 today; f32 reinterpreted later — no ABI change). NULL /
 * specCount 0 = no override (every slot reads its default; identical to v2).
 * Honored as a genuine pipeline-time spec constant on Vulkan and baked into the
 * per-value variant on CPU; AMD/NVPTX fold it into the device compile.
 * A future field is added as __cajeta_xpu_launch_v4 — never by repurposing. */
void __cajeta_xpu_launch_v3(const char* kernelName,
                            int32_t gridX, int32_t gridY, int32_t gridZ,
                            int32_t blockX, int32_t blockY, int32_t blockZ,
                            uint32_t sharedBytes, void* argv,
                            int64_t streamHandle, int32_t deviceId,
                            int32_t specCount, const int32_t* specValues);

/* Compat shim (ABI v1): equivalent to v2 with deviceId = -1. Frozen. */
void __cajeta_xpu_launch_v2(const char* kernelName,
                            int32_t gridX, int32_t gridY, int32_t gridZ,
                            int32_t blockX, int32_t blockY, int32_t blockZ,
                            uint32_t sharedBytes, void* argv,
                            int64_t streamHandle, int32_t deviceId);

/* Compat shim retained for the compiler emit path: equivalent to
 * __cajeta_xpu_launch_v2(..., deviceId = -1). Frozen signature. */
void __cajeta_xpu_launch(const char* kernelName,
                         int32_t gridX, int32_t gridY, int32_t gridZ,
                         int32_t blockX, int32_t blockY, int32_t blockZ,
                         uint32_t sharedBytes, void* argv, int64_t streamHandle);

/* --- device profile (xpu-device-profile) --------------------------------- *
 * Raw device facts a host consumer turns into a DeviceModel. `archName` is the
 * robust gfx-token scan; numeric fields come from ABI-stable
 * hipDeviceGetAttribute and are sanity-clamped, so a wrong attribute ordinal on
 * a different runtime fails safe (the field stays 0, `valid` still set off the
 * arch token). Nothing is persisted; the profile is per-process, in memory. */
typedef struct CajetaXpuRawDevice {
    char     archName[64];          /* gfx token / cuda name; "" if unknown     */
    uint32_t waveSize;              /* warpSize        (0 = unavailable)        */
    uint32_t maxThreadsPerBlock;    /*                 (0 = unavailable)        */
    uint32_t multiprocessorCount;   /* RDNA: WGPs = physical CUs/2 (0 = n/a)    */
    uint32_t regsPerMP;             /* MaxRegistersPerMultiprocessor (occupancy)*/
    uint32_t threadsPerMP;          /* MaxThreadsPerMultiProcessor   (occupancy)*/
    uint32_t ldsBytesPerMP;         /* MaxSharedMemoryPerMultiprocessor         */
    int32_t  valid;                 /* 1 iff a real device arch was read        */
    /* ABI v3 — Tier-B geometry (specs/device-geometry-parameterization-spec.md
     * §2.2). APPENDED, never interleaved: an older consumer compiled against v2
     * reads the prefix unchanged. Every field is 0 when this runtime did not
     * report it; 0 means UNKNOWN and no consumer may substitute the other
     * vendor's value for it. */
    uint32_t ldsBytesPerBlock;      /* per-BLOCK shared cap   (CUDA attr 8)     */
    uint32_t ldsBytesPerBlockOptin; /* raised cap, opt-in     (CUDA attr 97)    */
    uint32_t maxBlocksPerMP;        /* resident block cap     (CUDA attr 106)   */
    uint32_t l2CacheBytes;          /* L2 size                (CUDA attr 38)    */
    uint32_t memoryClockKHz;        /* memory clock, kHz      (CUDA attr 36)    */
    uint32_t memoryBusWidthBits;    /* bus width, bits        (CUDA attr 37)    */
    uint32_t clockRateKHz;          /* core clock, kHz        (CUDA attr 13)    */
    uint32_t maxGridDimX;           /* grid clamp             (CUDA attr 5)     */
    uint32_t maxBlockDimX;          /* block clamp            (CUDA attr 2)     */
    uint64_t totalGlobalMemBytes;   /* cuDeviceTotalMem                         */
    int32_t  integrated;            /* 1 = APU                (CUDA attr 18)    */
} CajetaXpuRawDevice;

/* Scheduler partitions per multiprocessor — the device half of the dispatch
 * law (specs/device-geometry-parameterization-spec.md §3 L1). This is an ARCH
 * constant, NOT a driver attribute: no runtime reports it, and deriving it
 * from threadsPerMP/waveSize would give 1.5 on Ada, which is a residency cap
 * and not a count of schedulers.
 *
 * An RDNA WGP (what the HIP driver calls a multiprocessor) is 2 CUs x 4 SIMD32
 * = 8; an NVIDIA SM has 4 partitions and has since Volta. 0 = unknown, and a
 * caller must not substitute either value for the other.
 *
 * It lives HERE, inline in the shared ABI header, because both the C runtime
 * (serving cajeta.xpu.Device) and the host-side DeviceProfile need it and they
 * are separate binaries — two copies of a two-line rule is exactly how the
 * gfx1151 constants got everywhere in the first place. */
static inline uint32_t cajeta_xpu_simds_per_mp(const char* archName) {
    if (!archName) return 0;
    if (archName[0] == 's' && archName[1] == 'm' && archName[2] == '_') return 4;
    if (archName[0] == 'g' && archName[1] == 'f' && archName[2] == 'x') return 8;
    return 0;
}

/* Keyed accessor for cajeta.xpu.Device's geometry surface. One symbol with an
 * APPEND-ONLY key enum rather than a symbol per fact, the same discipline the
 * parameter-kind enum above follows. Returns 0 for an unknown key, an
 * unqueryable device, or a fact this runtime did not report — 0 is UNKNOWN
 * throughout and no caller may read it as a budget. */
typedef enum CajetaXpuGeometryKey {
    CAJETA_XPU_GEO_MP_COUNT               = 0,
    CAJETA_XPU_GEO_SIMDS_PER_MP           = 1,
    CAJETA_XPU_GEO_WAVE_SIZE              = 2,
    CAJETA_XPU_GEO_MAX_THREADS_PER_BLOCK  = 3,
    CAJETA_XPU_GEO_LDS_BYTES_PER_BLOCK    = 4,
    CAJETA_XPU_GEO_LDS_BYTES_PER_BLOCK_OPTIN = 5,
    CAJETA_XPU_GEO_LDS_BYTES_PER_MP       = 6,
    CAJETA_XPU_GEO_MAX_BLOCKS_PER_MP      = 7,
    CAJETA_XPU_GEO_L2_CACHE_BYTES         = 8,
    CAJETA_XPU_GEO_TOTAL_VRAM_BYTES       = 9,
    CAJETA_XPU_GEO_INTEGRATED             = 10,
    CAJETA_XPU_GEO_REGS_PER_MP            = 11,
    CAJETA_XPU_GEO_THREADS_PER_MP         = 12,
    CAJETA_XPU_GEO_MAX_GRID_DIM_X         = 13,
    CAJETA_XPU_GEO_MAX_BLOCK_DIM_X        = 14
} CajetaXpuGeometryKey;

int64_t __cajeta_xpu_device_geometry(int32_t key);

/* Query the active device into *out (zeroed first). Returns 1 on success; 0 if
 * no GPU or profiling is disabled (CAJETA_XPU_DEVICE_PROFILE_DISABLE) — then
 * out->valid == 0 and the consumer falls back to estimated defaults. */
int32_t cajeta_xpu_query_raw_device(CajetaXpuRawDevice* out);

/* Measure device memory bandwidth (GB/s) via a device-to-device copy of `bytes`
 * (read + write = 2*bytes of traffic), best of `passes`. Returns 0.0 on failure,
 * no GPU, or profiling disabled. Nothing is persisted. */
double cajeta_xpu_measure_bandwidth_gbps(uint64_t bytes, int32_t passes);

#ifdef __cplusplus
}
#endif

#endif /* CAJETA_XPU_ABI_H */
