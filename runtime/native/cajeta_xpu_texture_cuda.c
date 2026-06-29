// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// --- CUDA texture / surface runtime (Texture2D + Image2D) -------------------
// The AMD/Vulkan parity gap, now wired for the NVPTX path. A Texture2D is a
// CUDA array (cuArrayCreate) whose handle is a pointer to a cajeta_cuda_tex
// record; the CUtexObject (which carries the image+sampler state the kernel's
// tex.unified.2d reads) is built per launch in cajeta_xpu_launch_cuda from the
// array + the paired Sampler's modes. Image2D is the writable twin: a CUDA
// array bound as a CUsurfObject (no sampler) the kernel writes via sust.b.2d.
//
// The driver structs are mirrored here byte-exact (cuda.h is not on the include
// path); the entry points are dlsym'd (cuArrayCreate_v2 etc.). Scope matches the
// NVPTX lowering (NvptxKernelLowering): 2-D, float/unorm/half formats sampled to
// v4f32 (integer textures need the v4s32 intrinsic — not lowered), R32F surface
// images. 1-D/3-D/array/cube and mipmaps are follow-ons.

// CUarray_format (cuda.h). Integer textures aren't supported by the NVPTX v4f32
// sample path, so only UNSIGNED_INT8 (UNORM), HALF and FLOAT are produced here.
enum {
    CAJ_CU_AD_FORMAT_UNSIGNED_INT8  = 0x01,
    CAJ_CU_AD_FORMAT_HALF           = 0x10,
    CAJ_CU_AD_FORMAT_FLOAT          = 0x20
};
// CUresourcetype / CUaddress_mode / CUfilter_mode / CUmemorytype / tex flags.
enum { CAJ_CU_RESOURCE_TYPE_ARRAY = 0 };
enum { CAJ_CU_TR_ADDRESS_MODE_WRAP = 0, CAJ_CU_TR_ADDRESS_MODE_CLAMP = 1 };
enum { CAJ_CU_TR_FILTER_MODE_POINT = 0, CAJ_CU_TR_FILTER_MODE_LINEAR = 1 };
enum { CAJ_CU_MEMORYTYPE_HOST = 1, CAJ_CU_MEMORYTYPE_DEVICE = 2,
       CAJ_CU_MEMORYTYPE_ARRAY = 3 };
enum { CAJ_CU_TRSF_READ_AS_INTEGER = 1, CAJ_CU_TRSF_NORMALIZED_COORDINATES = 2 };

typedef struct {
    size_t Width;
    size_t Height;
    unsigned int Format;       // CUarray_format
    unsigned int NumChannels;  // 1, 2 or 4
} caj_cu_array_desc;

typedef struct {
    int resType;               // CUresourcetype
    union {
        struct { void* hArray; } array;
        struct { void* hMipmappedArray; } mipmap;
        struct { unsigned long long devPtr; unsigned int format;
                 unsigned int numChannels; size_t sizeInBytes; } linear;
        struct { unsigned long long devPtr; unsigned int format;
                 unsigned int numChannels; size_t width, height,
                 pitchInBytes; } pitch2D;
        struct { int reserved[32]; } reserved;
    } res;
    unsigned int flags;        // must be 0
} caj_cu_resource_desc;

typedef struct {
    int addressMode[3];        // CUaddress_mode
    int filterMode;            // CUfilter_mode
    unsigned int flags;        // CU_TRSF_*
    unsigned int maxAnisotropy;
    int mipmapFilterMode;
    float mipmapLevelBias;
    float minMipmapLevelClamp;
    float maxMipmapLevelClamp;
    float borderColor[4];
    int reserved[12];
} caj_cu_texture_desc;

typedef struct {
    size_t srcXInBytes, srcY;
    int srcMemoryType;         // CUmemorytype
    const void* srcHost;
    unsigned long long srcDevice;
    void* srcArray;
    size_t srcPitch;
    size_t dstXInBytes, dstY;
    int dstMemoryType;
    void* dstHost;
    unsigned long long dstDevice;
    void* dstArray;
    size_t dstPitch;
    size_t WidthInBytes;
    size_t Height;
} caj_cu_memcpy2d;

struct cajeta_cuda_tex {
    void* array;       // CUarray
    uint32_t w, h;
    int32_t format;
    int channels;
};

static int cajeta_cu_tex_supported(void) {
    return g_xpu_cuda.cuArrayCreate && g_xpu_cuda.cuMemcpy2D &&
           g_xpu_cuda.cuTexObjectCreate && g_xpu_cuda.cuTexObjectDestroy &&
           g_xpu_cuda.cuArrayDestroy;
}
static int cajeta_cu_surf_supported(void) {
    return g_xpu_cuda.cuArrayCreate && g_xpu_cuda.cuMemcpy2D &&
           g_xpu_cuda.cuSurfObjectCreate && g_xpu_cuda.cuSurfObjectDestroy &&
           g_xpu_cuda.cuArrayDestroy;
}

// cajeta TextureFormat → CUarray_format. Integer formats return 0 (unsupported
// by the v4f32 NVPTX sample path; the caller skips them).
static unsigned int cajeta_cu_array_format(int32_t fmt) {
    if (cajeta_texfmt_is_integer(fmt)) return 0;       // not supported on NVPTX
    if (cajeta_texfmt_is_unorm(fmt))   return CAJ_CU_AD_FORMAT_UNSIGNED_INT8;
    if (cajeta_texfmt_is_half(fmt))    return CAJ_CU_AD_FORMAT_HALF;
    return CAJ_CU_AD_FORMAT_FLOAT;
}

static int64_t cajeta_xpu_cuda_tex_alloc(uint32_t w, uint32_t h, int32_t format) {
    if (!cajeta_cu_tex_supported() || w == 0 || h == 0) return 0;
    unsigned int cufmt = cajeta_cu_array_format(format);
    if (cufmt == 0) return 0;                          // integer texture — skip
    caj_cu_array_desc ad;
    memset(&ad, 0, sizeof(ad));
    ad.Width = w; ad.Height = h; ad.Format = cufmt;
    ad.NumChannels = (unsigned int) cajeta_texfmt_channels(format);
    void* array = NULL;
    if (g_xpu_cuda.cuArrayCreate(&array, &ad) != 0 || !array) return 0;
    struct cajeta_cuda_tex* t =
        (struct cajeta_cuda_tex*) malloc(sizeof(*t));
    if (!t) { g_xpu_cuda.cuArrayDestroy(array); return 0; }
    t->array = array; t->w = w; t->h = h; t->format = format;
    t->channels = cajeta_texfmt_channels(format);
    return (int64_t) (intptr_t) t;
}

// Upload host floats into the CUDA array, quantizing/converting to the array's
// stored type so the device read matches the CPU oracle (UNORM→u8, HALF→f16,
// FLOAT→f32). `src` is channel-interleaved floats (channels per texel).
static void cajeta_xpu_cuda_tex_upload(int64_t handle, const float* src,
                                       uint32_t w, uint32_t h, int32_t format) {
    struct cajeta_cuda_tex* t = (struct cajeta_cuda_tex*) (intptr_t) handle;
    if (!t || !t->array || !src) return;
    int ch = cajeta_texfmt_channels(format);
    size_t texels = (size_t) w * h * ch;
    size_t elemBytes;
    void* staged = NULL;
    if (cajeta_texfmt_is_unorm(format)) {
        elemBytes = 1;
        uint8_t* b = (uint8_t*) malloc(texels);
        if (!b) return;
        for (size_t i = 0; i < texels; ++i) b[i] = cajeta_texfmt_unorm8(src[i]);
        staged = b;
    } else if (cajeta_texfmt_is_half(format)) {
        elemBytes = 2;
        uint16_t* b = (uint16_t*) malloc(texels * 2);
        if (!b) return;
        for (size_t i = 0; i < texels; ++i) b[i] = cajeta_f32_to_f16(src[i]);
        staged = b;
    } else {
        elemBytes = 4;
        staged = (void*) src;   // f32 stored as-is
    }
    caj_cu_memcpy2d c;
    memset(&c, 0, sizeof(c));
    c.srcMemoryType = CAJ_CU_MEMORYTYPE_HOST;
    c.srcHost = staged;
    c.srcPitch = (size_t) w * ch * elemBytes;
    c.dstMemoryType = CAJ_CU_MEMORYTYPE_ARRAY;
    c.dstArray = t->array;
    c.WidthInBytes = (size_t) w * ch * elemBytes;
    c.Height = h;
    g_xpu_cuda.cuMemcpy2D(&c);
    if (staged != (void*) src) free(staged);
}

static void cajeta_xpu_cuda_tex_free(int64_t handle) {
    struct cajeta_cuda_tex* t = (struct cajeta_cuda_tex*) (intptr_t) handle;
    if (!t) return;
    if (t->array && g_xpu_cuda.cuArrayDestroy) g_xpu_cuda.cuArrayDestroy(t->array);
    free(t);
}

// Image2D storage image: an R32F CUDA array bound per launch as a CUsurfObject.
// Driver-API CUDA arrays are surface-capable directly (no special create flag,
// unlike the runtime API's cudaArraySurfaceLoadStore).
static int64_t cajeta_xpu_cuda_image_alloc(uint32_t w, uint32_t h) {
    if (!cajeta_cu_surf_supported() || w == 0 || h == 0) return 0;
    caj_cu_array_desc ad;
    memset(&ad, 0, sizeof(ad));
    ad.Width = w; ad.Height = h;
    ad.Format = CAJ_CU_AD_FORMAT_FLOAT; ad.NumChannels = 1;   // R32F
    void* array = NULL;
    if (g_xpu_cuda.cuArrayCreate(&array, &ad) != 0 || !array) return 0;
    struct cajeta_cuda_tex* t = (struct cajeta_cuda_tex*) malloc(sizeof(*t));
    if (!t) { g_xpu_cuda.cuArrayDestroy(array); return 0; }
    t->array = array; t->w = w; t->h = h; t->format = CAJ_TEXFMT_R32F;
    t->channels = 1;
    return (int64_t) (intptr_t) t;
}

static void cajeta_xpu_cuda_image_download(int64_t handle, void* host,
                                           uint32_t w, uint32_t h) {
    struct cajeta_cuda_tex* t = (struct cajeta_cuda_tex*) (intptr_t) handle;
    if (!t || !t->array || !host || !g_xpu_cuda.cuMemcpy2D) return;
    caj_cu_memcpy2d c;
    memset(&c, 0, sizeof(c));
    c.srcMemoryType = CAJ_CU_MEMORYTYPE_ARRAY;
    c.srcArray = t->array;
    c.dstMemoryType = CAJ_CU_MEMORYTYPE_HOST;
    c.dstHost = host;
    c.dstPitch = (size_t) w * sizeof(float);
    c.WidthInBytes = (size_t) w * sizeof(float);
    c.Height = h;
    g_xpu_cuda.cuMemcpy2D(&c);
}

static void cajeta_xpu_cuda_image_free(int64_t handle) {
    cajeta_xpu_cuda_tex_free(handle);   // same record shape
}

// Build a CUtexObject from a texture record + the bound Sampler's modes. Returns
// the u64 handle (0 on failure). NORMALIZED_COORDINATES matches the NVPTX
// tex.unified.2d normalized (u,v); UNORM reads back as normalized float (no
// READ_AS_INTEGER).
static unsigned long long cajeta_xpu_cuda_make_texobj(int64_t handle,
                                                      int32_t filterMode,
                                                      int32_t addressMode) {
    struct cajeta_cuda_tex* t = (struct cajeta_cuda_tex*) (intptr_t) handle;
    if (!t || !t->array || !cajeta_cu_tex_supported()) return 0;
    caj_cu_resource_desc rd;
    memset(&rd, 0, sizeof(rd));
    rd.resType = CAJ_CU_RESOURCE_TYPE_ARRAY;
    rd.res.array.hArray = t->array;
    caj_cu_texture_desc td;
    memset(&td, 0, sizeof(td));
    int cuAddr = addressMode == 1 ? CAJ_CU_TR_ADDRESS_MODE_WRAP
                                  : CAJ_CU_TR_ADDRESS_MODE_CLAMP;
    td.addressMode[0] = cuAddr; td.addressMode[1] = cuAddr; td.addressMode[2] = cuAddr;
    td.filterMode = filterMode == 1 ? CAJ_CU_TR_FILTER_MODE_LINEAR
                                    : CAJ_CU_TR_FILTER_MODE_POINT;
    td.flags = CAJ_CU_TRSF_NORMALIZED_COORDINATES;   // float read (no READ_AS_INTEGER)
    td.maxMipmapLevelClamp = 0.0f;
    unsigned long long obj = 0;
    if (g_xpu_cuda.cuTexObjectCreate(&obj, &rd, &td, NULL) != 0) return 0;
    return obj;
}

// Build a CUsurfObject for an Image2D (the writable twin; no sampler).
static unsigned long long cajeta_xpu_cuda_make_surfobj(int64_t handle) {
    struct cajeta_cuda_tex* t = (struct cajeta_cuda_tex*) (intptr_t) handle;
    if (!t || !t->array || !cajeta_cu_surf_supported()) return 0;
    caj_cu_resource_desc rd;
    memset(&rd, 0, sizeof(rd));
    rd.resType = CAJ_CU_RESOURCE_TYPE_ARRAY;
    rd.res.array.hArray = t->array;
    unsigned long long obj = 0;
    if (g_xpu_cuda.cuSurfObjectCreate(&obj, &rd) != 0) return 0;
    return obj;
}

// __cajeta_xpu_texture_alloc(this, width, height) -> int64 handle.
// Instance @Native (the Buffer convention): the leading `self` is the cajeta
// `this`, ignored — the device side is keyed on the returned handle.
int64_t __cajeta_xpu_texture_alloc(void* self, uint32_t width, uint32_t height,
                                   int32_t format) {
    (void) self;
    if (width == 0 || height == 0) return 0;
    int channels = cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) malloc(sizeof(*t));
            if (!t) return 0;
            t->w = width;
            t->h = height;
            t->d = 1;            // 2-D texture: single depth slice
            t->format = format;
            t->channels = channels;
            t->levels = 1;       // no mipmaps; level 0 at offset 0
            t->mipoff[0] = 0; t->mipw[0] = width; t->miph[0] = height;
            // CPU stores DECODED channel-interleaved floats (channels per texel);
            // UNORM precision is emulated at upload, so the sampler is float-only.
            t->data = (float*) calloc((size_t) width * height * channels,
                                      sizeof(float));
            if (!t->data) { free(t); return 0; }
            return (int64_t) (intptr_t) t;
        }
        case CAJ_XPU_VULKAN:
            return cajeta_xpu_vk_tex_alloc(width, height, 0, format, 1, 2, 1, 1); // sampled 2-D
        case CAJ_XPU_HIP:
            return cajeta_xpu_hip_tex_alloc(width, height, format);   // hipArray
        case CAJ_XPU_CUDA:
            return cajeta_xpu_cuda_tex_alloc(width, height, format);  // CUDA array
        default: return 0;
    }
}

// __cajeta_xpu_texture_upload(this, handle, host, width, height).
// `host` is a Cajeta float32[] header — { i64 count, [count x f32] data } — so
// the texels start at offset 8 (matches __cajeta_xpu_buffer_upload).
void __cajeta_xpu_texture_upload(void* self, int64_t handle, void* host,
                                 uint32_t width, uint32_t height, int32_t format) {
    (void) self;
    if (!handle || !host || width == 0 || height == 0) return;
    // `host` is a cajeta float32[] header { i64 count, [count x f32] } — the
    // channel-interleaved float texels (R, or R,G,B,A per texel) start at offset 8.
    const float* src = (const float*) ((const char*) host + 8);
    size_t texels = (size_t) width * height * cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            if (!t->data) return;
            if (cajeta_texfmt_is_unorm(format)) {
                // Emulate the device's 256-level UNORM quantization on the CPU so
                // both paths agree bit-for-bit on exactly-representable values.
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = (float) cajeta_texfmt_unorm8(src[i]) / 255.0f;
            } else if (cajeta_texfmt_is_half(format)) {
                // Emulate binary16 storage precision (round-trip through f16) so
                // the CPU path matches the device's half rounding.
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = cajeta_f16_to_f32(cajeta_f32_to_f16(src[i]));
            } else {
                memcpy(t->data, src, texels * sizeof(float));
            }
            return;
        }
        case CAJ_XPU_VULKAN:
            cajeta_xpu_vk_tex_upload(handle, src, width, height, format);
            return;
        case CAJ_XPU_HIP:
            cajeta_xpu_hip_tex_upload(handle, src, width, height, format);
            return;
        case CAJ_XPU_CUDA:
            cajeta_xpu_cuda_tex_upload(handle, src, width, height, format);
            return;
        default: return;
    }
}

void __cajeta_xpu_texture_free(void* self, int64_t handle) {
    (void) self;
    if (!handle) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            free(t->data);
            free(t);
            return;
        }
        case CAJ_XPU_VULKAN: cajeta_xpu_vk_tex_free(handle); return;
        case CAJ_XPU_HIP:    cajeta_xpu_hip_tex_free(handle); return;
        case CAJ_XPU_CUDA:   cajeta_xpu_cuda_tex_free(handle); return;
        default: return;
    }
}

// --- Mipmapped Texture2D ----------------------------------------------------
// A mip chain: level 0 = w x h, level L = max(1, w>>L) x max(1, h>>L). The CPU
// stores all levels in one buffer with per-level offsets (level 0 at offset 0, so
// the non-mip read path is unchanged). Vulkan/HIP mip paths land in later
// increments (default = 1-level fallback so the drop chain still works).

// __cajeta_xpu_texture_alloc_mip(this, w, h, format, levels) -> handle.
int64_t __cajeta_xpu_texture_alloc_mip(void* self, uint32_t width, uint32_t height,
                                       int32_t format, uint32_t levels) {
    (void) self;
    if (width == 0 || height == 0 || levels == 0) return 0;
    if (levels > CAJ_MAX_MIP) levels = CAJ_MAX_MIP;
    int channels = cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) malloc(sizeof(*t));
            if (!t) return 0;
            t->w = width; t->h = height; t->d = 1;
            t->format = format; t->channels = channels;
            t->levels = (int) levels;
            // Lay the mip levels out back-to-back; record each offset + dims.
            size_t off = 0;
            for (uint32_t l = 0; l < levels; ++l) {
                uint32_t lw = width >> l;  if (lw == 0) lw = 1;
                uint32_t lh = height >> l; if (lh == 0) lh = 1;
                t->mipoff[l] = off;
                t->mipw[l] = lw; t->miph[l] = lh;
                off += (size_t) lw * lh * channels;
            }
            t->data = (float*) calloc(off, sizeof(float));
            if (!t->data) { free(t); return 0; }
            return (int64_t) (intptr_t) t;
        }
        case CAJ_XPU_VULKAN:
            // A sampled 2-D image with `levels` mip levels; per-level texels are
            // staged by __cajeta_xpu_texture_upload_level.
            return cajeta_xpu_vk_tex_alloc(width, height, 0, format, 1, 2, 1, levels);
        case CAJ_XPU_HIP:
            return cajeta_xpu_hip_tex_alloc_mip(width, height, format, levels);
        default: return 0;
    }
}

// __cajeta_xpu_texture_upload_level(this, handle, host, lw, lh, level, format).
void __cajeta_xpu_texture_upload_level(void* self, int64_t handle, void* host,
                                       uint32_t lw, uint32_t lh, uint32_t level,
                                       int32_t format) {
    (void) self;
    if (!handle || !host || lw == 0 || lh == 0) return;
    const float* src = (const float*) ((const char*) host + 8);
    size_t texels = (size_t) lw * lh * cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            if (!t->data || (int) level >= t->levels) return;
            float* dst = t->data + t->mipoff[level];
            if (cajeta_texfmt_is_unorm(format)) {
                for (size_t i = 0; i < texels; ++i)
                    dst[i] = (float) cajeta_texfmt_unorm8(src[i]) / 255.0f;
            } else if (cajeta_texfmt_is_half(format)) {
                for (size_t i = 0; i < texels; ++i)
                    dst[i] = cajeta_f16_to_f32(cajeta_f32_to_f16(src[i]));
            } else {
                memcpy(dst, src, texels * sizeof(float));
            }
            return;
        }
        case CAJ_XPU_VULKAN:
            cajeta_xpu_vk_tex_upload_level(handle, src, lw, lh, level, format);
            return;
        case CAJ_XPU_HIP:
            cajeta_xpu_hip_tex_upload_level(handle, src, lw, lh, level, format);
            return;
        default: return;
    }
}

// --- Texture3D (3-D / volumetric textures) ----------------------------------
// The volumetric sibling of Texture2D. Distinct __cajeta_xpu_texture3d_* symbols
// because the 2-D vs 3-D image type (VK_IMAGE_TYPE_3D, hipMalloc3DArray) is fixed
// at allocation. CPU stores a w*h*d*channels DECODED-float volume (row-major: x
// fastest, then y, then z). Vulkan/HIP 3-D image paths land in later increments
// (default = no-op / 0 until then, so the cajeta drop chain still works).

// __cajeta_xpu_texture3d_alloc(this, width, height, depth, format) -> handle.
int64_t __cajeta_xpu_texture3d_alloc(void* self, uint32_t width, uint32_t height,
                                     uint32_t depth, int32_t format) {
    (void) self;
    if (width == 0 || height == 0 || depth == 0) return 0;
    int channels = cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) malloc(sizeof(*t));
            if (!t) return 0;
            t->w = width;
            t->h = height;
            t->d = depth;
            t->format = format;
            t->channels = channels;
            t->levels = 1;
            t->mipoff[0] = 0; t->mipw[0] = width; t->miph[0] = height;
            t->data = (float*) calloc(
                (size_t) width * height * depth * channels, sizeof(float));
            if (!t->data) { free(t); return 0; }
            return (int64_t) (intptr_t) t;
        }
        case CAJ_XPU_VULKAN:
            return cajeta_xpu_vk_tex_alloc(width, height, 0, format, depth, 3, 1, 1);
        case CAJ_XPU_HIP:
            return cajeta_xpu_hip_tex3d_alloc(width, height, depth, format);
        default: return 0;
    }
}

// __cajeta_xpu_texture3d_upload(this, handle, host, width, height, depth, format).
void __cajeta_xpu_texture3d_upload(void* self, int64_t handle, void* host,
                                   uint32_t width, uint32_t height, uint32_t depth,
                                   int32_t format) {
    (void) self;
    if (!handle || !host || width == 0 || height == 0 || depth == 0) return;
    const float* src = (const float*) ((const char*) host + 8);
    size_t texels = (size_t) width * height * depth * cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            if (!t->data) return;
            if (cajeta_texfmt_is_unorm(format)) {
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = (float) cajeta_texfmt_unorm8(src[i]) / 255.0f;
            } else if (cajeta_texfmt_is_half(format)) {
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = cajeta_f16_to_f32(cajeta_f32_to_f16(src[i]));
            } else {
                memcpy(t->data, src, texels * sizeof(float));
            }
            return;
        }
        case CAJ_XPU_VULKAN:
            // cajeta_xpu_vk_tex_upload reads the depth from the texture record,
            // so the 2-D upload path covers 3-D images unchanged.
            cajeta_xpu_vk_tex_upload(handle, src, width, height, format);
            return;
        case CAJ_XPU_HIP:
            cajeta_xpu_hip_tex3d_upload(handle, src, width, height, depth, format);
            return;
        default: return;
    }
}

void __cajeta_xpu_texture3d_free(void* self, int64_t handle) {
    (void) self;
    if (!handle) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            free(t->data);
            free(t);
            return;
        }
        case CAJ_XPU_VULKAN: cajeta_xpu_vk_tex_free(handle); return;
        case CAJ_XPU_HIP:    cajeta_xpu_hip_tex_free(handle); return;
        case CAJ_XPU_CUDA:   cajeta_xpu_cuda_tex_free(handle); return;
        default: return;
    }
}

// --- Texture1D (read-only 1-D images) ---------------------------------------
// Texture1D is the linear sibling of Texture2D/Texture3D: a single (width) row.
// On the CPU it is exactly a 2-D texobj with height = 1, so the alloc/upload
// below build that shape and every CPU read reuses the 2-D sample/fetch path
// (the 2-D bilinear collapses to a 1-D lerp when there is one row). Vulkan/HIP
// are stubbed for 3a and wired in 3b/3c.

// __cajeta_xpu_texture1d_alloc(this, width, format) -> handle.
int64_t __cajeta_xpu_texture1d_alloc(void* self, uint32_t width, int32_t format) {
    (void) self;
    if (width == 0) return 0;
    int channels = cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) malloc(sizeof(*t));
            if (!t) return 0;
            t->w = width;
            t->h = 1;
            t->d = 1;
            t->format = format;
            t->channels = channels;
            t->levels = 1;
            t->mipoff[0] = 0; t->mipw[0] = width; t->miph[0] = 1;
            t->data = (float*) calloc((size_t) width * channels, sizeof(float));
            if (!t->data) { free(t); return 0; }
            return (int64_t) (intptr_t) t;
        }
        case CAJ_XPU_VULKAN:
            // A 1-D sampled image (height = depth = 1, no mips).
            return cajeta_xpu_vk_tex_alloc(width, 1, 0, format, 1, 1, 1, 1);
        case CAJ_XPU_HIP:    return cajeta_xpu_hip_tex1d_alloc(width, format);
        default: return 0;
    }
}

// __cajeta_xpu_texture1d_upload(this, handle, host, width, format).
void __cajeta_xpu_texture1d_upload(void* self, int64_t handle, void* host,
                                   uint32_t width, int32_t format) {
    (void) self;
    if (!handle || !host || width == 0) return;
    const float* src = (const float*) ((const char*) host + 8);
    size_t texels = (size_t) width * cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            if (!t->data) return;
            if (cajeta_texfmt_is_unorm(format)) {
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = (float) cajeta_texfmt_unorm8(src[i]) / 255.0f;
            } else if (cajeta_texfmt_is_half(format)) {
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = cajeta_f16_to_f32(cajeta_f32_to_f16(src[i]));
            } else {
                memcpy(t->data, src, texels * sizeof(float));
            }
            return;
        }
        case CAJ_XPU_VULKAN:
            // A 1-D image is height 1; the upload reads depth (= 1) from the record.
            cajeta_xpu_vk_tex_upload(handle, src, width, 1, format);
            return;
        case CAJ_XPU_HIP:
            // A 1-D hipArray is a height-1 2-D copy — reuse the 2-D upload.
            cajeta_xpu_hip_tex_upload(handle, src, width, 1, format);
            return;
        default: return;
    }
}

void __cajeta_xpu_texture1d_free(void* self, int64_t handle) {
    (void) self;
    if (!handle) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            free(t->data);
            free(t);
            return;
        }
        case CAJ_XPU_VULKAN: cajeta_xpu_vk_tex_free(handle); return;
        case CAJ_XPU_HIP:    cajeta_xpu_hip_tex_free(handle); return;
        case CAJ_XPU_CUDA:   cajeta_xpu_cuda_tex_free(handle); return;
        default: return;
    }
}

// --- Texture2DArray (read-only layered 2-D images) --------------------------
// A 2-D array is N (width, height) planes. On the CPU it is a cajeta_cpu_texobj
// whose `d` field is the layer count and whose storage is laid out exactly like
// a 3-D volume's z slices (so the CPU fetch reuses the 3-D path with z = layer,
// and sample bilinearly filters within one layer). Vulkan/HIP layered images are
// wired in A2/A3.

// __cajeta_xpu_texture2darray_alloc(this, width, height, layers, format) -> handle.
int64_t __cajeta_xpu_texture2darray_alloc(void* self, uint32_t width,
                                          uint32_t height, uint32_t layers,
                                          int32_t format) {
    (void) self;
    if (width == 0 || height == 0 || layers == 0) return 0;
    int channels = cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) malloc(sizeof(*t));
            if (!t) return 0;
            t->w = width;
            t->h = height;
            t->d = layers;       // layer count stored in the volume's depth slot
            t->format = format;
            t->channels = channels;
            t->levels = 1;
            t->mipoff[0] = 0; t->mipw[0] = width; t->miph[0] = height;
            t->data = (float*) calloc(
                (size_t) width * height * layers * channels, sizeof(float));
            if (!t->data) { free(t); return 0; }
            return (int64_t) (intptr_t) t;
        }
        case CAJ_XPU_VULKAN:
            // A layered 2-D sampled image: imageKind 4, arrayLayers = layers.
            return cajeta_xpu_vk_tex_alloc(width, height, 0, format, 1, 4, layers, 1);
        case CAJ_XPU_HIP:
            return cajeta_xpu_hip_tex2darray_alloc(width, height, layers, format);
        default: return 0;
    }
}

// __cajeta_xpu_texture2darray_upload(this, handle, host, width, height, layers, format).
void __cajeta_xpu_texture2darray_upload(void* self, int64_t handle, void* host,
                                        uint32_t width, uint32_t height,
                                        uint32_t layers, int32_t format) {
    (void) self;
    if (!handle || !host || width == 0 || height == 0 || layers == 0) return;
    const float* src = (const float*) ((const char*) host + 8);
    size_t texels =
        (size_t) width * height * layers * cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            if (!t->data) return;
            if (cajeta_texfmt_is_unorm(format)) {
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = (float) cajeta_texfmt_unorm8(src[i]) / 255.0f;
            } else if (cajeta_texfmt_is_half(format)) {
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = cajeta_f16_to_f32(cajeta_f32_to_f16(src[i]));
            } else {
                memcpy(t->data, src, texels * sizeof(float));
            }
            return;
        }
        case CAJ_XPU_VULKAN:
            // The layered image carries its planes in array layers; the upload
            // reads layer count + layered flag from the texture record.
            cajeta_xpu_vk_tex_upload(handle, src, width, height, format);
            return;
        case CAJ_XPU_HIP:
            // A layered hipArray's memcpy3D copies all layers with d = layers.
            cajeta_xpu_hip_tex3d_upload(handle, src, width, height, layers, format);
            return;
        default: return;
    }
}

void __cajeta_xpu_texture2darray_free(void* self, int64_t handle) {
    (void) self;
    if (!handle) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            free(t->data);
            free(t);
            return;
        }
        case CAJ_XPU_VULKAN: cajeta_xpu_vk_tex_free(handle); return;
        case CAJ_XPU_HIP:    cajeta_xpu_hip_tex_free(handle); return;
        case CAJ_XPU_CUDA:   cajeta_xpu_cuda_tex_free(handle); return;
        default: return;
    }
}

// --- TextureCube (read-only cube maps, 6 faces) -----------------------------
// A cube is 6 square faces in +X,-X,+Y,-Y,+Z,-Z order. On the CPU it is a
// cajeta_cpu_texobj whose `d` is 6 (the faces are the z slices), so the CPU
// sampler reuses the volume storage + does the direction→face projection.
// Vulkan/HIP cube images are wired in B2/B3.

// __cajeta_xpu_texturecube_alloc(this, size, format) -> handle.
int64_t __cajeta_xpu_texturecube_alloc(void* self, uint32_t size, int32_t format) {
    (void) self;
    if (size == 0) return 0;
    int channels = cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) malloc(sizeof(*t));
            if (!t) return 0;
            t->w = size;
            t->h = size;
            t->d = 6;            // the 6 faces are the z slices
            t->format = format;
            t->channels = channels;
            t->levels = 1;
            t->mipoff[0] = 0; t->mipw[0] = size; t->miph[0] = size;
            t->data = (float*) calloc(
                (size_t) size * size * 6 * channels, sizeof(float));
            if (!t->data) { free(t); return 0; }
            return (int64_t) (intptr_t) t;
        }
        case CAJ_XPU_VULKAN:
            // A CUBE_COMPATIBLE 2-D image with 6 array layers (the faces).
            return cajeta_xpu_vk_tex_alloc(size, size, 0, format, 1, 5, 6, 1);
        case CAJ_XPU_HIP:    return cajeta_xpu_hip_texcube_alloc(size, format);
        default: return 0;
    }
}

// __cajeta_xpu_texturecube_upload(this, handle, host, size, format).
void __cajeta_xpu_texturecube_upload(void* self, int64_t handle, void* host,
                                     uint32_t size, int32_t format) {
    (void) self;
    if (!handle || !host || size == 0) return;
    const float* src = (const float*) ((const char*) host + 8);
    size_t texels = (size_t) size * size * 6 * cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            if (!t->data) return;
            if (cajeta_texfmt_is_unorm(format)) {
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = (float) cajeta_texfmt_unorm8(src[i]) / 255.0f;
            } else if (cajeta_texfmt_is_half(format)) {
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = cajeta_f16_to_f32(cajeta_f32_to_f16(src[i]));
            } else {
                memcpy(t->data, src, texels * sizeof(float));
            }
            return;
        }
        case CAJ_XPU_VULKAN:
            // The 6 faces are the image's 6 array layers; the upload reads the
            // layer count + layered flag from the texture record (layers = 6).
            cajeta_xpu_vk_tex_upload(handle, src, size, size, format);
            return;
        case CAJ_XPU_HIP:
            // A cubemap hipArray's memcpy3D copies all 6 faces with d = 6.
            cajeta_xpu_hip_tex3d_upload(handle, src, size, size, 6, format);
            return;
        default: return;
    }
}

void __cajeta_xpu_texturecube_free(void* self, int64_t handle) {
    (void) self;
    if (!handle) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            free(t->data);
            free(t);
            return;
        }
        case CAJ_XPU_VULKAN: cajeta_xpu_vk_tex_free(handle); return;
        case CAJ_XPU_HIP:    cajeta_xpu_hip_tex_free(handle); return;
        case CAJ_XPU_CUDA:   cajeta_xpu_cuda_tex_free(handle); return;
        default: return;
    }
}

// --- Image2D (writable storage images) --------------------------------------
// Image2D is the writable twin of Texture2D: a 2-D R32_SFLOAT storage image a
// kernel writes via `img.store(x, y, value)` (OpImageWrite), and the host reads
// back with `img.download(out)`. Vulkan (storage image), AMD (surface object),
// and CPU (the reference host float store); NV returns 0 / no-op so the cajeta
// drop chain still works. The handle is a backend-specific record/index.

// CPU Image2D: the in-process reference store — a flat R32f host float array in a
// cajeta_cpu_texobj (channels=1), the writable twin of the CPU texture path. The
// device kernel writes/reads it via __cajeta_xpu_cpu_image_store/_load below.
static int64_t cajeta_xpu_cpu_image_alloc(uint32_t w, uint32_t h) {
    struct cajeta_cpu_texobj* t =
        (struct cajeta_cpu_texobj*) malloc(sizeof(*t));
    if (!t) return 0;
    t->w = w; t->h = h; t->d = 1;
    t->format = CAJ_TEXFMT_R32F; t->channels = 1; t->levels = 1;
    t->mipoff[0] = 0; t->mipw[0] = w; t->miph[0] = h;
    t->data = (float*) calloc((size_t) w * h, sizeof(float));
    if (!t->data) { free(t); return 0; }
    return (int64_t) (intptr_t) t;
}

static void cajeta_xpu_cpu_image_download(int64_t handle, void* host,
                                          uint32_t w, uint32_t h) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) (intptr_t) handle;
    if (!t || !t->data || !host) return;
    memcpy(host, t->data, (size_t) w * h * sizeof(float));
}

static void cajeta_xpu_cpu_image_free(int64_t handle) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) (intptr_t) handle;
    if (!t) return;
    free(t->data);
    free(t);
}

// __cajeta_xpu_image_alloc(this, width, height) -> int64 handle.
int64_t __cajeta_xpu_image_alloc(void* self, uint32_t width, uint32_t height) {
    (void) self;
    if (width == 0 || height == 0) return 0;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU:
            return cajeta_xpu_cpu_image_alloc(width, height);  // host float store (R32F)
        case CAJ_XPU_VULKAN:
            return cajeta_xpu_vk_tex_alloc(width, height, 1, CAJ_TEXFMT_R32F, 1, 2, 1, 1);  // storage 2-D (R32F)
        case CAJ_XPU_HIP:
            return cajeta_xpu_hip_image_alloc(width, height);  // surface hipArray (R32F)
        case CAJ_XPU_CUDA:
            return cajeta_xpu_cuda_image_alloc(width, height);  // surface CUDA array (R32F)
        default: return 0;
    }
}

// __cajeta_xpu_image_download(this, handle, host, width, height).
// `host` is a Cajeta float32[] header — { i64 count, [count x f32] data } — so
// the texels land at offset 8 (matches __cajeta_xpu_texture_upload in reverse).
void __cajeta_xpu_image_download(void* self, int64_t handle, void* host,
                                 uint32_t width, uint32_t height) {
    (void) self;
    if (!handle || !host || width == 0 || height == 0) return;
    void* data = (void*) ((char*) host + 8);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU:
            cajeta_xpu_cpu_image_download(handle, data, width, height);
            return;
        case CAJ_XPU_VULKAN:
            cajeta_xpu_vk_tex_download(handle, data, width, height);
            return;
        case CAJ_XPU_HIP:
            cajeta_xpu_hip_image_download(handle, data, width, height);
            return;
        case CAJ_XPU_CUDA:
            cajeta_xpu_cuda_image_download(handle, data, width, height);
            return;
        default: return;
    }
}

void __cajeta_xpu_image_free(void* self, int64_t handle) {
    (void) self;
    if (!handle) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: cajeta_xpu_cpu_image_free(handle); return;
        case CAJ_XPU_VULKAN: cajeta_xpu_vk_tex_free(handle); return;
        case CAJ_XPU_HIP: cajeta_xpu_hip_image_free(handle); return;
        case CAJ_XPU_CUDA: cajeta_xpu_cuda_image_free(handle); return;
        default: return;
    }
}

// Portable software BVH builder + layout (the software AccelerationStructure
// noun). Self-contained pure C, also compiled directly by the builder unit test.
#include "cajeta_bvh.c"

// --- Noun seam: the resource-provider SPI (cajeta-gpu inc-4 brick #2) --------
