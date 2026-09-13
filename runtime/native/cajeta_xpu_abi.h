#ifndef CAJETA_XPU_ABI_H
#define CAJETA_XPU_ABI_H

/* Cajeta XPU — the stable C ABI for kernel registration and launch, shared by
 * the compiler emit side, the C runtime and every external port. The frozen
 * contract is documented in docs/gpu/xpu/CajetaXPU-FFI.md. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABI version: a new launch field arrives as a new symbol suffix, never by
 * repurposing an argument. */
#define CAJETA_XPU_ABI_VERSION 3

/* Per-kernel-parameter kind: one argv slot per parameter, in declaration order.
 * The values are frozen ABI — append at the end, never renumber. */
typedef enum CajetaXpuParamKind {
    CAJETA_XPU_KP_SCALAR       = 0, /* by-value primitive/POD; byteSize bytes  */
    CAJETA_XPU_KP_BUFFER       = 1, /* Buffer<T> device handle (int64)         */
    CAJETA_XPU_KP_TEXTURE      = 2, /* Texture{1D,2D,3D,Cube,2DArray} handle   */
    CAJETA_XPU_KP_SAMPLER      = 3, /* Sampler POD {i32 filter, i32 address}   */
    CAJETA_XPU_KP_ACCEL        = 4, /* AccelerationStructure POD               */
    CAJETA_XPU_KP_IMAGE        = 5, /* Image2D storage-image handle (int64)    */
    CAJETA_XPU_KP_BUFFER_ARRAY = 6  /* Buffer<T>[] bindless: slot = [count,h0…]*/
} CajetaXpuParamKind;

/* CAJETA_XPU_ABI_VERSION as compiled in — checked before an external dispatch. */
int32_t __cajeta_xpu_abi_version(void);

/* Launches a registered kernel: `argv` holds one pointer per parameter in
 * declaration order, marshalled per its kind; `streamHandle` 0 and `deviceId` -1
 * mean default/active; specValues[0..specCount) override leading spec slots. */
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

/* Compat shim for the compiler emit path: launch_v2 with deviceId = -1. */
void __cajeta_xpu_launch(const char* kernelName,
                         int32_t gridX, int32_t gridY, int32_t gridZ,
                         int32_t blockX, int32_t blockY, int32_t blockZ,
                         uint32_t sharedBytes, void* argv, int64_t streamHandle);

/* --- device profile (xpu-device-profile) --------------------------------- *
 * Raw, per-process device facts a host consumer turns into a DeviceModel.
 * Numeric fields are sanity-clamped and 0 means UNKNOWN throughout. */
typedef struct CajetaXpuRawDevice {
    char     archName[64];          /* gfx token / cuda name; "" if unknown     */
    uint32_t waveSize;              /* warpSize        (0 = unavailable)        */
    uint32_t maxThreadsPerBlock;    /*                 (0 = unavailable)        */
    uint32_t multiprocessorCount;   /* RDNA: WGPs = physical CUs/2 (0 = n/a)    */
    uint32_t regsPerMP;             /* MaxRegistersPerMultiprocessor (occupancy)*/
    uint32_t threadsPerMP;          /* MaxThreadsPerMultiProcessor   (occupancy)*/
    uint32_t ldsBytesPerMP;         /* MaxSharedMemoryPerMultiprocessor         */
    int32_t  valid;                 /* 1 iff a real device arch was read        */
    /* ABI v3 Tier-B geometry, APPENDED and never interleaved: a consumer built
     * against v2 reads the prefix above unchanged. */
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
    /* MEASURED scheduler partitions per multiprocessor, APPENDED. Only a
     * backend that can read the real number fills it (Vulkan, from
     * VK_AMD_shader_core_properties); 0 means the query could not answer and
     * cajeta_xpu_simds_per_mp's arch-name constant stands. */
    uint32_t simdsPerMP;
} CajetaXpuRawDevice;

/* Scheduler partitions per multiprocessor: an ARCH constant, not a driver
 * attribute and not derivable from threadsPerMP/waveSize.
 *
 * An RDNA compute unit has TWO SIMD32 and a WGP pairs two CUs, so a WGP --
 * which is what HIP counts as a multiprocessor -- has 4. VK_AMD_shader_core_
 * properties reports simdPerComputeUnit = 2 on gfx1151, and 196608 registers
 * over 4 SIMDs is 1536 per lane, which is RDNA3's 192 KB register file per
 * SIMD; both confirm 4. A GCN CU has 4 SIMD16 and IS the multiprocessor, so
 * it is 4 as well, and an NVIDIA SM has 4 scheduler partitions. 0 unknown.
 *
 * This was 8 for gfx until 2026-09-13, which double-counted RDNA's SIMDs to
 * make the dispatch law reproduce a measured block target. That target is
 * real; it is now carried by DeviceModel::wavesPerSimdTarget, where it reads
 * as the empirical oversubscription it is rather than as a hardware fact. */
static inline uint32_t cajeta_xpu_simds_per_mp(const char* archName) {
    if (!archName) return 0;
    if (archName[0] == 's' && archName[1] == 'm' && archName[2] == '_') return 4;
    if (archName[0] == 'g' && archName[1] == 'f' && archName[2] == 'x') return 4;
    return 0;
}

/* The same fact, preferring what the device REPORTED over what its name
 * implies: an arch token is a proxy the moment a part ships that the table has
 * never seen, and a Vulkan device name is not a token at all. */
static inline uint32_t cajeta_xpu_simds_per_mp_of(const CajetaXpuRawDevice* d) {
    if (!d) return 0;
    if (d->simdsPerMP) return d->simdsPerMP;
    return cajeta_xpu_simds_per_mp(d->archName);
}

/* Keys for cajeta.xpu.Device's geometry surface — APPEND-ONLY, never renumbered. */
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

/* Returns 0 for an unknown key, an unqueryable device or an unreported fact. */
int64_t __cajeta_xpu_device_geometry(int32_t key);

/* Per-kernel footprint MEASURED on the loaded module, not modelled ahead of the
 * device. The compile-time manifest has to predict occupancy for a part that is
 * not present; this asks the driver what it actually allocated. Append-only. */
typedef enum CajetaXpuKernelFootprintKey {
    CAJETA_XPU_KFP_REGS_PER_THREAD  = 0,
    CAJETA_XPU_KFP_SPILL_BYTES      = 1,  /* private/scratch per work-item   */
    CAJETA_XPU_KFP_LDS_STATIC_BYTES = 2,  /* static shared per work-group    */
    CAJETA_XPU_KFP_MAX_THREADS      = 3   /* group size the code object caps */
} CajetaXpuKernelFootprintKey;

/* Returns 0 when the backend cannot answer, the kernel is not registered, or
 * its module fails to load. Loads and resolves the kernel if needed. */
int64_t __cajeta_xpu_kernel_footprint(void* nameArr, int64_t len, int32_t key);

/* Fills *out (zeroed first) from the active device. Returns 1 on success, 0 with
 * out->valid 0 when there is no GPU or profiling is disabled by env. */
int32_t cajeta_xpu_query_raw_device(CajetaXpuRawDevice* out);

/* The same, pinned to the VULKAN physical device whatever the process selected.
 * Vulkan is the backend an unknown part arrives through, so its branch is
 * addressable on its own — the geometry suite drives it directly on a box whose
 * CUDA/HIP path would otherwise win. 0 when Vulkan is unavailable. */
int32_t cajeta_xpu_query_raw_device_vulkan(CajetaXpuRawDevice* out);

/* --- kernel manifests (xpu-tile-manifest §12.1) ---------------------------- *
 * Records the per-(kernel, target) manifest JSON emitted beside the device code.
 * `json` must stay live for the process; the same (name, backend, arch) wins. */
void __cajeta_xpu_register_kernel_manifest(const char* kernelName,
                                           int32_t backend, const char* arch,
                                           const void* json, uint64_t len);

/* The active backend's arch-matched manifest JSON for `nameArr` (a cajeta int8[],
 * payload at +8, NOT NUL-terminated), as a fresh int8[] the caller owns or NULL. */
void* __cajeta_xpu_kernel_manifest_json(void* nameArr, int64_t len);

/* Device bandwidth (GB/s): a device-to-device copy of `bytes` (2*bytes of
 * traffic), best of `passes`; 0.0 on failure or no GPU. */
double cajeta_xpu_measure_bandwidth_gbps(uint64_t bytes, int32_t passes);

#ifdef __cplusplus
}
#endif

#endif /* CAJETA_XPU_ABI_H */
