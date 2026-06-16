// cajeta_amdtex implementation — see cajeta_amdtex.h. Ported verbatim from the
// proven de-risk probe (plans/gpu/xpu/probes/mipprobe.cpp): the addrlib calls and
// parameters here are exactly the ones shown to produce bit-exact on-device mip
// sampling on gfx1151.
#include "cajeta_amdtex.h"

#include <cstdlib>
#include <cstring>

#include "addrinterface.h"
#include "addrtypes.h"
using namespace rocr;

namespace {

VOID* ADDR_API cbAlloc(const ADDR_ALLOCSYSMEM_INPUT* in) {
    return std::malloc(in->sizeInBytes);
}
ADDR_E_RETURNCODE ADDR_API cbFree(const ADDR_FREESYSMEM_INPUT* in) {
    std::free(in->pVirtAddr);
    return ADDR_OK;
}

// gfx11 mip surfaces require a 64KB _X swizzle mode (4KB / non-_X modes assert in
// addrlib for multi-level 2-D). Proven in the probe.
constexpr AddrSwizzleMode kMipSwizzle = ADDR_SW_64KB_R_X;  // = 27

// CIASICIDGFXENGINE_ARCTICISLAND — the GFX-engine id AddrCreate expects for
// GFX9..GFX11 ASICs. Lives in addrlib's internal core/addrlib.h; inlined here so
// the wrapper depends only on the public addrinterface.h/addrtypes.h headers.
constexpr int kGfxEngineArcticIsland = 0x0D;

}  // namespace

extern "C" void* cajeta_amdtex_create(uint32_t family, uint32_t rev,
                                      uint32_t gbAddrConfig) {
    ADDR_CREATE_INPUT ci;
    std::memset(&ci, 0, sizeof(ci));
    ci.size = sizeof(ci);
    ci.chipEngine = kGfxEngineArcticIsland;  // 0x0D
    ci.chipFamily = family;
    ci.chipRevision = rev;
    ci.callbacks.allocSysMem = cbAlloc;
    ci.callbacks.freeSysMem = cbFree;
    ci.regValue.gbAddrConfig = gbAddrConfig;
    ADDR_CREATE_OUTPUT co;
    std::memset(&co, 0, sizeof(co));
    co.size = sizeof(co);
    if (AddrCreate(&ci, &co) != ADDR_OK || !co.hLib) return nullptr;
    return co.hLib;
}

extern "C" void cajeta_amdtex_destroy(void* handle) {
    if (handle) AddrDestroy(static_cast<ADDR_HANDLE>(handle));
}

extern "C" int cajeta_amdtex_mip_layout(void* handle, uint32_t width,
                                        uint32_t height, uint32_t levels,
                                        uint32_t bpp, caj_amdtex_layout* out) {
    if (!handle || !out || levels == 0 || levels > CAJ_AMDTEX_MAX_LEVELS)
        return 1;
    ADDR_HANDLE h = static_cast<ADDR_HANDLE>(handle);

    ADDR2_COMPUTE_SURFACE_INFO_INPUT si;
    std::memset(&si, 0, sizeof(si));
    si.size = sizeof(si);
    si.flags.texture = 1;
    si.swizzleMode = kMipSwizzle;
    si.resourceType = ADDR_RSRC_TEX_2D;
    si.format = ADDR_FMT_32;     // 32 bits/element; channel count is independent
    si.bpp = bpp;
    si.width = width;
    si.height = height;
    si.numSlices = 1;
    si.numMipLevels = levels;
    si.numSamples = 1;

    ADDR2_MIP_INFO mip[CAJ_AMDTEX_MAX_LEVELS];
    std::memset(mip, 0, sizeof(mip));
    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT so;
    std::memset(&so, 0, sizeof(so));
    so.size = sizeof(so);
    so.pMipInfo = mip;
    if (Addr2ComputeSurfaceInfo(h, &si, &so) != ADDR_OK) return 2;

    std::memset(out, 0, sizeof(*out));
    out->surfSize = so.surfSize;
    out->baseAlign = so.baseAlign;
    out->pitch = so.pitch;
    out->swMode = static_cast<uint32_t>(kMipSwizzle);
    for (uint32_t l = 0; l < levels; ++l) {
        out->levelW[l] = mip[l].pixelPitch;
        out->levelH[l] = mip[l].pixelHeight;
        out->levelOffset[l] = mip[l].offset;
    }
    return 0;
}

extern "C" uint64_t cajeta_amdtex_addr_from_coord(
    void* handle, uint32_t width, uint32_t height, uint32_t levels, uint32_t bpp,
    uint32_t swMode, uint32_t pitch, uint32_t level, uint32_t x, uint32_t y) {
    if (!handle) return UINT64_MAX;
    ADDR_HANDLE h = static_cast<ADDR_HANDLE>(handle);

    ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT ai;
    std::memset(&ai, 0, sizeof(ai));
    ai.size = sizeof(ai);
    ai.x = x;
    ai.y = y;
    ai.slice = 0;
    ai.mipId = level;
    ai.swizzleMode = static_cast<AddrSwizzleMode>(swMode);
    ai.flags.texture = 1;
    ai.resourceType = ADDR_RSRC_TEX_2D;
    ai.bpp = bpp;
    ai.unalignedWidth = width;
    ai.unalignedHeight = height;
    ai.numSlices = 1;
    ai.numMipLevels = levels;
    ai.numSamples = 1;
    ai.pitchInElement = pitch;

    ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT ao;
    std::memset(&ao, 0, sizeof(ao));
    ao.size = sizeof(ao);
    if (Addr2ComputeSurfaceAddrFromCoord(h, &ai, &ao) != ADDR_OK)
        return UINT64_MAX;
    return ao.addr;
}
