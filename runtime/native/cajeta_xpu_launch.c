// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// --- Texture3D CPU sample/fetch ---------------------------------------------
// 3-D voxel read from the DECODED float volume, row-major (x fastest, then y,
// then z): index = ((z*h + y)*w + x)*channels. The 3-D analogue of
// cajeta_cpu_texel; missing channels default G/B = 0, A = 1.
static inline caj_v4f cajeta_cpu_texel3d(const struct cajeta_cpu_texobj* t,
                                         int x, int y, int z) {
    const float* p = t->data +
        (((size_t) z * t->h + (size_t) y) * t->w + (size_t) x) * t->channels;
    caj_v4f c = { 0.0f, 0.0f, 0.0f, 1.0f };
    for (int i = 0; i < t->channels; ++i) c[i] = p[i];
    return c;
}

// CPU 3-D texture sampler — the lowering of `tex.sample(sampler, u, v, w)`.
// (u, v, w) normalized in [0, 1]; filterMode 0 = nearest / 1 = trilinear;
// addressMode 0 = clamp / 1 = wrap. Trilinear uses the texel-center convention
// (coord = u*N - 0.5) matching GPU texture units, blending the 8 surrounding
// voxels. The 3-D twin of __cajeta_xpu_cpu_tex_sample_rgba.
caj_v4f __cajeta_xpu_cpu_tex3d_sample_rgba(void* texp, int32_t filterMode,
                                           int32_t addressMode, float u, float v,
                                           float w) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4f zero = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!t || !t->data || t->w == 0 || t->h == 0 || t->d == 0) return zero;
    int W = (int) t->w, H = (int) t->h, D = (int) t->d;
    if (filterMode == 0) {                   // nearest
        int x = cajeta_tex_addr((int) floorf(u * (float) W), W, addressMode);
        int y = cajeta_tex_addr((int) floorf(v * (float) H), H, addressMode);
        int z = cajeta_tex_addr((int) floorf(w * (float) D), D, addressMode);
        return cajeta_cpu_texel3d(t, x, y, z);
    }
    // trilinear (texel-center) — blend eight RGBA voxels
    float fx = u * (float) W - 0.5f;
    float fy = v * (float) H - 0.5f;
    float fz = w * (float) D - 0.5f;
    int x0 = (int) floorf(fx), y0 = (int) floorf(fy), z0 = (int) floorf(fz);
    float dx = fx - (float) x0, dy = fy - (float) y0, dz = fz - (float) z0;
    int cx0 = cajeta_tex_addr(x0,     W, addressMode);
    int cx1 = cajeta_tex_addr(x0 + 1, W, addressMode);
    int cy0 = cajeta_tex_addr(y0,     H, addressMode);
    int cy1 = cajeta_tex_addr(y0 + 1, H, addressMode);
    int cz0 = cajeta_tex_addr(z0,     D, addressMode);
    int cz1 = cajeta_tex_addr(z0 + 1, D, addressMode);
    caj_v4f c000 = cajeta_cpu_texel3d(t, cx0, cy0, cz0);
    caj_v4f c100 = cajeta_cpu_texel3d(t, cx1, cy0, cz0);
    caj_v4f c010 = cajeta_cpu_texel3d(t, cx0, cy1, cz0);
    caj_v4f c110 = cajeta_cpu_texel3d(t, cx1, cy1, cz0);
    caj_v4f c001 = cajeta_cpu_texel3d(t, cx0, cy0, cz1);
    caj_v4f c101 = cajeta_cpu_texel3d(t, cx1, cy0, cz1);
    caj_v4f c011 = cajeta_cpu_texel3d(t, cx0, cy1, cz1);
    caj_v4f c111 = cajeta_cpu_texel3d(t, cx1, cy1, cz1);
    // interpolate along x, then y, then z
    caj_v4f a0 = c000 + (c100 - c000) * dx;
    caj_v4f b0 = c010 + (c110 - c010) * dx;
    caj_v4f a1 = c001 + (c101 - c001) * dx;
    caj_v4f b1 = c011 + (c111 - c011) * dx;
    caj_v4f e0 = a0 + (b0 - a0) * dy;
    caj_v4f e1 = a1 + (b1 - a1) * dy;
    return e0 + (e1 - e0) * dz;
}

// CPU 3-D texelFetch — exact voxel at integer (x, y, z), mip 0, unfiltered.
caj_v4f __cajeta_xpu_cpu_tex3d_fetch_rgba(void* texp, int32_t x, int32_t y,
                                          int32_t z) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4f zero = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!t || !t->data || t->w == 0 || t->h == 0 || t->d == 0) return zero;
    int W = (int) t->w, H = (int) t->h, D = (int) t->d;
    int cx = x < 0 ? 0 : (x >= W ? W - 1 : x);
    int cy = y < 0 ? 0 : (y >= H ? H - 1 : y);
    int cz = z < 0 ? 0 : (z >= D ? D - 1 : z);
    return cajeta_cpu_texel3d(t, cx, cy, cz);
}

// CPU 3-D integer texelFetch — the int twin (raw 32-bit voxel bits read as i32).
caj_v4i __cajeta_xpu_cpu_tex3d_fetch_rgba_i32(void* texp, int32_t x, int32_t y,
                                              int32_t z) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4i zero = { 0, 0, 0, 1 };
    if (!t || !t->data || t->w == 0 || t->h == 0 || t->d == 0) return zero;
    int W = (int) t->w, H = (int) t->h, D = (int) t->d;
    int cx = x < 0 ? 0 : (x >= W ? W - 1 : x);
    int cy = y < 0 ? 0 : (y >= H ? H - 1 : y);
    int cz = z < 0 ? 0 : (z >= D ? D - 1 : z);
    const int32_t* p = (const int32_t*) t->data +
        (((size_t) cz * t->h + (size_t) cy) * t->w + (size_t) cx) * t->channels;
    caj_v4i c = { 0, 0, 0, 1 };
    for (int i = 0; i < t->channels; ++i) c[i] = p[i];
    return c;
}

// --- Texture2DArray CPU sample ----------------------------------------------
// A 2-D array stores its `layers` planes exactly like a 3-D volume's z slices
// (index = ((layer*h + y)*w + x)*channels), so `fetch` reuses the 3-D exact-voxel
// read with z = layer. Only `sample` differs: it filters bilinearly WITHIN the
// integer-selected layer (no cross-layer blend — unlike the 3-D trilinear). The
// lowering of `arr.sample(sampler, u, v, layer)`; `layer` is the integer array
// index (clamped), (u, v) normalized.
caj_v4f __cajeta_xpu_cpu_tex2da_sample_rgba(void* texp, int32_t filterMode,
                                            int32_t addressMode, float u, float v,
                                            int32_t layer) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4f zero = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!t || !t->data || t->w == 0 || t->h == 0 || t->d == 0) return zero;
    int W = (int) t->w, H = (int) t->h, D = (int) t->d;
    int z = layer < 0 ? 0 : (layer >= D ? D - 1 : layer);   // clamp layer index
    if (filterMode == 0) {                   // nearest
        int x = cajeta_tex_addr((int) floorf(u * (float) W), W, addressMode);
        int y = cajeta_tex_addr((int) floorf(v * (float) H), H, addressMode);
        return cajeta_cpu_texel3d(t, x, y, z);
    }
    // bilinear (texel-center) within layer z — blend four RGBA texels
    float fx = u * (float) W - 0.5f;
    float fy = v * (float) H - 0.5f;
    int x0 = (int) floorf(fx), y0 = (int) floorf(fy);
    float dx = fx - (float) x0, dy = fy - (float) y0;
    int cx0 = cajeta_tex_addr(x0,     W, addressMode);
    int cx1 = cajeta_tex_addr(x0 + 1, W, addressMode);
    int cy0 = cajeta_tex_addr(y0,     H, addressMode);
    int cy1 = cajeta_tex_addr(y0 + 1, H, addressMode);
    caj_v4f t00 = cajeta_cpu_texel3d(t, cx0, cy0, z);
    caj_v4f t10 = cajeta_cpu_texel3d(t, cx1, cy0, z);
    caj_v4f t01 = cajeta_cpu_texel3d(t, cx0, cy1, z);
    caj_v4f t11 = cajeta_cpu_texel3d(t, cx1, cy1, z);
    caj_v4f a = t00 + (t10 - t00) * dx;
    caj_v4f b = t01 + (t11 - t01) * dx;
    return a + (b - a) * dy;
}

// --- TextureCube CPU sample -------------------------------------------------
// Sample a cube map by a DIRECTION vector. The 6 faces are stored like a 6-layer
// volume (face = the z slice) in the canonical +X,-X,+Y,-Y,+Z,-Z order. This does
// the standard major-axis face projection (matching the GPU cube convention),
// then bilinear within the selected face. The lowering of
// `cube.sample(sampler, x, y, z)`; the direction need not be normalized.
caj_v4f __cajeta_xpu_cpu_texcube_sample_rgba(void* texp, int32_t filterMode,
                                             int32_t addressMode, float x, float y,
                                             float z) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4f zero = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!t || !t->data || t->w == 0 || t->h == 0 || t->d < 6) return zero;
    float ax = fabsf(x), ay = fabsf(y), az = fabsf(z);
    int face; float sc, tc, ma;
    if (ax >= ay && ax >= az) {            // major axis X
        ma = ax;
        if (x >= 0.0f) { face = 0; sc = -z; tc = -y; }   // +X
        else           { face = 1; sc =  z; tc = -y; }   // -X
    } else if (ay >= ax && ay >= az) {     // major axis Y
        ma = ay;
        if (y >= 0.0f) { face = 2; sc =  x; tc =  z; }   // +Y
        else           { face = 3; sc =  x; tc = -z; }   // -Y
    } else {                               // major axis Z
        ma = az;
        if (z >= 0.0f) { face = 4; sc =  x; tc = -y; }   // +Z
        else           { face = 5; sc = -x; tc = -y; }   // -Z
    }
    if (ma == 0.0f) ma = 1.0f;             // degenerate (0,0,0) → face 0 center
    float u = 0.5f * (sc / ma + 1.0f);
    float v = 0.5f * (tc / ma + 1.0f);
    int W = (int) t->w, H = (int) t->h;
    if (filterMode == 0) {                 // nearest
        int xi = cajeta_tex_addr((int) floorf(u * (float) W), W, addressMode);
        int yi = cajeta_tex_addr((int) floorf(v * (float) H), H, addressMode);
        return cajeta_cpu_texel3d(t, xi, yi, face);
    }
    // bilinear (texel-center) within the selected face
    float fx = u * (float) W - 0.5f;
    float fy = v * (float) H - 0.5f;
    int x0 = (int) floorf(fx), y0 = (int) floorf(fy);
    float dx = fx - (float) x0, dy = fy - (float) y0;
    int cx0 = cajeta_tex_addr(x0,     W, addressMode);
    int cx1 = cajeta_tex_addr(x0 + 1, W, addressMode);
    int cy0 = cajeta_tex_addr(y0,     H, addressMode);
    int cy1 = cajeta_tex_addr(y0 + 1, H, addressMode);
    caj_v4f c00 = cajeta_cpu_texel3d(t, cx0, cy0, face);
    caj_v4f c10 = cajeta_cpu_texel3d(t, cx1, cy0, face);
    caj_v4f c01 = cajeta_cpu_texel3d(t, cx0, cy1, face);
    caj_v4f c11 = cajeta_cpu_texel3d(t, cx1, cy1, face);
    caj_v4f a = c00 + (c10 - c00) * dx;
    caj_v4f b = c01 + (c11 - c01) * dx;
    return a + (b - a) * dy;
}

// --- Launch + module registration -------------------------------------------
// The compiler lowers `kernel.launch(stream, grid:, block:)(args)` to a call
// here, passing the kernel's PTX entry name, 1-D grid/block, and the CUDA
// kernelParams argv (an array of pointers to each argument value). The real
// NVPTX path (cuLaunchKernel via the dlopen'd driver) lands in the host-launch
// runtime; this is the not-yet-wired no-op so the symbol resolves and host
// codegen of a launch site links.
// CUDA launch: lazily load the module + resolve the function, then 1-D launch.
// Marshal the canonical count-shape argv into the OptiX launch params (the layout
// contract in NvptxOptixRayQuery.h) and run the RT-core pipeline. argv slots, in
// signature order: [0] &AS handle, [1..3] &originX/Y/Z device ptr, [4] &out device
// ptr, [5] &n. The packed params struct mirrors LLVM {i64×5, i32, i64} exactly
// (handle@0, origins@8/16/24, out@32, n@40, boxes@48; size 56) — the same struct
// the 3-C-i device probe launched with. `boxes` is the AS's AABB data (NOT a kernel
// arg); the glue retained it at build time (cajeta_xpu_optix_accel_boxes).
// optixHandle is the OptiX AS rep resolved at launch (M3 Phase 2/3): the AS POD's
// primary handle under eager =optix, or the lazily-built OptiX secondary under AUTO.
// It keys the traversable/boxes lookups instead of av[0], so the AUTO path (whose POD
// primary is the software floor) reaches the right OptiX rep.
static void cajeta_xpu_launch_cuda_optix(struct cajeta_optix_rq* rq, void* argv,
                                         int64_t optixHandle) {
    void** av = (void**) argv;
    if (!av) return;
    int64_t asHandle = optixHandle;
    uint64_t trav = cajeta_xpu_optix_traversable(asHandle);
    if (!trav) {
        fprintf(stderr, "cajeta.xpu: OptiX ray-query launch '%s' missing "
                "traversable for AS handle %lld; not launching\n",
                rq->name, (long long) asHandle);
        return;
    }
    int rc;
    if (rq->shape == 1) {
        // Triangle nearest-hit: argv [AS, outT, outI] -> { handle, outT, outI }.
        struct { uint64_t handle, outT, outI; } p;
        p.handle = trav;
        p.outT   = *(uint64_t*) av[1];
        p.outI   = *(uint64_t*) av[2];
        rc = cajeta_xpu_optix_launch_tri(rq->ptx, rq->ptxLen, rq->raygen,
                                         rq->prog1 /*closesthit*/, "" /*anyhit*/,
                                         rq->prog2 /*miss*/, &p, sizeof(p), 1);
    } else if (rq->shape == 2) {
        // Triangle candidate getters: argv [AS, out] -> { handle, out }.
        struct { uint64_t handle, out; } p;
        p.handle = trav;
        p.out    = *(uint64_t*) av[1];
        rc = cajeta_xpu_optix_launch_tri(rq->ptx, rq->ptxLen, rq->raygen,
                                         "" /*closesthit*/, rq->prog1 /*anyhit*/,
                                         rq->prog2 /*miss*/, &p, sizeof(p), 1);
    } else if (rq->shape == 3) {
        // Committed-triangle per-launch: argv [AS, b0, b1, out, n] ->
        // { handle, b0, b1, out, n }; one ray per launch index (width = n).
        struct { uint64_t handle, b0, b1, out; uint32_t n; } p;
        p.handle = trav;
        p.b0     = *(uint64_t*) av[1];
        p.b1     = *(uint64_t*) av[2];
        p.out    = *(uint64_t*) av[3];
        p.n      = *(uint32_t*) av[4];
        rc = cajeta_xpu_optix_launch_tri(rq->ptx, rq->ptxLen, rq->raygen,
                                         rq->prog1 /*closesthit*/, "" /*anyhit*/,
                                         rq->prog2 /*miss*/, &p, sizeof(p), p.n);
    } else {
        // AABB candidate count: argv [AS, ox,oy,oz, out, n] -> the count params.
        struct {
            uint64_t handle, originX, originY, originZ, out;
            uint32_t n;
            uint64_t boxes;
        } p;
        p.handle  = trav;
        p.originX = *(uint64_t*) av[1];
        p.originY = *(uint64_t*) av[2];
        p.originZ = *(uint64_t*) av[3];
        p.out     = *(uint64_t*) av[4];
        p.n       = *(uint32_t*) av[5];
        p.boxes   = cajeta_xpu_optix_accel_boxes(asHandle);
        if (!p.boxes) {
            fprintf(stderr, "cajeta.xpu: OptiX ray-query launch '%s' missing "
                    "boxes for AS handle %lld; not launching\n",
                    rq->name, (long long) asHandle);
            return;
        }
        rc = cajeta_xpu_optix_launch(rq->ptx, rq->ptxLen, rq->raygen,
                                     rq->prog1 /*is*/, rq->prog2 /*anyhit*/,
                                     rq->prog3 /*miss*/, &p, sizeof(p), p.n);
    }
    if (rc != 0)
        fprintf(stderr, "cajeta.xpu: OptiX ray-query launch '%s' failed (%d)\n",
                rq->name, rc);
}

static void cajeta_xpu_launch_cuda(const char* kernelName,
                                   int32_t gridX, int32_t gridY, int32_t gridZ,
                                   int32_t blockX, int32_t blockY, int32_t blockZ,
                                   uint32_t sharedBytes, void* argv,
                                   int64_t streamHandle,
                                   int32_t specCount, const int32_t* specValues) {
    // M3 Phase 2: launch-time impl selection (the verb picks). Read the ACTUAL
    // AccelerationStructure argument's recorded impl (POD offset 12 = ((int32*)pod)[3])
    // rather than a global resolve, so ONE AS can serve an OptiX-shape kernel (which
    // emitted an OptiX program set → optixLaunch / RT cores) AND an Unsupported-shape
    // kernel (no program set → the software cubin over the retained software floor) in
    // the same program. The verb picks per launch; no kernel ever receives a rep it
    // cannot traverse (R2). When the software path gets a non-software AS primary
    // (OptiX) the marshalling pass below swaps in the registered software floor so the
    // software cubin reads a real BVH blob, not the OptixAs* — eliminating the silent
    // fault (R6). Supersedes the M2 global caj_cuda_resolve_as_impl(AUTO)==OPTIX gate.
    {
        void** av0 = (void**) argv;
        struct cajeta_kparams* kpx = cajeta_xpu_find_kparams(kernelName);
        int32_t asArgImpl = -1;            // this launch's AS arg impl (-1 = no AS arg)
        int64_t asPrimary = 0;             // its POD handle (offset 0)
        if (kpx && av0) {
            for (int i = 0; i < kpx->count; ++i)
                if (kpx->kind[i] == CAJETA_KP_ACCEL && av0[i]) {
                    asArgImpl = ((const int32_t*) av0[i])[3];
                    asPrimary = *(int64_t*) av0[i];
                    break;
                }
        }
        struct cajeta_optix_rq* rq = cajeta_xpu_find_optix_rq(kernelName);
        if (rq && cajeta_xpu_optix_available() && asPrimary) {
            // Resolve the OptiX rep for this AS: under eager =optix the POD primary IS
            // the OptiX AS; under AUTO the primary is the software floor and we build
            // (or reuse) the OptiX rep lazily on this first supported-shape launch (R4).
            // Forced =software retains no geometry → resolve returns 0 → software path.
            int64_t optixHandle = 0;
            if (asArgImpl == CAJ_AS_IMPL_OPTIX)
                optixHandle = asPrimary;
            else if (asArgImpl == CAJ_AS_IMPL_SOFTWARE_BVH)
                optixHandle = caj_cuda_as_resolve_optix(asPrimary);
            if (optixHandle) {
                cajeta_xpu_launch_cuda_optix(rq, argv, optixHandle);
                return;
            }
        }
    }
    pthread_mutex_lock(&g_xpu_cuda_lock);
    struct cajeta_xpu_module* e = cajeta_xpu_find_module(kernelName);
    if (e) {
        if (!e->module) {
            if (g_xpu_cuda.cuModuleLoadData(&e->module, e->image) != 0)
                e->module = NULL;
        }
        if (e->module && !e->function) {
            if (g_xpu_cuda.cuModuleGetFunction(&e->function, e->module,
                                               kernelName) != 0)
                e->function = NULL;
        }
    }
    void* fn = e ? e->function : NULL;
    void* mod = e ? e->module : NULL;
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    if (!fn) {
        fprintf(stderr, "cajeta.xpu: no registered kernel '%s' to launch\n",
                kernelName);
        return;
    }
    // Texture / Image2D guard: the CUDA texture+surface runtime (cuArrayCreate /
    // cuTexObjectCreate / cuSurfObjectCreate) IS wired below; an unbacked (0)
    // handle here means alloc failed or the driver lacks the entry points, in
    // which case dispatching would feed tex/suld a null object and FAULT — skip
    // the launch instead (mirrors the HIP launchOk=0 guard) so the caller degrades
    // cleanly. A valid (nonzero) handle proceeds to per-launch object translation.
    {
        void** av = (void**) argv;
        struct cajeta_kparams* kp = cajeta_xpu_find_kparams(kernelName);
        if (kp && kp->count > 0 && av) {
            for (int i = 0; i < kp->count; ++i)
                if ((kp->kind[i] == CAJETA_KP_IMAGE ||
                     kp->kind[i] == CAJETA_KP_TEXTURE) && av[i] &&
                    *(int64_t*) av[i] == 0) {
                    fprintf(stderr, "cajeta.xpu: CUDA texture/storage-image alloc "
                            "failed (driver lacks cuArray/TexObject/SurfObject?); "
                            "not launching '%s'\n", kernelName);
                    return;
                }
        }
    }
    // H9: the CUDA context is bound to the thread that created it (cuCtxCreate);
    // a launch from a different thread (the carrier fiber vs the main thread) runs
    // with no current context -> CUDA_ERROR_INVALID_CONTEXT and the launch is a
    // silent no-op. Make the context current on this thread first, and surface a
    // launch failure instead of discarding the return code.
    if (g_xpu_cuda.cuCtxSetCurrent) g_xpu_cuda.cuCtxSetCurrent(g_xpu_cuda.ctx);
    // Host spec-constant override (stage12-spec-override Phase C): set the
    // module's constant-memory spec globals before launch. The kernel reads
    // `(slot < count) ? values[slot] : default`, so writing count (0 when no
    // override, clearing any prior launch's values) suffices; the symbols are
    // absent for kernels without spec constants → getGlobal fails → skip (the
    // kernel then has no spec read anyway). Verified on-device (RTX 4090,
    // XpuCudaDispatchDeviceTests.{specOverride,noOverride}*); safe-by-default —
    // a failed/absent copy leaves the zero-init default.
    if (mod && g_xpu_cuda.cuModuleGetGlobal && g_xpu_cuda.cuMemcpyHtoD) {
        cajeta_cudeviceptr g; size_t gbytes;
        int32_t count = (specCount > 0 && specValues) ? specCount : 0;
        if (count > 60) count = 60;
        if (g_xpu_cuda.cuModuleGetGlobal(&g, &gbytes, mod,
                "__cajeta_xpu_spec_count") == 0 && gbytes >= sizeof(int32_t)) {
            g_xpu_cuda.cuMemcpyHtoD(g, &count, sizeof(int32_t));
            if (count > 0 && g_xpu_cuda.cuModuleGetGlobal(&g, &gbytes, mod,
                    "__cajeta_xpu_spec_values") == 0) {
                size_t want = (size_t) count * sizeof(int32_t);
                g_xpu_cuda.cuMemcpyHtoD(g, specValues,
                                        want <= gbytes ? want : gbytes);
            }
        }
    }
    // Per-launch kernarg translation (mirrors cajeta_xpu_launch_hip):
    //   TEXTURE      → build a CUtexObject from the texture record + the bound
    //                  Sampler's modes, pass the u64 handle by value.
    //   IMAGE        → build a CUsurfObject (no sampler), pass the u64 by value.
    //   BUFFER_ARRAY → copy the HOST [count, h…] handle array to device memory,
    //                  pass &devPtr (the kernel flat-loads each device handle).
    // Everything else passes through unchanged.
    void** useArgv = (void**) argv;
    void* subArgv[64];
    void* bufArrVals[8];
    cajeta_cudeviceptr bufArrDev[8];
    int nbufarr = 0;
    unsigned long long texObjs[8]; void* texObjVals[8]; int ntex = 0;
    unsigned long long surfObjs[8]; void* surfObjVals[8]; int nsurf = 0;
    // M3 Phase 2: substitute AccelerationStructure PODs (handle swapped to the software
    // floor) for the software-path floor fallback. POD layout {i64 handle, u32 count,
    // i32 impl} = 16 bytes, matching the cajeta AS struct the cubin reads by value.
    struct { int64_t handle; uint32_t count; int32_t impl; } asPods[8];
    int nas = 0;
    {
        struct cajeta_kparams* kpa = cajeta_xpu_find_kparams(kernelName);
        if (kpa && kpa->count > 0 && kpa->count <= 64) {
            int hasXlat = 0;
            for (int i = 0; i < kpa->count; ++i) {
                if (kpa->kind[i] == CAJETA_KP_BUFFER_ARRAY ||
                    kpa->kind[i] == CAJETA_KP_TEXTURE ||
                    kpa->kind[i] == CAJETA_KP_IMAGE) {
                    hasXlat = 1;
                } else if (kpa->kind[i] == CAJETA_KP_ACCEL && ((void**) argv)[i] &&
                           ((const int32_t*) ((void**) argv)[i])[3]
                               != CAJ_AS_IMPL_SOFTWARE_BVH) {
                    // A non-software AS primary reaching the software cubin — needs the
                    // floor swap (R6). The optix path already returned above if taken.
                    hasXlat = 1;
                }
            }
            if (hasXlat) {
                // The (single, v1) Sampler param supplies the filter/address modes.
                int32_t filterMode = CAJ_CU_TR_FILTER_MODE_LINEAR, addressMode = 0;
                for (int i = 0; i < kpa->count; ++i)
                    if (kpa->kind[i] == CAJETA_KP_SAMPLER) {
                        const int32_t* modes = (const int32_t*) ((void**) argv)[i];
                        filterMode = modes[0]; addressMode = modes[1];
                        break;
                    }
                int ok = 1;
                for (int i = 0; i < kpa->count; ++i) {
                    subArgv[i] = ((void**) argv)[i];
                    if (kpa->kind[i] == CAJETA_KP_TEXTURE) {
                        if (ntex >= 8) { ok = 0; break; }
                        int64_t rec = *(int64_t*) ((void**) argv)[i];
                        unsigned long long obj =
                            cajeta_xpu_cuda_make_texobj(rec, filterMode, addressMode);
                        if (!obj) {
                            fprintf(stderr, "cajeta.xpu: CUDA texture-object creation "
                                    "failed for '%s'; not launching\n", kernelName);
                            ok = 0; break;
                        }
                        texObjs[ntex] = obj;
                        texObjVals[ntex] = (void*) (intptr_t) obj;
                        subArgv[i] = &texObjVals[ntex];
                        ++ntex;
                    } else if (kpa->kind[i] == CAJETA_KP_IMAGE) {
                        if (nsurf >= 8) { ok = 0; break; }
                        int64_t rec = *(int64_t*) ((void**) argv)[i];
                        unsigned long long obj = cajeta_xpu_cuda_make_surfobj(rec);
                        if (!obj) {
                            fprintf(stderr, "cajeta.xpu: CUDA surface-object creation "
                                    "failed for '%s'; not launching\n", kernelName);
                            ok = 0; break;
                        }
                        surfObjs[nsurf] = obj;
                        surfObjVals[nsurf] = (void*) (intptr_t) obj;
                        subArgv[i] = &surfObjVals[nsurf];
                        ++nsurf;
                    } else if (kpa->kind[i] == CAJETA_KP_BUFFER_ARRAY) {
                        if (nbufarr >= 8) {
                            fprintf(stderr, "cajeta.xpu: CUDA kernel '%s' uses more than "
                                    "8 bindless buffer arrays; not launching\n",
                                    kernelName);
                            ok = 0; break;
                        }
                        const int64_t* hostArr = (const int64_t*) ((void**) argv)[i];
                        int64_t cnt = hostArr ? hostArr[0] : -1;
                        if (cnt < 0 || cnt > 16) {   // 16 = kMaxBindlessBuffers
                            fprintf(stderr, "cajeta.xpu: CUDA kernel '%s' bindless "
                                    "buffer-array count %lld out of range; not "
                                    "launching\n", kernelName, (long long) cnt);
                            ok = 0; break;
                        }
                        size_t bytes = (size_t) (cnt + 1) * sizeof(int64_t);
                        cajeta_cudeviceptr dev = 0;
                        if (g_xpu_cuda.cuMemAlloc(&dev, bytes) != 0 || !dev) {
                            fprintf(stderr, "cajeta.xpu: CUDA bindless buffer-array "
                                    "device alloc failed for '%s'; not launching\n",
                                    kernelName);
                            ok = 0; break;
                        }
                        if (g_xpu_cuda.cuMemcpyHtoD(dev, hostArr, bytes) != 0) {
                            g_xpu_cuda.cuMemFree(dev);
                            fprintf(stderr, "cajeta.xpu: CUDA bindless buffer-array "
                                    "upload failed for '%s'; not launching\n",
                                    kernelName);
                            ok = 0; break;
                        }
                        bufArrDev[nbufarr] = dev;
                        bufArrVals[nbufarr] = (void*) (intptr_t) dev;
                        subArgv[i] = &bufArrVals[nbufarr];
                        ++nbufarr;
                    } else if (kpa->kind[i] == CAJETA_KP_ACCEL) {
                        // M3 Phase 2 floor swap. The software cubin reads field 0 (the
                        // handle) as the BVH-blob device pointer; a non-software AS
                        // primary (OptiX OptixAs*) would fault. Substitute a POD copy
                        // pointing at the retained software-BVH floor (Phase 1 registry).
                        int32_t asImpl = ((const int32_t*) ((void**) argv)[i])[3];
                        if (asImpl != CAJ_AS_IMPL_SOFTWARE_BVH) {
                            if (nas >= 8) { ok = 0; break; }
                            int64_t primary = *(int64_t*) ((void**) argv)[i];
                            int32_t sImpl = 0; int64_t sHandle = 0;
                            if (!caj_as_sec_lookup(primary, &sImpl, &sHandle) ||
                                sImpl != CAJ_AS_IMPL_SOFTWARE_BVH || !sHandle) {
                                fprintf(stderr, "cajeta.xpu: ray-query kernel '%s' takes "
                                    "the software path but its AccelerationStructure "
                                    "carries no software-BVH floor (impl %d); not "
                                    "launching (forced =optix with an unsupported "
                                    "shape?)\n", kernelName, asImpl);
                                ok = 0; break;
                            }
                            asPods[nas].handle = sHandle;
                            asPods[nas].count  = ((const uint32_t*) ((void**) argv)[i])[2];
                            asPods[nas].impl   = CAJ_AS_IMPL_SOFTWARE_BVH;
                            subArgv[i] = &asPods[nas];
                            ++nas;
                        }
                    }
                }
                if (!ok) {
                    for (int j = 0; j < nbufarr; ++j) g_xpu_cuda.cuMemFree(bufArrDev[j]);
                    for (int j = 0; j < ntex; ++j)
                        g_xpu_cuda.cuTexObjectDestroy(texObjs[j]);
                    for (int j = 0; j < nsurf; ++j)
                        g_xpu_cuda.cuSurfObjectDestroy(surfObjs[j]);
                    return;
                }
                useArgv = subArgv;
            }
        }
    }
    // 3-D grid/block; default stream; kernelParams = the CUDA argv the launch
    // site marshalled (pointers to each arg value). sharedBytes sizes the
    // kernel's dynamic (extern) shared memory; 0 for static-only kernels.
    int launchRc = g_xpu_cuda.cuLaunchKernel(
        fn, (unsigned) gridX, (unsigned) gridY, (unsigned) gridZ,
        (unsigned) blockX, (unsigned) blockY, (unsigned) blockZ,
        (unsigned) sharedBytes, /*stream=*/(void*) (intptr_t) streamHandle,
        useArgv, /*extra=*/NULL);
    // Free per-launch resources. Sync first (the launch is async; texobj/surfobj
    // and the bindless array are read during execution) — mirrors the HIP path's
    // hipDeviceSynchronize-before-free.
    if (nbufarr > 0 || ntex > 0 || nsurf > 0) {
        g_xpu_cuda.cuCtxSynchronize();
        for (int j = 0; j < nbufarr; ++j) g_xpu_cuda.cuMemFree(bufArrDev[j]);
        for (int j = 0; j < ntex; ++j) g_xpu_cuda.cuTexObjectDestroy(texObjs[j]);
        for (int j = 0; j < nsurf; ++j) g_xpu_cuda.cuSurfObjectDestroy(surfObjs[j]);
    }
    if (launchRc != 0)
        fprintf(stderr, "cajeta.xpu: cuLaunchKernel('%s') failed (%d)\n",
                kernelName, launchRc);
}

// HIP launch: lazily load the hsaco module + resolve the function (reusing the
// shared module table — only one device backend is active per run), then 1-D
// launch. Mirrors cajeta_xpu_launch_cuda with hip* entry points. Texture params
// (Item 8 Stage C) are translated here: the argv slot holds a texture-record
// handle, so a hipTextureObject is built from its hipArray + the paired Sampler's
// modes and substituted into the kernelParams (the kernel reads the image+sampler
// SRDs from that object via __ockl_image_sample_2D); destroyed after the launch.
static void cajeta_xpu_launch_hip(const char* kernelName,
                                  int32_t gridX, int32_t gridY, int32_t gridZ,
                                  int32_t blockX, int32_t blockY, int32_t blockZ,
                                  uint32_t sharedBytes, void* argvv,
                                  int64_t streamHandle,
                                  int32_t specCount, const int32_t* specValues) {
    pthread_mutex_lock(&g_xpu_cuda_lock);
    struct cajeta_xpu_module* e = cajeta_xpu_find_module(kernelName);
    if (e) {
        if (!e->module) {
            if (g_xpu_hip.hipModuleLoadData(&e->module, e->image) != 0)
                e->module = NULL;
        }
        if (e->module && !e->function) {
            if (g_xpu_hip.hipModuleGetFunction(&e->function, e->module,
                                               kernelName) != 0)
                e->function = NULL;
        }
    }
    void* fn = e ? e->function : NULL;
    void* mod = e ? e->module : NULL;
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    // Host spec-constant override (Phase C): set the module's constant-memory
    // spec globals before launch (count 0 = no override, clears prior values;
    // absent symbol → no spec constants → skip). UNVERIFIED on-device (AMD
    // in-process JIT is comgr-blocked here); safe-by-default (zero-init = default).
    if (mod && g_xpu_hip.hipModuleGetGlobal && g_xpu_hip.hipMemcpyHtoD) {
        void* g; size_t gbytes;
        int32_t count = (specCount > 0 && specValues) ? specCount : 0;
        if (count > 60) count = 60;
        if (g_xpu_hip.hipModuleGetGlobal(&g, &gbytes, mod,
                "__cajeta_xpu_spec_count") == 0 && gbytes >= sizeof(int32_t)) {
            g_xpu_hip.hipMemcpyHtoD(g, &count, sizeof(int32_t));
            if (count > 0 && g_xpu_hip.hipModuleGetGlobal(&g, &gbytes, mod,
                    "__cajeta_xpu_spec_values") == 0) {
                size_t want = (size_t) count * sizeof(int32_t);
                g_xpu_hip.hipMemcpyHtoD(g, specValues,
                                        want <= gbytes ? want : gbytes);
            }
        }
    }
    if (!fn) {
        fprintf(stderr, "cajeta.xpu: no registered kernel '%s' to launch\n",
                kernelName);
        return;
    }
    void** argv = (void**) argvv;

    // Texture/surface-object translation (only if this kernel has a Texture2D or
    // Image2D param). A Texture2D param is bound as a sampled texture object; an
    // Image2D param as a writable surface object — both arrive at the kernel as a
    // ptr-addrspace(4) kernarg, so we substitute &objVal into the argv slot.
    void** useArgv = argv;
    void* subArgv[64];
    void* texObjVals[8];
    int64_t texObjs[8];
    int texObjEmu[8] = {0};   // 1 = emulated mip blob (owned by record; don't destroy)
    int ntex = 0;
    void* surfObjVals[8];
    int64_t surfObjs[8];
    int nsurf = 0;
    void* bufArrVals[8];   // bindless device-array pointer values (&slot stays stable)
    void* bufArrDev[8];    // device copies of [count, h…] to free after the launch
    int nbufarr = 0;
    int launchOk = 1;
    struct cajeta_kparams* kp = cajeta_xpu_find_kparams(kernelName);
    if (kp && kp->count > 0 && kp->count <= 64) {
        int hasTex = 0, hasImg = 0, hasBufArr = 0;
        for (int i = 0; i < kp->count; ++i) {
            if (kp->kind[i] == CAJETA_KP_TEXTURE) hasTex = 1;
            else if (kp->kind[i] == CAJETA_KP_IMAGE) hasImg = 1;
            else if (kp->kind[i] == CAJETA_KP_BUFFER_ARRAY) hasBufArr = 1;
        }
        if (hasTex || hasImg || hasBufArr) {
            // The (single, v1) Sampler param supplies the filter/address modes.
            int32_t filterMode = CAJ_HIP_FILTER_LINEAR, addressMode = 0;
            for (int i = 0; i < kp->count; ++i)
                if (kp->kind[i] == CAJETA_KP_SAMPLER) {
                    const int32_t* modes = (const int32_t*) argv[i];
                    filterMode = modes[0]; addressMode = modes[1];
                    break;
                }
            for (int i = 0; i < kp->count; ++i) {
                subArgv[i] = argv[i];
                if (kp->kind[i] == CAJETA_KP_TEXTURE) {
                    if (ntex >= 8) {   // M6: more textures than the texObj buffers
                        fprintf(stderr, "cajeta.xpu: HIP kernel '%s' uses more than "
                                "8 textures (unsupported); not launching\n", kernelName);
                        launchOk = 0; break;
                    }
                    int64_t rec = *(int64_t*) argv[i];   // texture-record handle
                    int64_t obj = cajeta_xpu_hip_make_texobj(rec, filterMode,
                                                             addressMode);
                    if (!obj) {        // M5: texture-object creation failed
                        fprintf(stderr, "cajeta.xpu: HIP texture-object creation "
                                "failed for kernel '%s'; not launching\n", kernelName);
                        launchOk = 0; break;
                    }
                    texObjs[ntex] = obj;
                    texObjEmu[ntex] =
                        ((struct cajeta_hip_tex*) (intptr_t) rec)->emulated;
                    texObjVals[ntex] = (void*) (intptr_t) obj;
                    subArgv[i] = &texObjVals[ntex];      // arg = the texObj ptr
                    ++ntex;
                } else if (kp->kind[i] == CAJETA_KP_IMAGE) {
                    if (nsurf >= 8) {  // more storage images than the surfObj buffers
                        fprintf(stderr, "cajeta.xpu: HIP kernel '%s' uses more than "
                                "8 storage images (unsupported); not launching\n",
                                kernelName);
                        launchOk = 0; break;
                    }
                    int64_t rec = *(int64_t*) argv[i];   // image-record handle
                    int64_t obj = cajeta_xpu_hip_make_surfobj(rec);
                    if (!obj) {        // surface-object creation failed/unsupported
                        fprintf(stderr, "cajeta.xpu: HIP surface-object creation "
                                "failed for kernel '%s'; not launching\n", kernelName);
                        launchOk = 0; break;
                    }
                    surfObjs[nsurf] = obj;
                    surfObjVals[nsurf] = (void*) (intptr_t) obj;
                    subArgv[i] = &surfObjVals[nsurf];    // arg = the surfObj ptr
                    ++nsurf;
                } else if (kp->kind[i] == CAJETA_KP_BUFFER_ARRAY) {
                    // Bindless Buffer<T>[]: argv[i] points at the HOST-marshalled
                    // [i64 count, i64 h0 … ] handle array. The device kernel takes a
                    // global pointer to it (the default bufferArrayElement flat-loads
                    // each handle, which is itself a device address). Copy the array
                    // into device memory and pass &devPtr as the kernarg.
                    if (nbufarr >= 8) {
                        fprintf(stderr, "cajeta.xpu: HIP kernel '%s' uses more than "
                                "8 bindless buffer arrays (unsupported); not "
                                "launching\n", kernelName);
                        launchOk = 0; break;
                    }
                    const int64_t* hostArr = (const int64_t*) argv[i];
                    int64_t cnt = hostArr ? hostArr[0] : -1;
                    if (cnt < 0 || cnt > 16) {   // 16 = kMaxBindlessBuffers (host cap)
                        fprintf(stderr, "cajeta.xpu: HIP kernel '%s' bindless buffer-"
                                "array count %lld out of range; not launching\n",
                                kernelName, (long long) cnt);
                        launchOk = 0; break;
                    }
                    size_t bytes = (size_t) (cnt + 1) * sizeof(int64_t);
                    void* dev = NULL;
                    if (g_xpu_hip.hipMalloc(&dev, bytes) != 0 || !dev) {
                        fprintf(stderr, "cajeta.xpu: HIP bindless buffer-array device "
                                "alloc failed for kernel '%s'; not launching\n",
                                kernelName);
                        launchOk = 0; break;
                    }
                    if (g_xpu_hip.hipMemcpyHtoD(dev, hostArr, bytes) != 0) {
                        g_xpu_hip.hipFree(dev);
                        fprintf(stderr, "cajeta.xpu: HIP bindless buffer-array upload "
                                "failed for kernel '%s'; not launching\n", kernelName);
                        launchOk = 0; break;
                    }
                    bufArrDev[nbufarr] = dev;
                    bufArrVals[nbufarr] = dev;           // the device-array address
                    subArgv[i] = &bufArrVals[nbufarr];   // kernarg = &devPtr
                    ++nbufarr;
                }
            }
            useArgv = subArgv;
        }
    }

    if (launchOk)
        g_xpu_hip.hipModuleLaunchKernel(fn, (unsigned) gridX, (unsigned) gridY,
                                        (unsigned) gridZ, (unsigned) blockX,
                                        (unsigned) blockY, (unsigned) blockZ,
                                        (unsigned) sharedBytes,
                                        /*stream=*/(void*) (intptr_t) streamHandle,
                                        useArgv, /*extra=*/NULL);
    if (ntex > 0 || nsurf > 0 || nbufarr > 0) {
        if (launchOk)
            g_xpu_hip.hipDeviceSynchronize();   // finish before freeing resources
        for (int i = 0; i < ntex; ++i)          // also frees objs made before a skip
            if (texObjs[i] && !texObjEmu[i] && g_xpu_hip.hipDestroyTextureObject)
                g_xpu_hip.hipDestroyTextureObject((void*) (intptr_t) texObjs[i]);
        for (int i = 0; i < nsurf; ++i)
            if (surfObjs[i] && g_xpu_hip.hipDestroySurfaceObject)
                g_xpu_hip.hipDestroySurfaceObject((void*) (intptr_t) surfObjs[i]);
        for (int i = 0; i < nbufarr; ++i)       // free the device handle-array copies
            if (bufArrDev[i] && g_xpu_hip.hipFree)
                g_xpu_hip.hipFree(bufArrDev[i]);
    }
}

// Vulkan launch: translate the uniform kernelParams argv into descriptor
// bindings using the per-kernel param metadata — buffer args map to their
// existing storage buffers (argv slot holds the buffer-table handle), scalar
// args are copied into transient single-element SSBOs (freed after) — then
// dispatch gridX work-groups (the local size is baked into the SPIR-V). This is
// the one backend whose launch ABI forks from the pointer-arg kernelParams
// model: Vulkan's compute entry has no params, only descriptor bindings.
static void cajeta_xpu_launch_vulkan(const char* kernelName,
                                     int32_t gridX, int32_t gridY, int32_t gridZ,
                                     int32_t blockX, int32_t blockY, int32_t blockZ,
                                     int32_t sharedBytes, void* argvv,
                                     int32_t specCount, const int32_t* specValues) {
    void** argv = (void**) argvv;
    // kparams are shared across variants (looked up by the base name); the launch
    // resolves the AS bind kind from the recorded impl below.
    struct cajeta_kparams* kp = cajeta_xpu_find_kparams(kernelName);
    if (!kp || kp->count <= 0 || kp->count > 64) {
        fprintf(stderr,
                "cajeta.xpu: missing/invalid parameter metadata for Vulkan "
                "kernel '%s'\n", kernelName);
        return;
    }
    const int n = kp->count;

    // Variant selection (inc-4 brick #3): if an AccelerationStructure argument was
    // built as a software BVH, launch the "<name>$sw" variant — the SoftwareRayQuery
    // walk in plain SPIR-V, AS bound as a storage buffer — instead of the native
    // module. The impl is recorded at the AS POD's offset 12 ({i64 handle, u32
    // count, i32 impl}). v1: AS args in one launch share one impl.
    int asSoftware = 0;
    for (int i = 0; i < n; ++i) {
        if (kp->kind[i] == CAJETA_KP_ACCEL &&
            ((const int32_t*) argv[i])[3] == CAJ_AS_IMPL_SOFTWARE_BVH) {
            asSoftware = 1;
            break;
        }
    }
    char variantName[128];
    const char* launchName = kernelName;
    if (asSoftware) {
        snprintf(variantName, sizeof(variantName), "%s$sw", kernelName);
        launchName = variantName;
    }

    pthread_mutex_lock(&g_xpu_cuda_lock);
    struct cajeta_xpu_module* e = cajeta_xpu_find_module(launchName);
    const void* spirv = e ? e->image : NULL;
    uint64_t len = e ? e->len : 0;
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    if (!spirv || len < 4) {
        fprintf(stderr,
                "cajeta.xpu: no registered SPIR-V kernel '%s' to launch\n",
                launchName);
        return;
    }
    int64_t bindings[64];
    uint8_t bkinds[64];                     // per-binding resource kind
    int64_t transient[64];                  // transient scalar SSBOs to free
    int64_t samplers[64];                   // transient VkSamplers (as int64)
    int ntrans = 0, nsamp = 0;
    int built = 1;
    for (int i = 0; i < n; ++i) {
        switch (kp->kind[i]) {
            case CAJETA_KP_BUFFER:
                bindings[i] = *(int64_t*) argv[i];    // existing storage buffer
                bkinds[i] = CAJ_VKB_BUFFER;
                break;
            case CAJETA_KP_BUFFER_ARRAY:
                // argv[i] points at the marshalled [int64 count, int64 h0 …]
                // handle array; pass that pointer through to the descriptor-array
                // write (which reads the count + handles).
                bindings[i] = (int64_t) (intptr_t) argv[i];
                bkinds[i] = CAJ_VKB_BUFFER_ARRAY;
                break;
            case CAJETA_KP_TEXTURE:
                // argv slot holds the Texture2D deviceHandle = texture-table index.
                bindings[i] = *(int64_t*) argv[i];
                bkinds[i] = CAJ_VKB_TEXTURE;
                break;
            case CAJETA_KP_IMAGE:
                // argv slot holds the Image2D deviceHandle = texture-table index
                // (storage image). Bind it as a STORAGE_IMAGE (GENERAL layout).
                bindings[i] = *(int64_t*) argv[i];
                bkinds[i] = CAJ_VKB_STORAGE_IMAGE;
                break;
            case CAJETA_KP_ACCEL: {
                // argv slot points at the AccelerationStructure POD:
                // { i64 deviceHandle, u32 primitiveCount, i32 impl }. The first
                // field is the handle; the bind kind follows the noun's RECORDED
                // impl (the matching verb variant was already selected above):
                // native BLAS → an acceleration-structure descriptor; software
                // BVH → the storage buffer the "$sw" variant reads as bvh[i].
                int32_t asImpl = ((const int32_t*) argv[i])[3];
                bindings[i] = *(int64_t*) argv[i];
                bkinds[i] = (asImpl == CAJ_AS_IMPL_SOFTWARE_BVH) ? CAJ_VKB_BUFFER
                                                                 : CAJ_VKB_ACCEL;
                break;
            }
            case CAJETA_KP_SAMPLER: {
                // argv slot points at the by-value Sampler POD: { i32 filterMode,
                // i32 addressMode }. Build a transient VkSampler from it.
                const int32_t* modes = (const int32_t*) argv[i];
                int64_t s = cajeta_xpu_vk_make_sampler(modes[0], modes[1]);
                if (!s) { built = 0; break; }
                bindings[i] = s;
                bkinds[i] = CAJ_VKB_SAMPLER;
                samplers[nsamp++] = s;
                break;
            }
            default: {   // scalar by value -> transient single-element SSBO
                uint32_t sz = kp->byteSize[i] ? kp->byteSize[i] : 4u;
                int64_t h = cajeta_xpu_vk_alloc(sz);
                if (!h) { built = 0; break; }
                void* m = cajeta_xpu_vk_mapped(h);
                if (m) memcpy(m, argv[i], sz);
                bindings[i] = h;
                bkinds[i] = CAJ_VKB_BUFFER;
                transient[ntrans++] = h;
                break;
            }
        }
        if (!built) break;
    }
    if (built)
        cajeta_xpu_vk_launch(spirv, len, launchName, bindings, bkinds, n,
                             (unsigned) gridX, (unsigned) gridY, (unsigned) gridZ,
                             (unsigned) (blockX > 0 ? blockX : 1),
                             (unsigned) (blockY > 0 ? blockY : 1),
                             (unsigned) (blockZ > 0 ? blockZ : 1),
                             (unsigned) (sharedBytes > 0 ? sharedBytes : 0),
                             (int) specCount, specValues);
    for (int i = 0; i < ntrans; ++i) cajeta_xpu_vk_free(transient[i]);
    for (int i = 0; i < nsamp; ++i) cajeta_xpu_vk_destroy_sampler(samplers[i]);
}

// Dispatch a launch to whatever backend is active, on its current device.
// `specCount`/`specValues` are host overrides for the kernel's user spec
// constants (NULL/0 = none); see __cajeta_xpu_launch_v3.
static void caj_xpu_dispatch(const char* kernelName,
                             int32_t gridX, int32_t gridY, int32_t gridZ,
                             int32_t blockX, int32_t blockY, int32_t blockZ,
                             uint32_t sharedBytes, void* argv,
                             int64_t streamHandle,
                             int32_t specCount, const int32_t* specValues) {
    int backend = cajeta_xpu_active_backend();
    switch (backend) {
        case CAJ_XPU_CUDA:
            // streamHandle (0 = default stream) orders this launch with the
            // async copies queued on the same stream. Spec override → the
            // module's constant-memory globals (Phase C).
            cajeta_xpu_launch_cuda(kernelName, gridX, gridY, gridZ,
                                   blockX, blockY, blockZ, sharedBytes, argv,
                                   streamHandle, specCount, specValues);
            return;
        case CAJ_XPU_HIP:
            cajeta_xpu_launch_hip(kernelName, gridX, gridY, gridZ,
                                  blockX, blockY, blockZ, sharedBytes, argv,
                                  streamHandle, specCount, specValues);
            return;
        case CAJ_XPU_VULKAN:
            // Vulkan v1 submits on its own queue; per-stream ordering is a
            // follow-on (cajeta-xpu). The stream handle is accepted, not used.
            (void) streamHandle;
            cajeta_xpu_launch_vulkan(kernelName, gridX, gridY, gridZ,
                                     blockX, blockY, blockZ,
                                     (int32_t) sharedBytes, argv,
                                     specCount, specValues);
            return;
        case CAJ_XPU_CPU:
            // CPU launches run synchronously; the stream is ordering-irrelevant.
            // CPU honors a spec override by reading it at runtime (hybrid).
            (void) streamHandle;
            cajeta_xpu_launch_cpu(kernelName, gridX, gridY, gridZ,
                                  blockX, blockY, blockZ,
                                  (int32_t) sharedBytes, argv,
                                  specCount, specValues);
            return;
        default: return;   // none: diagnostic emitted
    }
}

// How many devices the given backend exposes (>= 1). Best-effort; falls back to
// 1 if the count can't be queried. The index space the launch `deviceId` selects
// within (handles originate from cajeta-gpu enumeration; xpu consumes the index).
static int caj_xpu_device_count(int backend) {
    switch (backend) {
        case CAJ_XPU_HIP: {
            int c = 0;
            if (g_xpu_hip.hipGetDeviceCount &&
                g_xpu_hip.hipGetDeviceCount(&c) == 0 && c > 0) return c;
            return 1;
        }
        case CAJ_XPU_CUDA: {
            int c = 0;
            if (g_xpu_cuda.cuDeviceGetCount &&
                g_xpu_cuda.cuDeviceGetCount(&c) == 0 && c > 0) return c;
            return 1;
        }
        case CAJ_XPU_VULKAN: {
            uint32_t c = 0;
            if (g_xpu_vk.vkEnumeratePhysicalDevices && g_xpu_vk.instance &&
                g_xpu_vk.vkEnumeratePhysicalDevices(g_xpu_vk.instance, &c, NULL)
                    == VK_SUCCESS && c > 0) return (int) c;
            return 1;
        }
        case CAJ_XPU_CPU:
        default:
            return 1;
    }
}

// The versioned host-source launch entry point (ABI v1): dispatch to the active
// backend (chosen + cached on first device touch). `deviceId` selects the target
// device — -1 = the current active device (no targeting; the pre-Stage-12
// behavior); >= 0 = an index into the active backend's enumerated devices. An
// out-of-range index is a defined no-op (a diagnostic, never UB). v1 targets
// "where it is cheap + correct": deviceId 0 is the default device on every
// backend, and HIP genuinely selects deviceId>0 via hipSetDevice (the caller
// owns buffer affinity — buffers must already live on the target device; no
// migration here). Multi-device >0 on CUDA/Vulkan needs per-device contexts not
// yet built and is a defined "unsupported", not a silent wrong-device launch.
// A future field is added as __cajeta_xpu_launch_v4, never by repurposing an arg.
// `specCount`/`specValues` are host overrides for the kernel's user
// specialization constants (NULL/0 = none); see the header for the slot→SpecId
// mapping and per-backend honoring.
void __cajeta_xpu_launch_v3(const char* kernelName,
                            int32_t gridX, int32_t gridY, int32_t gridZ,
                            int32_t blockX, int32_t blockY, int32_t blockZ,
                            uint32_t sharedBytes, void* argv,
                            int64_t streamHandle, int32_t deviceId,
                            int32_t specCount, const int32_t* specValues) {
    if (!kernelName) return;
    if (specCount < 0 || !specValues) specCount = 0;

    if (deviceId >= 0) {
        int backend = cajeta_xpu_active_backend();
        int count = caj_xpu_device_count(backend);
        if (deviceId >= count) {
            fprintf(stderr,
                    "cajeta.xpu: launch deviceId %d out of range (%s exposes "
                    "%d device%s); launch skipped\n",
                    deviceId, cajeta_xpu_backend_name(backend), count,
                    count == 1 ? "" : "s");
            return;   // defined no-op, no UB
        }
        if (deviceId > 0) {
            if (backend == CAJ_XPU_HIP && g_xpu_hip.hipSetDevice) {
                // Bind the target device for this launch, then restore the
                // runtime's default so subsequent (deviceId<=0) launches and
                // buffer ops keep landing on the default device.
                int prev = g_xpu_hip.device;
                g_xpu_hip.hipSetDevice(deviceId);
                caj_xpu_dispatch(kernelName, gridX, gridY, gridZ,
                                 blockX, blockY, blockZ, sharedBytes, argv,
                                 streamHandle, specCount, specValues);
                g_xpu_hip.hipSetDevice(prev);
                return;
            }
            fprintf(stderr,
                    "cajeta.xpu: per-launch targeting to deviceId %d not yet "
                    "implemented for backend %s (only deviceId 0/-1); launch "
                    "skipped\n",
                    deviceId, cajeta_xpu_backend_name(backend));
            return;
        }
        // deviceId == 0 falls through: the default device on every backend.
    }

    caj_xpu_dispatch(kernelName, gridX, gridY, gridZ,
                     blockX, blockY, blockZ, sharedBytes, argv, streamHandle,
                     specCount, specValues);
}

// Compat shim (ABI v2): no spec override. Frozen signature.
void __cajeta_xpu_launch_v2(const char* kernelName,
                            int32_t gridX, int32_t gridY, int32_t gridZ,
                            int32_t blockX, int32_t blockY, int32_t blockZ,
                            uint32_t sharedBytes, void* argv,
                            int64_t streamHandle, int32_t deviceId) {
    __cajeta_xpu_launch_v3(kernelName, gridX, gridY, gridZ,
                           blockX, blockY, blockZ, sharedBytes, argv,
                           streamHandle, deviceId, /*specCount=*/0, /*specValues=*/NULL);
}

// Backward-compat shim — the original positional entry point. Forwards to v2
// with deviceId = -1 (the active device). Frozen: keep this signature stable.
void __cajeta_xpu_launch(const char* kernelName,
                         int32_t gridX, int32_t gridY, int32_t gridZ,
                         int32_t blockX, int32_t blockY, int32_t blockZ,
                         uint32_t sharedBytes, void* argv, int64_t streamHandle) {
    __cajeta_xpu_launch_v2(kernelName, gridX, gridY, gridZ,
                           blockX, blockY, blockZ, sharedBytes, argv,
                           streamHandle, /*deviceId=*/-1);
}

// Register a kernel's compiled cubin image under its PTX entry name. The
// device-cubin pass emits a module global constructor that calls this; the
// launch path (above) loads the CUDA module + resolves the function lazily on
// first use. The image pointer lives in the host module's constant data and
// stays valid for the process lifetime.
void __cajeta_xpu_register_module(const char* kernelName, const void* image,
                                  uint64_t len) {
    if (!kernelName || !image) return;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    // Dedup by name, OVERWRITING on re-registration (mirrors the kparams registry).
    // A second JIT'd program in the same process that reuses a kernel name (the
    // test suite; any multi-program JIT host) MUST adopt the NEW image: the old
    // one was embedded in the first program's module and is freed when that JIT is
    // torn down, so keeping the stale pointer is a use-after-free at the next
    // cuModuleLoadData (wrong kernel / crash). Resetting module+function forces a
    // reload from the live image. (The old CUmodule/hipModule handle is leaked;
    // re-registration is rare and the alternative — unloading without the owning
    // backend context here — is unsafe.)
    struct cajeta_xpu_module* e = cajeta_xpu_find_module(kernelName);
    if (!e && g_xpu_module_count < CAJETA_XPU_MAX_MODULES) {
        e = &g_xpu_modules[g_xpu_module_count++];
        strncpy(e->name, kernelName, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
    }
    if (e) {
        e->image = image;
        e->len = len;
        e->module = NULL;
        e->function = NULL;
    }
    pthread_mutex_unlock(&g_xpu_cuda_lock);
}

