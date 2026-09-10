// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// --- HIP texture helpers ----------------------------------------------------
// On AMD a Texture2D is a hipArray; its texobj is rebuilt at each launch.
static int cajeta_hip_tex_supported(void) {
    return g_xpu_hip.hipMallocArray && g_xpu_hip.hipMemcpy2DToArray &&
           g_xpu_hip.hipCreateTextureObject && g_xpu_hip.hipDestroyTextureObject;
}

// Allocates a 1-level w*h hipArray for `format` as a cajeta_hip_tex handle (0
// when unsupported); UNORM stores bytes read back normalized, others elements.
static int64_t cajeta_xpu_hip_tex_alloc(uint32_t w, uint32_t h, int32_t format) {
    if (!cajeta_hip_tex_supported()) return 0;
    int channels = cajeta_texfmt_channels(format);
    int bits = cajeta_texfmt_is_unorm(format) ? 8
             : cajeta_texfmt_is_half(format)  ? 16
                                              : 32;
    struct caj_hip_channel_format_desc cd;
    memset(&cd, 0, sizeof(cd));
    cd.x = bits;
    if (channels == 4) { cd.y = bits; cd.z = bits; cd.w = bits; }
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
    memset(t, 0, sizeof(*t));
    t->array = array; t->mipmap = NULL; t->w = w; t->h = h; t->d = 1;
    t->format = format; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

// --- Image2D storage images on AMD (the writable twin of the texture path) ---
// A storage image is a surface-capable hipArray bound per launch, no sampler.
static int cajeta_hip_surf_supported(void) {
    return g_xpu_hip.hipMallocArray && g_xpu_hip.hipFreeArray &&
           g_xpu_hip.hipCreateSurfaceObject && g_xpu_hip.hipDestroySurfaceObject &&
           g_xpu_hip.hipMemcpy2DFromArray;
}

// Allocates the surface-capable hipArray behind an Image2D (R32F, 1 level).
static int64_t cajeta_xpu_hip_image_alloc(uint32_t w, uint32_t h) {
    if (!cajeta_hip_surf_supported()) return 0;
    struct caj_hip_channel_format_desc cd;
    memset(&cd, 0, sizeof(cd));
    cd.x = 32;
    cd.f = CAJ_HIP_CHANNEL_FLOAT;
    void* array = NULL;
    if (g_xpu_hip.hipMallocArray(&array, &cd, w, h,
                                 CAJ_HIP_ARRAY_SURFACE_LOAD_STORE) != 0 || !array)
        return 0;
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) { if (g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(array); return 0; }
    memset(t, 0, sizeof(*t));
    t->array = array; t->mipmap = NULL; t->w = w; t->h = h; t->d = 1;
    t->format = CAJ_TEXFMT_R32F; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

// Reads the surface array back to `host`; pitch and width in BYTES, height rows.
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

// Texture1D is a hipArray made 1-D by height 0; upload/free reuse the 2-D paths.
static int64_t cajeta_xpu_hip_tex1d_alloc(uint32_t w, int32_t format) {
    if (!cajeta_hip_tex_supported()) return 0;
    struct caj_hip_channel_format_desc cd = cajeta_hip_channel_desc(format);
    void* array = NULL;
    if (g_xpu_hip.hipMallocArray(&array, &cd, w, 0, 0) != 0 || !array) return 0;
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) { if (g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(array); return 0; }
    memset(t, 0, sizeof(*t));
    t->array = array; t->mipmap = NULL; t->w = w; t->h = 1; t->d = 1;
    t->format = format; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

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
    memset(t, 0, sizeof(*t));
    t->array = array; t->mipmap = NULL; t->w = w; t->h = h; t->d = d;
    t->format = format; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

// Texture2DArray is a layered hipArray whose extent.d is the LAYER count.
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
    memset(t, 0, sizeof(*t));
    t->array = array; t->mipmap = NULL; t->w = w; t->h = h; t->d = layers;
    t->format = format; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

// TextureCube is EMULATED as a 6-layer array (hipArrayCubemap is unsupported),
// the major-axis face projection done in-kernel: no seamless edge filtering.
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
    memset(t, 0, sizeof(*t));
    t->array = array; t->mipmap = NULL; t->w = size; t->h = size; t->d = 6;
    t->format = format; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

// Uploads a linear host volume into a 3-D array, encoding UNORM/half en route.
static void cajeta_xpu_hip_tex3d_upload(int64_t handle, const float* src,
                                        uint32_t w, uint32_t h, uint32_t d,
                                        int32_t format) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) handle;
    if (!t || !t->array || w != t->w || h != t->h || d != t->d) return;
    size_t channels = (size_t) cajeta_texfmt_channels(format);
    size_t texelBytes = cajeta_texfmt_texel_bytes(format);
    size_t texels = (size_t) w * h * d * channels;
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
    p.srcPtr.pitch = (size_t) w * texelBytes;
    p.srcPtr.xsize = w;
    p.srcPtr.ysize = h;
    p.dstArray = t->array;
    p.extent.w = w;
    p.extent.h = h;
    p.extent.d = d;
    p.kind = CAJ_HIP_MEMCPY_HTOD;
    g_xpu_hip.hipMemcpy3D(&p);
    if (tmp) free(tmp);
}

// Uploads a linear host image into a 2-D array, encoding UNORM/half en route.
static void cajeta_xpu_hip_tex_upload(int64_t handle, const float* src,
                                      uint32_t w, uint32_t h, int32_t format) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) handle;
    if (!t || !t->array || w != t->w || h != t->h) return;
    size_t rowBytes = (size_t) w * cajeta_texfmt_texel_bytes(format);
    if (cajeta_texfmt_is_unorm(format) || cajeta_texfmt_is_half(format)) {
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

// A native mip Texture2D is a hipMipmappedArray staged one level at a time.
static int cajeta_hip_tex_mip_supported(void) {
    return g_xpu_hip.hipMallocMipmappedArray &&
           g_xpu_hip.hipGetMipmappedArrayLevel && g_xpu_hip.hipMemcpy2DToArray &&
           g_xpu_hip.hipCreateTextureObject && g_xpu_hip.hipDestroyTextureObject;
}

// === Emulated mip Texture2D ==================================================
// Where the HIP runtime lacks mipmapped arrays, a mip texture is one hipMalloc
// tiled by addrlib, sampled through a hand-built SRD; 0 without libcajeta_amdtex.
#define CAJ_HIP_HOST_COHERENT 0x40000000u   // hipHostMallocCoherent

// A 2 MiB-aligned base zeroes the gfx11 _X swizzle's pipe/bank-xor bits.
#define CAJ_AMD_MIP_BASE_ALIGN 0x200000ull

static int cajeta_hip_mip_emulation_available(void) {
    return g_xpu_hip.hipMalloc && g_xpu_hip.hipFree && g_xpu_hip.hipMemcpyHtoD &&
           g_xpu_hip.hipHostMalloc && g_xpu_hip.hipMallocArray &&
           g_xpu_hip.hipFreeArray && g_xpu_hip.hipCreateTextureObject &&
           g_xpu_hip.hipDestroyTextureObject && cajeta_xpu_amdtex_init();
}

// Lays out an emulated mip surface: a tex record (emulated=1), or 0 to fall back.
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
    // The levels share one tile, so this whole-surface staging copy must persist.
    t->stagingHost = calloc(1, lo.surfSize);
    if (!t->stagingHost) {
        g_xpu_hip.hipFree(devAlloc); g_xpu_amdtex.destroy(addr); free(t); return 0;
    }
    return (int64_t) (intptr_t) t;
}

// Host-tiles one level, then re-pushes the whole surface it is scattered across.
static void cajeta_xpu_hip_tex_upload_level_emulated(struct cajeta_hip_tex* t,
                                                     const float* src, uint32_t lw,
                                                     uint32_t lh, uint32_t level,
                                                     int32_t format) {
    if (!t->addr || !t->stagingHost || (int) level >= t->levels) return;
    size_t texelBytes = cajeta_texfmt_texel_bytes(format);
    size_t texels = (size_t) lw * lh * cajeta_texfmt_channels(format);
    unsigned char* lin = (unsigned char*) malloc((size_t) lw * lh * texelBytes);
    if (!lin) return;
    if (cajeta_texfmt_is_unorm(format) || cajeta_texfmt_is_half(format) ||
        cajeta_texfmt_is_integer(format))
        cajeta_texfmt_encode(lin, src, texels, format);
    else
        memcpy(lin, src, (size_t) lw * lh * texelBytes);
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

// Builds the texobj of an emulated mip texture: an SVM blob {img[8],pad,samp[4]}
// cloned from a live single-level texobj, then patched below. The ptr, or 0.
static int64_t cajeta_xpu_hip_mip_build_srd_blob(struct cajeta_hip_tex* t,
                                                 int32_t filterMode,
                                                 int32_t addressMode) {
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
    uint32_t T[16]; memcpy(T, (const void*) tmplObj, sizeof(T));
    g_xpu_hip.hipDestroyTextureObject(tmplObj);
    if (g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(tmplArr);

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
    // A single-level template leaves MIP_FILTER=none, pinning samples to level 0.
    srd[13] = (srd[13] & ~0xFFFu);                                    // MIN_LOD = 0
    srd[13] = (srd[13] & ~(0xFFFu << 12)) | (0xFFFu << 12);           // MAX_LOD = max
    uint32_t mipFilter = filterMode == 1 ? 2u : 1u;  // linear(trilinear) vs point
    srd[14] = (srd[14] & ~(0x3u << 26)) | (mipFilter << 26);          // MIP_FILTER

    if (!t->srdBlob &&
        g_xpu_hip.hipHostMalloc(&t->srdBlob, sizeof(srd), CAJ_HIP_HOST_COHERENT) != 0)
        t->srdBlob = NULL;
    if (!t->srdBlob) return 0;
    memcpy(t->srdBlob, srd, sizeof(srd));
    return (int64_t) (intptr_t) t->srdBlob;
}

// Allocates a mip Texture2D, preferring emulation because some HIP runtimes lack
// mipmapped arrays; falls back to the native path, and returns 0 if neither is.
static int64_t cajeta_xpu_hip_tex_alloc_mip(uint32_t w, uint32_t h, int32_t format,
                                            uint32_t levels) {
    int64_t emu = cajeta_xpu_hip_tex_alloc_mip_emulated(w, h, format, levels);
    if (emu) return emu;
    if (!cajeta_hip_tex_mip_supported()) return 0;
    struct caj_hip_channel_format_desc cd = cajeta_hip_channel_desc(format);
    struct caj_hip_extent ext; ext.w = w; ext.h = h; ext.d = 0;
    void* mipmap = NULL;
    if (g_xpu_hip.hipMallocMipmappedArray(&mipmap, &cd, ext, levels, 0) != 0 ||
        !mipmap)
        return 0;
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) {
        if (g_xpu_hip.hipFreeMipmappedArray) g_xpu_hip.hipFreeMipmappedArray(mipmap);
        return 0;
    }
    memset(t, 0, sizeof(*t));
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

// Builds a hipTextureObject from a record's array and a Sampler's modes
// (filterMode 0=nearest/1=linear, addressMode 0=clamp/1=wrap). The ptr, or 0.
static int64_t cajeta_xpu_hip_make_texobj(int64_t texHandle, int32_t filterMode,
                                          int32_t addressMode) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) texHandle;
    if (!t) return 0;
    // An emulated mip texture's "texobj" is an SRD blob: never destroy it as one.
    if (t->emulated)
        return cajeta_xpu_hip_mip_build_srd_blob(t, filterMode, addressMode);
    if ((!t->array && !t->mipmap) || !cajeta_hip_tex_supported()) return 0;
    struct caj_hip_resource_desc rd;
    memset(&rd, 0, sizeof(rd));
    if (t->mipmap) {
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
    td.readMode = cajeta_texfmt_is_unorm(t->format) ? CAJ_HIP_READ_NORMALIZED_FLOAT
                                                    : CAJ_HIP_READ_ELEMENT;
    td.normalizedCoords = 1;
    // maxMipmapLevelClamp must admit the top level, else sample_lod clamps to 0.
    td.mipmapFilterMode = hipFilter;
    td.minMipmapLevelClamp = 0.0f;
    td.maxMipmapLevelClamp = t->levels > 1 ? (float) (t->levels - 1) : 0.0f;
    void* texObj = NULL;
    if (g_xpu_hip.hipCreateTextureObject(&texObj, &rd, &td, NULL) != 0)
        return 0;
    return (int64_t) (intptr_t) texObj;
}

// Builds an Image2D surface object: array desc only, no sampler or read mode.
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

// --- Texture2D + Sampler on the CPU backend ---------------------------------
// There the device image IS a host allocation the int64 deviceHandle points at.
#define CAJ_MAX_MIP 16
struct cajeta_cpu_texobj {
    float*   data;     // row-major decoded texels (owned); level l at mipoff[l]
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
