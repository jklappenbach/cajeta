// cajeta_amdtex — thin C-ABI wrapper over AMD's addrlib for the HIP mipmap/cube
// texture emulation (option B: a hand-built gfx11 image SRD over an addrlib-tiled
// hipMalloc allocation). Built as the OPTIONAL shared library libcajeta_amdtex.so
// and dlopen'd by the runtime exactly like libamdhip64 — so the heavy C++ addrlib
// stays out of the embedded JIT bitcode and the whole path degrades to
// "unsupported" when the .so (or an AMD GPU) is absent.
//
// addrlib is pure host-side address math: given the device's gfx config it
// computes the tiled surface layout (per-mip byte offsets, padded pitch, total
// size) and the swizzled byte offset of any (level,x,y) texel. The runtime uses
// that to host-tile the upload and to fill the SQ_IMG_RSRC base/pitch fields; the
// rest of the SRD (format/dst_sel/type) is cloned from a live single-level texobj
// and patched (MAX_MIP, LAST_LEVEL, SW_MODE) per the proven recipe in
// plans/gpu/xpu/probes/mipprobe.cpp.
#ifndef CAJETA_AMDTEX_H
#define CAJETA_AMDTEX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAJ_AMDTEX_MAX_LEVELS 16

// Tiled layout of a mipmapped 2-D surface, as computed by addrlib.
typedef struct caj_amdtex_layout {
    uint64_t surfSize;                          // total bytes to hipMalloc
    uint32_t baseAlign;                         // required base alignment
    uint32_t pitch;                             // padded pitch, in ELEMENTS
    uint32_t swMode;                            // swizzle mode (-> SRD SW_MODE)
    uint32_t levelW[CAJ_AMDTEX_MAX_LEVELS];     // per-level width  (texels)
    uint32_t levelH[CAJ_AMDTEX_MAX_LEVELS];     // per-level height (texels)
    uint64_t levelOffset[CAJ_AMDTEX_MAX_LEVELS];// per-level base byte offset
} caj_amdtex_layout;

// Resolve the addrlib config triple (chip family id, chip external revision,
// GB_ADDR_CONFIG) for a device, keyed by its gfx arch string (e.g. "gfx1151",
// as returned by hipGetDeviceProperties().gcnArchName — a trailing ":xnack..."
// feature suffix is tolerated). These three values are addrlib's required
// AddrCreate inputs; GB_ADDR_CONFIG in particular is not exposed by any public
// HIP/HSA query (only ROCr's internal libhsakmt tile-config thunk has it), so for
// the swizzle modes cajeta uses it is carried here as a per-architecture constant
// — the same per-GPU-table approach drivers and game engines take. Returns 0 and
// fills *family/*rev/*gbAddrConfig on a known arch; returns non-zero (mip/cube
// emulation then degrades to unsupported) on an unrecognised one.
int cajeta_amdtex_query_gfx_config(const char* gcnArchName, uint32_t* family,
                                   uint32_t* rev, uint32_t* gbAddrConfig);

// Create an addrlib handle for a gfx device. family/rev/gbAddrConfig come from
// cajeta_amdtex_query_gfx_config. Returns NULL on failure. Opaque (ADDR_HANDLE).
void* cajeta_amdtex_create(uint32_t family, uint32_t rev, uint32_t gbAddrConfig);
void  cajeta_amdtex_destroy(void* handle);

// Compute the tiled mip layout for a width x height, `levels`-level, `bpp`-bit
// 2-D surface. Returns 0 on success, non-zero on addrlib error.
int cajeta_amdtex_mip_layout(void* handle, uint32_t width, uint32_t height,
                             uint32_t levels, uint32_t bpp,
                             caj_amdtex_layout* out);

// Swizzled byte offset of texel (x,y) at mip `level`, for the surface described by
// the same parameters passed to cajeta_amdtex_mip_layout (plus the resolved
// swMode/pitch from the layout). Returns the byte offset, or UINT64_MAX on error.
uint64_t cajeta_amdtex_addr_from_coord(void* handle, uint32_t width,
                                       uint32_t height, uint32_t levels,
                                       uint32_t bpp, uint32_t swMode,
                                       uint32_t pitch, uint32_t level,
                                       uint32_t x, uint32_t y);

#ifdef __cplusplus
}
#endif

#endif  // CAJETA_AMDTEX_H
