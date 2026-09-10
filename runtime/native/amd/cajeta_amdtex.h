// Thin C-ABI wrapper over AMD's addrlib — pure host-side address math giving the
// tiled surface layout and any texel's swizzled offset. Built as the OPTIONAL
// libcajeta_amdtex.so and dlopen'd, so its absence degrades to "unsupported".
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

// AddrCreate's required triple for `gcnArchName` (a trailing ":xnack..." suffix is
// tolerated). GB_ADDR_CONFIG is exposed by no public HIP/HSA query, so it is a
// per-architecture constant here. 0 and fills the outputs; non-zero if unknown.
int cajeta_amdtex_query_gfx_config(const char* gcnArchName, uint32_t* family,
                                   uint32_t* rev, uint32_t* gbAddrConfig);

// An opaque ADDR_HANDLE from a query_gfx_config triple; NULL on failure.
void* cajeta_amdtex_create(uint32_t family, uint32_t rev, uint32_t gbAddrConfig);
void  cajeta_amdtex_destroy(void* handle);

// Tiled mip layout of a width x height, `levels`-level, `bpp`-bit 2-D surface.
int cajeta_amdtex_mip_layout(void* handle, uint32_t width, uint32_t height,
                             uint32_t levels, uint32_t bpp,
                             caj_amdtex_layout* out);

// Swizzled byte offset of texel (x,y) at mip `level`. Takes the same surface
// parameters as mip_layout plus its resolved swMode/pitch; UINT64_MAX on error.
uint64_t cajeta_amdtex_addr_from_coord(void* handle, uint32_t width,
                                       uint32_t height, uint32_t levels,
                                       uint32_t bpp, uint32_t swMode,
                                       uint32_t pitch, uint32_t level,
                                       uint32_t x, uint32_t y);

#ifdef __cplusplus
}
#endif

#endif  // CAJETA_AMDTEX_H
