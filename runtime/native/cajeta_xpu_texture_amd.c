// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// --- HIP texture helpers (Item 8 Stage C) -----------------------------------
// On AMD a Texture2D is a hipArray (created here) whose handle is a pointer to a
// cajeta_hip_tex record. The hipTextureObject — which carries the image AND
// sampler SRDs the kernel's __ockl_image_sample_2D reads — is built per launch
// (cajeta_xpu_launch_hip) from this array + the paired Sampler's modes.
static int cajeta_hip_tex_supported(void) {
    return g_xpu_hip.hipMallocArray && g_xpu_hip.hipMemcpy2DToArray &&
           g_xpu_hip.hipCreateTextureObject && g_xpu_hip.hipDestroyTextureObject;
}

static int64_t cajeta_xpu_hip_tex_alloc(uint32_t w, uint32_t h, int32_t format) {
    if (!cajeta_hip_tex_supported()) return 0;
    int channels = cajeta_texfmt_channels(format);
    int bits = cajeta_texfmt_is_unorm(format) ? 8                 // per-channel
             : cajeta_texfmt_is_half(format)  ? 16
                                              : 32;
    struct caj_hip_channel_format_desc cd;
    memset(&cd, 0, sizeof(cd));
    cd.x = bits;
    if (channels == 4) { cd.y = bits; cd.z = bits; cd.w = bits; }
    // UNORM stores unsigned bytes (read back normalized to [0,1] via the texobj's
    // NormalizedFloat read mode); float/half store raw floats (16- or 32-bit) read
    // element-typed (Float channel kind); raw 32-bit integer formats store
    // signed/unsigned ints, read element-typed (the texobj readMode below is
    // Element, so image_load returns the raw integer bits — bitcast on the device).
    cd.f = cajeta_texfmt_is_integer(format)
               ? (cajeta_texfmt_is_unsigned(format) ? CAJ_HIP_CHANNEL_UNSIGNED
                                                     : CAJ_HIP_CHANNEL_SIGNED)
         : cajeta_texfmt_is_unorm(format) ? CAJ_HIP_CHANNEL_UNSIGNED
                                          : CAJ_HIP_CHANNEL_FLOAT;
    void* array = NULL;
    if (g_xpu_hip.hipMallocArray(&array, &cd, w, h, 0) != 0 || !array) return 0;
    struct cajeta_hip_tex* t =
        (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) { if (g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(array); return 0; }
    memset(t, 0, sizeof(*t));   // emulated=0 + null the emulated-path pointers
    t->array = array; t->mipmap = NULL; t->w = w; t->h = h; t->d = 1;
    t->format = format; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

// --- Image2D storage images on AMD (the writable twin of the texture path) ---
// A storage image is a hipArray allocated with hipArraySurfaceLoadStore, bound
// per launch as a SURFACE object (no sampler). Optional, exactly like the
// texture path: when the symbols (or the driver) are absent the alloc returns 0
// and the feature degrades to unsupported (cf. the mipmap path on this APU).
static int cajeta_hip_surf_supported(void) {
    return g_xpu_hip.hipMallocArray && g_xpu_hip.hipFreeArray &&
           g_xpu_hip.hipCreateSurfaceObject && g_xpu_hip.hipDestroySurfaceObject &&
           g_xpu_hip.hipMemcpy2DFromArray;
}

// Allocate an R32F surface-capable hipArray (Image2D is R32F only, matching the
// Vulkan storage image). Reuses the cajeta_hip_tex record (format R32F, 1 level).
static int64_t cajeta_xpu_hip_image_alloc(uint32_t w, uint32_t h) {
    if (!cajeta_hip_surf_supported()) return 0;
    struct caj_hip_channel_format_desc cd;
    memset(&cd, 0, sizeof(cd));
    cd.x = 32;                       // single 32-bit channel
    cd.f = CAJ_HIP_CHANNEL_FLOAT;
    void* array = NULL;
    if (g_xpu_hip.hipMallocArray(&array, &cd, w, h,
                                 CAJ_HIP_ARRAY_SURFACE_LOAD_STORE) != 0 || !array)
        return 0;
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) { if (g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(array); return 0; }
    memset(t, 0, sizeof(*t));   // emulated=0 + null the emulated-path pointers
    t->array = array; t->mipmap = NULL; t->w = w; t->h = h; t->d = 1;
    t->format = CAJ_TEXFMT_R32F; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

// Read the surface array back to host (the texels the kernel wrote). Row pitch
// and copy width are in BYTES (w * sizeof(float)); height is in rows.
static void cajeta_xpu_hip_image_download(int64_t handle, void* host,
                                          uint32_t w, uint32_t h) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) handle;
    if (!t || !t->array || !host || !g_xpu_hip.hipMemcpy2DFromArray) return;
    size_t rowBytes = (size_t) w * sizeof(float);
    g_xpu_hip.hipMemcpy2DFromArray(host, rowBytes, t->array, 0, 0, rowBytes, h,
                                   CAJ_HIP_MEMCPY_DTOH);
}

static void cajeta_xpu_hip_image_free(int64_t handle) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) handle;
    if (!t) return;
    if (t->array && g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(t->array);
    free(t);
}

// Build the channel-format descriptor for a TextureFormat (shared by 2-D + 3-D).
static struct caj_hip_channel_format_desc cajeta_hip_channel_desc(int32_t format) {
    int channels = cajeta_texfmt_channels(format);
    int bits = cajeta_texfmt_is_unorm(format) ? 8
             : cajeta_texfmt_is_half(format)  ? 16 : 32;
    struct caj_hip_channel_format_desc cd;
    memset(&cd, 0, sizeof(cd));
    cd.x = bits;
    if (channels == 4) { cd.y = bits; cd.z = bits; cd.w = bits; }
    cd.f = cajeta_texfmt_is_integer(format)
               ? (cajeta_texfmt_is_unsigned(format) ? CAJ_HIP_CHANNEL_UNSIGNED
                                                     : CAJ_HIP_CHANNEL_SIGNED)
         : cajeta_texfmt_is_unorm(format) ? CAJ_HIP_CHANNEL_UNSIGNED
                                          : CAJ_HIP_CHANNEL_FLOAT;
    return cd;
}

// Texture1D on AMD: a 1-D hipArray. hipMallocArray with height 0 makes a 1-D
// array, which yields a 1-D image SRD through the same dimension-agnostic
// RES_ARRAY texobj path — so the kernel's __ockl_image_{sample,load}_1D address
// it correctly. Upload + free reuse the 2-D paths (a 1-D array is a height-1
// hipMemcpy2DToArray).
static int64_t cajeta_xpu_hip_tex1d_alloc(uint32_t w, int32_t format) {
    if (!cajeta_hip_tex_supported()) return 0;
    struct caj_hip_channel_format_desc cd = cajeta_hip_channel_desc(format);
    void* array = NULL;
    if (g_xpu_hip.hipMallocArray(&array, &cd, w, 0, 0) != 0 || !array) return 0;
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) { if (g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(array); return 0; }
    memset(t, 0, sizeof(*t));   // emulated=0 + null the emulated-path pointers
    t->array = array; t->mipmap = NULL; t->w = w; t->h = 1; t->d = 1;
    t->format = format; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

// Texture3D on AMD: a 3-D hipArray (hipMalloc3DArray) + per-launch hipTextureObject
// (dimension-agnostic). Upload via hipMemcpy3D from a linear host volume.
static int cajeta_hip_tex3d_supported(void) {
    return g_xpu_hip.hipMalloc3DArray && g_xpu_hip.hipMemcpy3D &&
           g_xpu_hip.hipCreateTextureObject && g_xpu_hip.hipDestroyTextureObject;
}

static int64_t cajeta_xpu_hip_tex3d_alloc(uint32_t w, uint32_t h, uint32_t d,
                                          int32_t format) {
    if (!cajeta_hip_tex3d_supported()) return 0;
    struct caj_hip_channel_format_desc cd = cajeta_hip_channel_desc(format);
    struct caj_hip_extent ext; ext.w = w; ext.h = h; ext.d = d;
    void* array = NULL;
    if (g_xpu_hip.hipMalloc3DArray(&array, &cd, ext, 0) != 0 || !array) return 0;
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) { if (g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(array); return 0; }
    memset(t, 0, sizeof(*t));   // emulated=0 + null the emulated-path pointers
    t->array = array; t->mipmap = NULL; t->w = w; t->h = h; t->d = d;
    t->format = format; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

// Texture2DArray on AMD: a layered hipArray (hipMalloc3DArray + hipArrayLayered),
// whose extent.depth carries the LAYER count (not a true depth). The per-launch
// hipTextureObject is dimension-agnostic (RES_ARRAY), and the upload reuses the
// 3-D memcpy3D path with d = layers (a layered memcpy3D copies all layers).
static int64_t cajeta_xpu_hip_tex2darray_alloc(uint32_t w, uint32_t h,
                                               uint32_t layers, int32_t format) {
    if (!cajeta_hip_tex3d_supported()) return 0;
    struct caj_hip_channel_format_desc cd = cajeta_hip_channel_desc(format);
    struct caj_hip_extent ext; ext.w = w; ext.h = h; ext.d = layers;
    void* array = NULL;
    if (g_xpu_hip.hipMalloc3DArray(&array, &cd, ext, CAJ_HIP_ARRAY_LAYERED) != 0 ||
        !array)
        return 0;
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) { if (g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(array); return 0; }
    memset(t, 0, sizeof(*t));   // emulated=0 + null the emulated-path pointers
    t->array = array; t->mipmap = NULL; t->w = w; t->h = h; t->d = layers;
    t->format = format; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

// TextureCube on AMD: EMULATED as a 6-LAYER LAYERED array (hipArrayCubemap is
// unsupported by the HIP runtime on gfx1151 — invalid-arg, confirmed ROCm 7.11.0 +
// 7.11.0; see reference_amd_hip_mipmap_cubemap_unsupported). A layered array IS
// supported, so we store the 6 faces as 6 layers and do the major-axis face
// projection IN-KERNEL (AmdgpuKernelLowering::sampleTextureCube) → sample the
// chosen layer via __ockl_image_sample_2Da. Same storage + upload as a 6-layer
// Texture2DArray (RES_ARRAY texobj; memcpy3D copies all 6 faces). Limitation: no
// hardware seamless filtering across face edges (each face clamps at its edge).
static int64_t cajeta_xpu_hip_texcube_alloc(uint32_t size, int32_t format) {
    if (!cajeta_hip_tex3d_supported()) return 0;
    struct caj_hip_channel_format_desc cd = cajeta_hip_channel_desc(format);
    struct caj_hip_extent ext; ext.w = size; ext.h = size; ext.d = 6;
    void* array = NULL;
    if (g_xpu_hip.hipMalloc3DArray(&array, &cd, ext, CAJ_HIP_ARRAY_LAYERED) != 0 ||
        !array)
        return 0;
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) { if (g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(array); return 0; }
    memset(t, 0, sizeof(*t));   // emulated=0 + null the emulated-path pointers
    t->array = array; t->mipmap = NULL; t->w = size; t->h = size; t->d = 6;
    t->format = format; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

static void cajeta_xpu_hip_tex3d_upload(int64_t handle, const float* src,
                                        uint32_t w, uint32_t h, uint32_t d,
                                        int32_t format) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) handle;
    if (!t || !t->array || w != t->w || h != t->h || d != t->d) return;
    size_t channels = (size_t) cajeta_texfmt_channels(format);
    size_t texelBytes = cajeta_texfmt_texel_bytes(format);
    size_t texels = (size_t) w * h * d * channels;
    // Encode into a packed temp for UNORM/half (1/2 bytes/channel); for f32 / raw
    // integer the float[] source bytes ARE the storage, so copy directly.
    void* hostBytes = (void*) src;
    void* tmp = NULL;
    if (cajeta_texfmt_is_unorm(format) || cajeta_texfmt_is_half(format)) {
        tmp = malloc(texels * cajeta_texfmt_channel_bytes(format));
        if (!tmp) return;
        cajeta_texfmt_encode(tmp, src, texels, format);
        hostBytes = tmp;
    }
    struct caj_hip_memcpy3d_parms p;
    memset(&p, 0, sizeof(p));
    p.srcPtr.ptr = hostBytes;
    p.srcPtr.pitch = (size_t) w * texelBytes;   // row pitch in bytes
    p.srcPtr.xsize = w;                          // logical width  (elements)
    p.srcPtr.ysize = h;                          // logical height (elements)
    p.dstArray = t->array;
    p.extent.w = w;                              // array-element extents
    p.extent.h = h;
    p.extent.d = d;
    p.kind = CAJ_HIP_MEMCPY_HTOD;
    g_xpu_hip.hipMemcpy3D(&p);
    if (tmp) free(tmp);
}

static void cajeta_xpu_hip_tex_upload(int64_t handle, const float* src,
                                      uint32_t w, uint32_t h, int32_t format) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) handle;
    if (!t || !t->array || w != t->w || h != t->h) return;
    size_t rowBytes = (size_t) w * cajeta_texfmt_texel_bytes(format);
    if (cajeta_texfmt_is_unorm(format) || cajeta_texfmt_is_half(format)) {
        // Storage differs from the float[] source (1 byte/sample UNORM, 2 bytes
        // half) — encode into a temp buffer, then copy the packed bytes.
        size_t texels = (size_t) w * h * cajeta_texfmt_channels(format);
        size_t bytes  = texels * cajeta_texfmt_channel_bytes(format);
        unsigned char* tmp = (unsigned char*) malloc(bytes);
        if (!tmp) return;
        cajeta_texfmt_encode(tmp, src, texels, format);
        g_xpu_hip.hipMemcpy2DToArray(t->array, 0, 0, tmp, rowBytes, rowBytes, h,
                                     CAJ_HIP_MEMCPY_HTOD);
        free(tmp);
    } else {
        g_xpu_hip.hipMemcpy2DToArray(t->array, 0, 0, src, rowBytes, rowBytes, h,
                                     CAJ_HIP_MEMCPY_HTOD);
    }
}

// Mipmapped Texture2D on AMD: a hipMipmappedArray (numLevels) whose per-level
// texels are staged by hipGetMipmappedArrayLevel → hipMemcpy2DToArray (the 2-D
// upload path per level); the per-launch hipTextureObject binds it via
// RES_MIPMAPPED_ARRAY (cajeta_xpu_hip_make_texobj).
static int cajeta_hip_tex_mip_supported(void) {
    return g_xpu_hip.hipMallocMipmappedArray &&
           g_xpu_hip.hipGetMipmappedArrayLevel && g_xpu_hip.hipMemcpy2DToArray &&
           g_xpu_hip.hipCreateTextureObject && g_xpu_hip.hipDestroyTextureObject;
}

// === Emulated mip Texture2D (option B) =======================================
// When the HIP runtime lacks mipmapped arrays (true on gfx1151, ROCm 7.11),
// a mip texture is a single plain hipMalloc tiled by addrlib, sampled through a
// HAND-BUILT gfx11 image SRD. Proven bit-exact on-device in the de-risk probe
// (plans/gpu/xpu/probes/mipprobe.cpp); the SRD field semantics come from Mesa's
// ac_descriptors.c / gfx11-rsrc.json. Requires libcajeta_amdtex + a recognised
// gfx arch; otherwise this returns 0 and the caller falls back / degrades.
#define CAJ_HIP_HOST_COHERENT 0x40000000u   // hipHostMallocCoherent

// 2 MiB base alignment: the gfx11 _X swizzle derives pipe/bank-xor bits from the
// base address; a 2 MiB-aligned base makes them zero, matching addrlib's
// pipeBankXor=0 layout (so our SRD carries no tile_swizzle). Verified in the probe.
#define CAJ_AMD_MIP_BASE_ALIGN 0x200000ull

static int cajeta_hip_mip_emulation_available(void) {
    return g_xpu_hip.hipMalloc && g_xpu_hip.hipFree && g_xpu_hip.hipMemcpyHtoD &&
           g_xpu_hip.hipHostMalloc && g_xpu_hip.hipMallocArray &&
           g_xpu_hip.hipFreeArray && g_xpu_hip.hipCreateTextureObject &&
           g_xpu_hip.hipDestroyTextureObject && cajeta_xpu_amdtex_init();
}

// Allocate + lay out an emulated mip surface. Returns a tex record (emulated=1)
// or 0 (caller falls back to the hipMallocMipmappedArray path).
static int64_t cajeta_xpu_hip_tex_alloc_mip_emulated(uint32_t w, uint32_t h,
                                                     int32_t format,
                                                     uint32_t levels) {
    if (!cajeta_hip_mip_emulation_available()) return 0;
    char arch[64];
    if (!cajeta_xpu_hip_gfx_arch(arch, sizeof(arch))) return 0;
    uint32_t family = 0, rev = 0, gbcfg = 0;
    if (g_xpu_amdtex.query_gfx_config(arch, &family, &rev, &gbcfg) != 0) return 0;
    void* addr = g_xpu_amdtex.create(family, rev, gbcfg);
    if (!addr) return 0;

    uint32_t bpp = (uint32_t) (cajeta_texfmt_texel_bytes(format) * 8);
    struct caj_amdtex_layout_c lo;
    memset(&lo, 0, sizeof(lo));
    if (g_xpu_amdtex.mip_layout(addr, w, h, levels, bpp, &lo) != 0) {
        g_xpu_amdtex.destroy(addr); return 0;
    }
    void* devAlloc = NULL;
    if (g_xpu_hip.hipMalloc(&devAlloc, lo.surfSize + CAJ_AMD_MIP_BASE_ALIGN) != 0 ||
        !devAlloc) {
        g_xpu_amdtex.destroy(addr); return 0;
    }
    uint64_t devBase = ((uint64_t) (uintptr_t) devAlloc + (CAJ_AMD_MIP_BASE_ALIGN - 1))
                       & ~(CAJ_AMD_MIP_BASE_ALIGN - 1);
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) { g_xpu_hip.hipFree(devAlloc); g_xpu_amdtex.destroy(addr); return 0; }
    memset(t, 0, sizeof(*t));
    t->array = NULL; t->mipmap = NULL; t->w = w; t->h = h; t->d = 1;
    t->format = format; t->levels = (int) levels;
    t->emulated = 1; t->devAlloc = devAlloc; t->devBase = devBase;
    t->addr = addr; t->srdBlob = NULL; t->layout = lo;
    // Persistent host copy of the whole tiled surface: each uploadLevel scatters
    // its level into this and re-pushes the lot, so earlier levels survive (the
    // levels share one tile — a fresh per-level buffer would zero the others).
    t->stagingHost = calloc(1, lo.surfSize);
    if (!t->stagingHost) {
        g_xpu_hip.hipFree(devAlloc); g_xpu_amdtex.destroy(addr); free(t); return 0;
    }
    return (int64_t) (intptr_t) t;
}

// Host-tile one level via addrlib + upload the whole (small, single-tile) surface.
// Each level's texels are scattered across the shared tile, so the full surface is
// re-staged + copied per level (levels are few and tiny — correctness over churn).
static void cajeta_xpu_hip_tex_upload_level_emulated(struct cajeta_hip_tex* t,
                                                     const float* src, uint32_t lw,
                                                     uint32_t lh, uint32_t level,
                                                     int32_t format) {
    if (!t->addr || !t->stagingHost || (int) level >= t->levels) return;
    size_t texelBytes = cajeta_texfmt_texel_bytes(format);
    size_t texels = (size_t) lw * lh * cajeta_texfmt_channels(format);
    // Encode the level into a linear (row-major) buffer first.
    unsigned char* lin = (unsigned char*) malloc((size_t) lw * lh * texelBytes);
    if (!lin) return;
    if (cajeta_texfmt_is_unorm(format) || cajeta_texfmt_is_half(format) ||
        cajeta_texfmt_is_integer(format))
        cajeta_texfmt_encode(lin, src, texels, format);
    else
        memcpy(lin, src, (size_t) lw * lh * texelBytes);
    // Scatter into the PERSISTENT staging buffer at each texel's tiled offset, then
    // re-push the whole surface (levels interleave within the shared tile).
    unsigned char* surf = (unsigned char*) t->stagingHost;
    for (uint32_t y = 0; y < lh; ++y)
        for (uint32_t x = 0; x < lw; ++x) {
            uint64_t off = g_xpu_amdtex.addr_from_coord(
                t->addr, t->w, t->h, (uint32_t) t->levels, (uint32_t) (texelBytes * 8),
                t->layout.swMode, t->layout.pitch, level, x, y);
            if (off != (uint64_t) -1 && off + texelBytes <= t->layout.surfSize)
                memcpy(surf + off, lin + ((size_t) y * lw + x) * texelBytes, texelBytes);
        }
    g_xpu_hip.hipMemcpyHtoD((void*) (uintptr_t) t->devBase, surf, t->layout.surfSize);
    free(lin);
}

// Build the per-launch texobj for an emulated mip texture: a fine-grain-SVM blob
// {imageSRD[8], pad[4], samplerSRD[4]}. Clones a live single-level texobj of the
// same base size/format (for the device's exact word2/3 format/dst_sel/type +
// sampler encoding), then patches the three mip fields proven in the probe:
//   WORD1 MAX_MIP[16:19]=levels-1, WORD3 {BASE_LEVEL=0, LAST_LEVEL=levels-1,
//   SW_MODE[20:24]}, base address (word0 + word1[7:0]); sampler WORD2
//   MIP_FILTER[26:27] + WORD1 MAX_LOD wide. Returns the blob ptr (int64) or 0.
static int64_t cajeta_xpu_hip_mip_build_srd_blob(struct cajeta_hip_tex* t,
                                                 int32_t filterMode,
                                                 int32_t addressMode) {
    // 1. Template: a 1-level base WxH array + texobj with the requested modes.
    struct caj_hip_channel_format_desc cd = cajeta_hip_channel_desc(t->format);
    void* tmplArr = NULL;
    if (g_xpu_hip.hipMallocArray(&tmplArr, &cd, t->w, t->h, 0) != 0 || !tmplArr)
        return 0;
    struct caj_hip_resource_desc rd; memset(&rd, 0, sizeof(rd));
    rd.resType = CAJ_HIP_RES_ARRAY; rd.res.array.array = tmplArr;
    struct caj_hip_texture_desc td; memset(&td, 0, sizeof(td));
    int hipAddr = addressMode == 1 ? CAJ_HIP_ADDR_WRAP : CAJ_HIP_ADDR_CLAMP;
    td.addressMode[0] = hipAddr; td.addressMode[1] = hipAddr; td.addressMode[2] = hipAddr;
    int hipFilter = filterMode == 1 ? CAJ_HIP_FILTER_LINEAR : CAJ_HIP_FILTER_POINT;
    td.filterMode = hipFilter;
    td.readMode = cajeta_texfmt_is_unorm(t->format) ? CAJ_HIP_READ_NORMALIZED_FLOAT
                                                    : CAJ_HIP_READ_ELEMENT;
    td.normalizedCoords = 1;
    void* tmplObj = NULL;
    if (g_xpu_hip.hipCreateTextureObject(&tmplObj, &rd, &td, NULL) != 0 || !tmplObj) {
        if (g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(tmplArr);
        return 0;
    }
    // __hip_texture is fine-grain SVM: read the 16-dword {img[8],pad,samp[4]} blob.
    uint32_t T[16]; memcpy(T, (const void*) tmplObj, sizeof(T));
    g_xpu_hip.hipDestroyTextureObject(tmplObj);
    if (g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(tmplArr);

    // 2. Patch into the mip SRD (recipe: plans/gpu/xpu/probes/mipprobe.cpp).
    uint32_t srd[16]; memset(srd, 0, sizeof(srd));
    for (int j = 0; j < 8; ++j) srd[j] = T[j];      // image SRD
    for (int j = 12; j < 16; ++j) srd[j] = T[j];    // sampler SRD
    uint32_t last = (uint32_t) (t->levels - 1);
    uint64_t a8 = t->devBase >> 8;
    srd[0] = (uint32_t) (a8 & 0xFFFFFFFFu);
    srd[1] = (T[1] & ~0xFFu) | (uint32_t) ((a8 >> 32) & 0xFFu);       // BASE_ADDR_HI
    srd[1] = (srd[1] & ~(0xFu << 16)) | (last << 16);                 // MAX_MIP
    srd[3] = (srd[3] & ~(0xFu << 12));                                // BASE_LEVEL = 0
    srd[3] = (srd[3] & ~(0xFu << 16)) | (last << 16);                 // LAST_LEVEL
    srd[3] = (srd[3] & ~(0x1Fu << 20)) | ((t->layout.swMode & 0x1Fu) << 20);  // SW_MODE
    // Sampler: open MAX_LOD + enable mip filtering (a single-level template leaves
    // MIP_FILTER=none, which would pin every sample to the base level).
    srd[13] = (srd[13] & ~0xFFFu);                                    // MIN_LOD = 0
    srd[13] = (srd[13] & ~(0xFFFu << 12)) | (0xFFFu << 12);           // MAX_LOD = max
    uint32_t mipFilter = filterMode == 1 ? 2u : 1u;  // linear(trilinear) vs point
    srd[14] = (srd[14] & ~(0x3u << 26)) | (mipFilter << 26);          // MIP_FILTER

    // 3. Place the blob in fine-grain SVM (device reads it as the texobj kernarg).
    if (!t->srdBlob &&
        g_xpu_hip.hipHostMalloc(&t->srdBlob, sizeof(srd), CAJ_HIP_HOST_COHERENT) != 0)
        t->srdBlob = NULL;
    if (!t->srdBlob) return 0;
    memcpy(t->srdBlob, srd, sizeof(srd));
    return (int64_t) (intptr_t) t->srdBlob;
}

static int64_t cajeta_xpu_hip_tex_alloc_mip(uint32_t w, uint32_t h, int32_t format,
                                            uint32_t levels) {
    // Prefer the emulated path (hand-built SRD over an addrlib-tiled hipMalloc):
    // it works on hardware whose HIP runtime lacks mipmapped arrays (gfx1151), and
    // is proven bit-exact on-device. Falls through to the native hipMipmappedArray
    // path when emulation is unavailable (no helper .so / unknown arch / non-AMD).
    int64_t emu = cajeta_xpu_hip_tex_alloc_mip_emulated(w, h, format, levels);
    if (emu) return emu;
    if (!cajeta_hip_tex_mip_supported()) return 0;
    struct caj_hip_channel_format_desc cd = cajeta_hip_channel_desc(format);
    struct caj_hip_extent ext; ext.w = w; ext.h = h; ext.d = 0;  // 2-D mip array
    void* mipmap = NULL;
    // NB: on gfx1151 hipMallocMipmappedArray returns hipErrorNotSupported(801) —
    // mipmapped arrays are unimplemented in the HIP runtime on this APU (ROCm
    // 7.11.0). With the emulation above that no longer matters; this
    // native path remains for HW/runtimes that DO implement mipmapped arrays.
    if (g_xpu_hip.hipMallocMipmappedArray(&mipmap, &cd, ext, levels, 0) != 0 ||
        !mipmap)
        return 0;
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) {
        if (g_xpu_hip.hipFreeMipmappedArray) g_xpu_hip.hipFreeMipmappedArray(mipmap);
        return 0;
    }
    memset(t, 0, sizeof(*t));   // emulated=0 + null the emulated-path pointers
    t->array = NULL; t->mipmap = mipmap; t->w = w; t->h = h; t->d = 1;
    t->format = format; t->levels = (int) levels;
    return (int64_t) (intptr_t) t;
}

static void cajeta_xpu_hip_tex_upload_level(int64_t handle, const float* src,
                                            uint32_t lw, uint32_t lh,
                                            uint32_t level, int32_t format) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) handle;
    if (!t) return;
    if (t->emulated) {
        cajeta_xpu_hip_tex_upload_level_emulated(t, src, lw, lh, level, format);
        return;
    }
    if (!t->mipmap || (int) level >= t->levels) return;
    void* levelArray = NULL;   // owned by the mipmapped array; not freed here
    if (g_xpu_hip.hipGetMipmappedArrayLevel(&levelArray, t->mipmap, level) != 0 ||
        !levelArray)
        return;
    size_t rowBytes = (size_t) lw * cajeta_texfmt_texel_bytes(format);
    if (cajeta_texfmt_is_unorm(format) || cajeta_texfmt_is_half(format)) {
        size_t texels = (size_t) lw * lh * cajeta_texfmt_channels(format);
        unsigned char* tmp =
            (unsigned char*) malloc(texels * cajeta_texfmt_channel_bytes(format));
        if (!tmp) return;
        cajeta_texfmt_encode(tmp, src, texels, format);
        g_xpu_hip.hipMemcpy2DToArray(levelArray, 0, 0, tmp, rowBytes, rowBytes, lh,
                                     CAJ_HIP_MEMCPY_HTOD);
        free(tmp);
    } else {
        g_xpu_hip.hipMemcpy2DToArray(levelArray, 0, 0, src, rowBytes, rowBytes, lh,
                                     CAJ_HIP_MEMCPY_HTOD);
    }
}

static void cajeta_xpu_hip_tex_free(int64_t handle) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) handle;
    if (!t) return;
    if (t->emulated) {
        if (t->srdBlob && g_xpu_hip.hipHostFree) g_xpu_hip.hipHostFree(t->srdBlob);
        if (t->devAlloc && g_xpu_hip.hipFree) g_xpu_hip.hipFree(t->devAlloc);
        if (t->addr && g_xpu_amdtex.destroy) g_xpu_amdtex.destroy(t->addr);
        free(t->stagingHost);
        free(t);
        return;
    }
    if (t->array && g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(t->array);
    if (t->mipmap && g_xpu_hip.hipFreeMipmappedArray)
        g_xpu_hip.hipFreeMipmappedArray(t->mipmap);
    free(t);
}

// Build a hipTextureObject from a texture record's array + a cajeta Sampler's
// modes (filterMode 0=nearest/1=linear, addressMode 0=clamp/1=wrap), normalized
// coords, element-type read. Returns the object pointer (as int64) or 0.
static int64_t cajeta_xpu_hip_make_texobj(int64_t texHandle, int32_t filterMode,
                                          int32_t addressMode) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) texHandle;
    if (!t) return 0;
    // Emulated mip texture: the "texobj" is our hand-built SRD blob (rebuilt with
    // the bound sampler's modes), NOT a hipTextureObject — see the cleanup guard
    // in the launch path (it must not be hipDestroyTextureObject'd).
    if (t->emulated)
        return cajeta_xpu_hip_mip_build_srd_blob(t, filterMode, addressMode);
    if ((!t->array && !t->mipmap) || !cajeta_hip_tex_supported()) return 0;
    struct caj_hip_resource_desc rd;
    memset(&rd, 0, sizeof(rd));
    if (t->mipmap) {   // mip Texture2D — bind the whole mipmapped array
        rd.resType = CAJ_HIP_RES_MIPMAPPED_ARRAY;
        rd.res.mipmap.mipmap = t->mipmap;
    } else {
        rd.resType = CAJ_HIP_RES_ARRAY;
        rd.res.array.array = t->array;
    }
    struct caj_hip_texture_desc td;
    memset(&td, 0, sizeof(td));
    int hipAddr = addressMode == 1 ? CAJ_HIP_ADDR_WRAP : CAJ_HIP_ADDR_CLAMP;
    td.addressMode[0] = hipAddr; td.addressMode[1] = hipAddr;
    td.addressMode[2] = hipAddr;
    int hipFilter = filterMode == 1 ? CAJ_HIP_FILTER_LINEAR : CAJ_HIP_FILTER_POINT;
    td.filterMode = hipFilter;
    // UNORM arrays read back as normalized float [0,1]; float arrays read raw.
    td.readMode = cajeta_texfmt_is_unorm(t->format) ? CAJ_HIP_READ_NORMALIZED_FLOAT
                                                    : CAJ_HIP_READ_ELEMENT;
    td.normalizedCoords = 1;
    // Mip clamp: maxMipmapLevelClamp must admit the highest level an explicit-LOD
    // sample can request — 0 would clamp every __ockl_image_sample_lod_2D to level
    // 0 (the AMD analog of the Vulkan sampler maxLod=0 bug). Inter-level filter
    // mirrors the magnify filter; harmless for non-mip (levels=1 → clamp 0).
    td.mipmapFilterMode = hipFilter;
    td.minMipmapLevelClamp = 0.0f;
    td.maxMipmapLevelClamp = t->levels > 1 ? (float) (t->levels - 1) : 0.0f;
    void* texObj = NULL;
    if (g_xpu_hip.hipCreateTextureObject(&texObj, &rd, &td, NULL) != 0)
        return 0;
    return (int64_t) (intptr_t) texObj;
}

// Build a SURFACE object for an Image2D storage image (the writable twin of
// make_texobj). Just the ARRAY resource desc — no sampler, no read mode (a
// surface read/write is raw). The kernel consumes it via __ockl_image_store_2D /
// __ockl_image_load_2D. Returns 0 if surfaces are unsupported or creation fails.
static int64_t cajeta_xpu_hip_make_surfobj(int64_t imgHandle) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) imgHandle;
    if (!t || !t->array || !cajeta_hip_surf_supported()) return 0;
    struct caj_hip_resource_desc rd;
    memset(&rd, 0, sizeof(rd));
    rd.resType = CAJ_HIP_RES_ARRAY;
    rd.res.array.array = t->array;
    void* surfObj = NULL;
    if (g_xpu_hip.hipCreateSurfaceObject(&surfObj, &rd) != 0) return 0;
    return (int64_t) (intptr_t) surfObj;
}

// --- Texture2D + Sampler (Item 8) -------------------------------------------
// A Texture2D is a small host-side handle (deviceHandle + width/height) over a
// device image; on the CPU backend the device image IS a host allocation. The
// int64 deviceHandle is a pointer to this texobj — a row-major float32 image
// with its dimensions — exactly as a Buffer's handle is its host block. The
// kernel receives that pointer (marshalled like a buffer) and reads it through
// __cajeta_xpu_cpu_tex_sample, which does the addressing + filtering the GPU
// texture unit would. On Vulkan/AMD the handle is a device image / hipArray
// record and the kernel samples it through the native image path.
#define CAJ_MAX_MIP 16
struct cajeta_cpu_texobj {
    float*   data;     // row-major DECODED float texels (owned). Level 0 starts at
                       // offset 0 (mipoff[0]=0), so non-mip code reads t->data
                       // directly; mip levels follow at mipoff[l].
    uint32_t w;        // level-0 width
    uint32_t h;        // level-0 height
    uint32_t d;        // depth: 1 for a 2-D texture, >=1 for a 3-D volume
    int32_t  format;   // TextureFormat ordinal
    int      channels; // 1 (R) or 4 (RGBA)
    int      levels;   // mip level count (1 = no mipmaps)
    size_t   mipoff[CAJ_MAX_MIP];  // element offset (in floats) of each mip level
    uint32_t mipw[CAJ_MAX_MIP], miph[CAJ_MAX_MIP];  // per-level dims
};

// --- CUDA texture / surface runtime (Texture2D + Image2D) -------------------
