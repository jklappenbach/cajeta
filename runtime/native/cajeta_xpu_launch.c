// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
// --- Texture3D CPU sample/fetch ---------------------------------------------
// One voxel of the DECODED float volume: ((z*h + y)*w + x)*channels; absent G/B = 0, A = 1.
static inline caj_v4f cajeta_cpu_texel3d(const struct cajeta_cpu_texobj* t,
                                         int x, int y, int z) {
    const float* p = t->data +
        (((size_t) z * t->h + (size_t) y) * t->w + (size_t) x) * t->channels;
    caj_v4f c = { 0.0f, 0.0f, 0.0f, 1.0f };
    for (int i = 0; i < t->channels; ++i) c[i] = p[i];
    return c;
}

// CPU 3-D sampler — the lowering of `tex.sample(sampler, u, v, w)`, (u, v, w) in [0, 1].
// filterMode 0 = nearest / 1 = trilinear (texel-center); addressMode 0 = clamp / 1 = wrap.
caj_v4f __cajeta_xpu_cpu_tex3d_sample_rgba(void* texp, int32_t filterMode,
                                           int32_t addressMode, float u, float v,
                                           float w) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4f zero = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!t || !t->data || t->w == 0 || t->h == 0 || t->d == 0) return zero;
    int W = (int) t->w, H = (int) t->h, D = (int) t->d;
    if (filterMode == 0) {
        int x = cajeta_tex_addr((int) floorf(u * (float) W), W, addressMode);
        int y = cajeta_tex_addr((int) floorf(v * (float) H), H, addressMode);
        int z = cajeta_tex_addr((int) floorf(w * (float) D), D, addressMode);
        return cajeta_cpu_texel3d(t, x, y, z);
    }
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
// The lowering of `arr.sample(sampler, u, v, layer)`: `layer` is the clamped integer
// index, (u, v) normalized. Filtering is bilinear WITHIN a layer, never across layers.
caj_v4f __cajeta_xpu_cpu_tex2da_sample_rgba(void* texp, int32_t filterMode,
                                            int32_t addressMode, float u, float v,
                                            int32_t layer) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4f zero = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!t || !t->data || t->w == 0 || t->h == 0 || t->d == 0) return zero;
    int W = (int) t->w, H = (int) t->h, D = (int) t->d;
    int z = layer < 0 ? 0 : (layer >= D ? D - 1 : layer);
    if (filterMode == 0) {
        int x = cajeta_tex_addr((int) floorf(u * (float) W), W, addressMode);
        int y = cajeta_tex_addr((int) floorf(v * (float) H), H, addressMode);
        return cajeta_cpu_texel3d(t, x, y, z);
    }
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
// The lowering of `cube.sample(sampler, x, y, z)`: sample by DIRECTION (need not be
// normalized). Faces are z slices in +X,-X,+Y,-Y,+Z,-Z order, picked by major axis.
caj_v4f __cajeta_xpu_cpu_texcube_sample_rgba(void* texp, int32_t filterMode,
                                             int32_t addressMode, float x, float y,
                                             float z) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4f zero = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!t || !t->data || t->w == 0 || t->h == 0 || t->d < 6) return zero;
    float ax = fabsf(x), ay = fabsf(y), az = fabsf(z);
    int face; float sc, tc, ma;
    if (ax >= ay && ax >= az) {
        ma = ax;
        if (x >= 0.0f) { face = 0; sc = -z; tc = -y; }
        else           { face = 1; sc =  z; tc = -y; }
    } else if (ay >= ax && ay >= az) {
        ma = ay;
        if (y >= 0.0f) { face = 2; sc =  x; tc =  z; }
        else           { face = 3; sc =  x; tc = -z; }
    } else {
        ma = az;
        if (z >= 0.0f) { face = 4; sc =  x; tc = -y; }
        else           { face = 5; sc = -x; tc = -y; }
    }
    if (ma == 0.0f) ma = 1.0f;
    float u = 0.5f * (sc / ma + 1.0f);
    float v = 0.5f * (tc / ma + 1.0f);
    int W = (int) t->w, H = (int) t->h;
    if (filterMode == 0) {
        int xi = cajeta_tex_addr((int) floorf(u * (float) W), W, addressMode);
        int yi = cajeta_tex_addr((int) floorf(v * (float) H), H, addressMode);
        return cajeta_cpu_texel3d(t, xi, yi, face);
    }
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
// Marshal argv into the OptiX launch params (layout contract in NvptxOptixRayQuery.h)
// and run the RT-core pipeline. `optixHandle` keys the traversable/boxes lookups.
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
        // Triangle nearest-hit: argv [AS, outT, outI].
        struct { uint64_t handle, outT, outI; } p;
        p.handle = trav;
        p.outT   = *(uint64_t*) av[1];
        p.outI   = *(uint64_t*) av[2];
        rc = cajeta_xpu_optix_launch_tri(rq->ptx, rq->ptxLen, rq->raygen,
                                         rq->prog1 /*closesthit*/, "" /*anyhit*/,
                                         rq->prog2 /*miss*/, &p, sizeof(p), 1);
    } else if (rq->shape == 2) {
        // Triangle candidate getters: argv [AS, out].
        struct { uint64_t handle, out; } p;
        p.handle = trav;
        p.out    = *(uint64_t*) av[1];
        rc = cajeta_xpu_optix_launch_tri(rq->ptx, rq->ptxLen, rq->raygen,
                                         "" /*closesthit*/, rq->prog1 /*anyhit*/,
                                         rq->prog2 /*miss*/, &p, sizeof(p), 1);
    } else if (rq->shape == 3) {
        // Committed-triangle per-launch: argv [AS, b0, b1, out, n], one ray per index.
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
        // AABB candidate count: argv [AS, ox, oy, oz, out, n]; `boxes` is retained AS data.
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

// CUDA launch: pick the OptiX or software path from the AS argument's impl, load the
// module lazily, apply spec overrides, translate kernargs, launch, then free them.
static void cajeta_xpu_launch_cuda(const char* kernelName,
                                   int32_t gridX, int32_t gridY, int32_t gridZ,
                                   int32_t blockX, int32_t blockY, int32_t blockZ,
                                   uint32_t sharedBytes, void* argv,
                                   int64_t streamHandle,
                                   int32_t specCount, const int32_t* specValues) {
    // Read the ACTUAL AS argument's impl (POD offset 12), not a global resolve.
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
    struct cajeta_xpu_module* e = cajeta_xpu_find_module(kernelName, CAJ_XPU_CUDA);
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
    // An unbacked (0) texture/image handle would feed tex/suld a null object and FAULT.
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
    // Bound to its creating thread: a launch elsewhere finds no context and no-ops.
    if (g_xpu_cuda.cuCtxSetCurrent) g_xpu_cuda.cuCtxSetCurrent(g_xpu_cuda.ctx);
    // The kernel reads `(slot < count) ? values[slot] : default`, so count 0 clears.
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
    // Per-launch kernarg translation: TEXTURE → a CUtexObject built from the record
    // + the bound Sampler's modes; IMAGE → a CUsurfObject; BUFFER_ARRAY → a device
    // copy of the host [count, h…] array, passed as &devPtr. Others pass through.
    void** useArgv = (void**) argv;
    void* subArgv[64];
    void* bufArrVals[8];
    cajeta_cudeviceptr bufArrDev[8];
    int nbufarr = 0;
    unsigned long long texObjs[8]; void* texObjVals[8]; int ntex = 0;
    unsigned long long surfObjs[8]; void* surfObjVals[8]; int nsurf = 0;
    // AS PODs substituted for the software-floor fallback. Layout {i64 handle, u32
    // count, i32 impl} = 16 bytes, matching the struct the cubin reads by value.
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
                    hasXlat = 1;
                }
            }
            if (hasXlat) {
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
                        // Field 0 is read as the BVH-blob pointer: point at the floor.
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
    // The EVENT-tier bracket sits on the launch's OWN stream and never waits.
    caj_cuda_bracket_drain();
    const int profSlot = caj_cuda_bracket_begin(
        __cajeta_prof_cuda_current_launch(), (void*) (intptr_t) streamHandle);
    int launchRc = g_xpu_cuda.cuLaunchKernel(
        fn, (unsigned) gridX, (unsigned) gridY, (unsigned) gridZ,
        (unsigned) blockX, (unsigned) blockY, (unsigned) blockZ,
        (unsigned) sharedBytes, /*stream=*/(void*) (intptr_t) streamHandle,
        useArgv, /*extra=*/NULL);
    caj_cuda_bracket_end(profSlot, (void*) (intptr_t) streamHandle);
    // Sync before freeing: the launch is async and still reads these resources.
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

// HIP launch: mirrors cajeta_xpu_launch_cuda with hip* entry points, over the shared
// module table. Texture/image objects are built here and destroyed after the launch.
static void cajeta_xpu_launch_hip(const char* kernelName,
                                  int32_t gridX, int32_t gridY, int32_t gridZ,
                                  int32_t blockX, int32_t blockY, int32_t blockZ,
                                  uint32_t sharedBytes, void* argvv,
                                  int64_t streamHandle,
                                  int32_t specCount, const int32_t* specValues) {
    pthread_mutex_lock(&g_xpu_cuda_lock);
    struct cajeta_xpu_module* e = cajeta_xpu_find_module(kernelName, CAJ_XPU_HIP);
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
    // Spec override: count 0 clears a prior launch; an absent symbol means none.
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

    // Texture2D binds as a sampled texture object, Image2D as a writable surface one;
    // both reach the kernel as a ptr-addrspace(4) kernarg, so &objVal fills the slot.
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
                    if (ntex >= 8) {
                        fprintf(stderr, "cajeta.xpu: HIP kernel '%s' uses more than "
                                "8 textures (unsupported); not launching\n", kernelName);
                        launchOk = 0; break;
                    }
                    int64_t rec = *(int64_t*) argv[i];   // texture-record handle
                    int64_t obj = cajeta_xpu_hip_make_texobj(rec, filterMode,
                                                             addressMode);
                    if (!obj) {
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
                    if (nsurf >= 8) {
                        fprintf(stderr, "cajeta.xpu: HIP kernel '%s' uses more than "
                                "8 storage images (unsupported); not launching\n",
                                kernelName);
                        launchOk = 0; break;
                    }
                    int64_t rec = *(int64_t*) argv[i];   // image-record handle
                    int64_t obj = cajeta_xpu_hip_make_surfobj(rec);
                    if (!obj) {
                        fprintf(stderr, "cajeta.xpu: HIP surface-object creation "
                                "failed for kernel '%s'; not launching\n", kernelName);
                        launchOk = 0; break;
                    }
                    surfObjs[nsurf] = obj;
                    surfObjVals[nsurf] = (void*) (intptr_t) obj;
                    subArgv[i] = &surfObjVals[nsurf];    // arg = the surfObj ptr
                    ++nsurf;
                } else if (kp->kind[i] == CAJETA_KP_BUFFER_ARRAY) {
                    // argv[i] is the HOST [i64 count, i64 h0 …] array; the kernel
                    // flat-loads device addresses out of a device copy of it.
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

// Vulkan launch: translate argv into descriptor bindings — buffers to their storage
// buffers, scalars to transient SSBOs — then dispatch. Vulkan's entry takes no params.
static void cajeta_xpu_launch_vulkan(const char* kernelName,
                                     int32_t gridX, int32_t gridY, int32_t gridZ,
                                     int32_t blockX, int32_t blockY, int32_t blockZ,
                                     int32_t sharedBytes, void* argvv,
                                     int32_t specCount, const int32_t* specValues) {
    void** argv = (void**) argvv;
    // kparams are shared across variants, looked up by the base name.
    struct cajeta_kparams* kp = cajeta_xpu_find_kparams(kernelName);
    if (!kp || kp->count <= 0 || kp->count > 64) {
        fprintf(stderr,
                "cajeta.xpu: missing/invalid parameter metadata for Vulkan "
                "kernel '%s'\n", kernelName);
        return;
    }
    const int n = kp->count;

    // A software-BVH AS launches the "<name>$sw" variant, AS bound as a storage buffer.
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
    struct cajeta_xpu_module* e = cajeta_xpu_find_module(launchName, CAJ_XPU_VULKAN);
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
    int64_t transient[64];                  // transient scalar view slots to free
    int64_t samplers[64];                   // transient VkSamplers (as int64)
    int ntrans = 0, nsamp = 0;
    int built = 1;
    cajeta_xpu_vk_scalar_begin_launch();    // arena headroom for this launch
    for (int i = 0; i < n; ++i) {
        switch (kp->kind[i]) {
            case CAJETA_KP_BUFFER:
                bindings[i] = *(int64_t*) argv[i];    // existing storage buffer
                bkinds[i] = CAJ_VKB_BUFFER;
                break;
            case CAJETA_KP_BUFFER_ARRAY:
                // argv[i] is the marshalled [int64 count, int64 h0 …] array itself.
                bindings[i] = (int64_t) (intptr_t) argv[i];
                bkinds[i] = CAJ_VKB_BUFFER_ARRAY;
                break;
            case CAJETA_KP_TEXTURE:
                // argv slot holds the Texture2D deviceHandle = texture-table index.
                bindings[i] = *(int64_t*) argv[i];
                bkinds[i] = CAJ_VKB_TEXTURE;
                break;
            case CAJETA_KP_IMAGE:
                // argv slot holds the Image2D deviceHandle = texture-table index.
                bindings[i] = *(int64_t*) argv[i];
                bkinds[i] = CAJ_VKB_STORAGE_IMAGE;
                break;
            case CAJETA_KP_ACCEL: {
                // argv slot points at the AS POD { i64 deviceHandle, u32 primitiveCount,
                // i32 impl }: native BLAS binds an acceleration-structure descriptor,
                // software BVH the storage buffer "$sw" reads as bvh[i].
                int32_t asImpl = ((const int32_t*) argv[i])[3];
                bindings[i] = *(int64_t*) argv[i];
                bkinds[i] = (asImpl == CAJ_AS_IMPL_SOFTWARE_BVH) ? CAJ_VKB_BUFFER
                                                                 : CAJ_VKB_ACCEL;
                break;
            }
            case CAJETA_KP_SAMPLER: {
                // argv slot points at the by-value Sampler POD { i32 filter, i32 address }.
                const int32_t* modes = (const int32_t*) argv[i];
                int64_t s = cajeta_xpu_vk_make_sampler(modes[0], modes[1]);
                if (!s) { built = 0; break; }
                bindings[i] = s;
                bkinds[i] = CAJ_VKB_SAMPLER;
                samplers[nsamp++] = s;
                break;
            }
            default: {   // scalar by value -> a slot in the persistent arena
                uint32_t sz = kp->byteSize[i] ? kp->byteSize[i] : 4u;
                int64_t h = cajeta_xpu_vk_scalar_push(argv[i], sz);
                if (!h) { built = 0; break; }
                bindings[i] = h;
                bkinds[i] = CAJ_VKB_BUFFER;
                transient[ntrans++] = h;
                break;
            }
        }
        if (!built) break;
    }
    if (!built)
        cajeta_xpu_note_launch_failure();
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
static void caj_xpu_dispatch_raw(const char* kernelName,
                             int32_t gridX, int32_t gridY, int32_t gridZ,
                             int32_t blockX, int32_t blockY, int32_t blockZ,
                             uint32_t sharedBytes, void* argv,
                             int64_t streamHandle,
                             int32_t specCount, const int32_t* specValues) {
    int backend = cajeta_xpu_active_backend();
    switch (backend) {
        case CAJ_XPU_CUDA:
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
            // Vulkan submits on its own queue: the stream handle is accepted, not used.
            (void) streamHandle;
            cajeta_xpu_launch_vulkan(kernelName, gridX, gridY, gridZ,
                                     blockX, blockY, blockZ,
                                     (int32_t) sharedBytes, argv,
                                     specCount, specValues);
            return;
        case CAJ_XPU_CPU:
            // CPU launches run synchronously; the stream is ordering-irrelevant.
            (void) streamHandle;
            cajeta_xpu_launch_cpu(kernelName, gridX, gridY, gridZ,
                                  blockX, blockY, blockZ,
                                  (int32_t) sharedBytes, argv,
                                  specCount, specValues);
            return;
        default: return;   // none: diagnostic emitted
    }
}

// The profiler's dispatch-record seam lives HERE, the single point every launch
// passes through; a hook one level up would be two call sites, not one.
typedef struct {
    const char* kernelName;
    int32_t gridX, gridY, gridZ, blockX, blockY, blockZ;
    uint32_t sharedBytes;
    void* argv;
    int64_t streamHandle;
    int32_t specCount;
    const int32_t* specValues;
} CajXpuDispatchArgs;

// Trampoline that replays a recorded dispatch from the profiler's launch hook.
static void caj_xpu_dispatch_thunk(void* p) {
    CajXpuDispatchArgs* a = (CajXpuDispatchArgs*) p;
    caj_xpu_dispatch_raw(a->kernelName, a->gridX, a->gridY, a->gridZ,
                         a->blockX, a->blockY, a->blockZ, a->sharedBytes,
                         a->argv, a->streamHandle, a->specCount, a->specValues);
}

// Dispatch through the profiler seam: unarmed, the raw dispatch; armed, the thunk.
static void caj_xpu_dispatch(const char* kernelName,
                             int32_t gridX, int32_t gridY, int32_t gridZ,
                             int32_t blockX, int32_t blockY, int32_t blockZ,
                             uint32_t sharedBytes, void* argv,
                             int64_t streamHandle,
                             int32_t specCount, const int32_t* specValues,
                             int32_t deviceId) {
    if (__cajeta_prof_gpu_sink_count() == 0) {
        caj_xpu_dispatch_raw(kernelName, gridX, gridY, gridZ, blockX, blockY,
                             blockZ, sharedBytes, argv, streamHandle,
                             specCount, specValues);
        return;
    }
    CajXpuDispatchArgs a = { kernelName, gridX, gridY, gridZ, blockX, blockY,
                             blockZ, sharedBytes, argv, streamHandle,
                             specCount, specValues };
    __cajeta_prof_gpu_launch(kernelName, gridX, gridY, gridZ,
                             blockX, blockY, blockZ, sharedBytes, streamHandle,
                             deviceId, cajeta_xpu_active_backend(),
                             caj_xpu_dispatch_thunk, &a);
}

// How many devices the backend exposes (>= 1) — the space `deviceId` indexes. Best-effort.
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
#if defined(CAJETA_RT_HAS_VULKAN)
            uint32_t c = 0;
            if (g_xpu_vk.vkEnumeratePhysicalDevices && g_xpu_vk.instance &&
                g_xpu_vk.vkEnumeratePhysicalDevices(g_xpu_vk.instance, &c, NULL)
                    == VK_SUCCESS && c > 0) return (int) c;
#endif
            return 1;
        }
        case CAJ_XPU_CPU:
        default:
            return 1;
    }
}

// The versioned host-source launch entry point (ABI v3). `deviceId`: -1 = the active
// device, >= 0 = an index into the backend's devices, honored above 0 on HIP only;
// out of range or unsupported is a diagnosed no-op. Frozen: add a v4, never a field.
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
                // Restored below so later launches and buffer ops keep the default.
                int prev = g_xpu_hip.device;
                g_xpu_hip.hipSetDevice(deviceId);
                caj_xpu_dispatch(kernelName, gridX, gridY, gridZ,
                                 blockX, blockY, blockZ, sharedBytes, argv,
                                 streamHandle, specCount, specValues, deviceId);
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
                     specCount, specValues, deviceId);
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

// Backward-compat shim (the original entry point): v2 with deviceId = -1. Frozen.
void __cajeta_xpu_launch(const char* kernelName,
                         int32_t gridX, int32_t gridY, int32_t gridZ,
                         int32_t blockX, int32_t blockY, int32_t blockZ,
                         uint32_t sharedBytes, void* argv, int64_t streamHandle) {
    __cajeta_xpu_launch_v2(kernelName, gridX, gridY, gridZ,
                           blockX, blockY, blockZ, sharedBytes, argv,
                           streamHandle, /*deviceId=*/-1);
}

// Register a kernel's compiled device image under its entry name + backend id; the
// launch path resolves by (name, active backend) and loads lazily on first use.
static void cajeta_xpu_register_module_impl(const char* kernelName,
                                            const void* image, uint64_t len,
                                            int backend) {
    if (!kernelName || !image) return;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    // Re-registration OVERWRITES: the old image died with the JIT that embedded it.
    int i;
    struct cajeta_xpu_module* e = NULL;
    for (i = 0; i < g_xpu_module_count; i++) {
        if (g_xpu_modules[i].backend == backend &&
            strncmp(g_xpu_modules[i].name, kernelName,
                    sizeof(g_xpu_modules[i].name)) == 0) {
            e = &g_xpu_modules[i];
            break;
        }
    }
    if (!e && g_xpu_module_count < CAJETA_XPU_MAX_MODULES) {
        e = &g_xpu_modules[g_xpu_module_count++];
        strncpy(e->name, kernelName, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        e->backend = backend;
    }
    if (!e) {
        fprintf(stderr,
                "cajeta.xpu: kernel registry FULL (%d) — dropping '%s'; "
                "raise CAJETA_XPU_MAX_MODULES\n",
                CAJETA_XPU_MAX_MODULES, kernelName);
    }
    if (e) {
        e->image = image;
        e->len = len;
        e->module = NULL;
        e->function = NULL;
    }
    pthread_mutex_unlock(&g_xpu_cuda_lock);
}

// Backend-tagged registration — what the per-backend ctors emit.
void __cajeta_xpu_register_module_be(const char* kernelName, const void* image,
                                     uint64_t len, int32_t backend) {
    cajeta_xpu_register_module_impl(kernelName, image, len, (int) backend);
}

// Legacy entry point: registers as backend -1, which lookup treats as serves-any. Frozen.
void __cajeta_xpu_register_module(const char* kernelName, const void* image,
                                  uint64_t len) {
    cajeta_xpu_register_module_impl(kernelName, image, len, -1);
}

// --- kernel manifests (xpu-tile-manifest §12.1) ------------------------------
// One per (name, backend, arch): the manifest JSON embedded beside that device code.
struct cajeta_xpu_manifest {
    char name[256];
    int backend;
    char arch[64];
    const char* json;
    uint64_t len;
};
// Two per module slot: a multi-arch bundle registers one manifest per arch.
#define CAJETA_XPU_MAX_MANIFESTS (2 * CAJETA_XPU_MAX_MODULES)
static struct cajeta_xpu_manifest g_xpu_manifests[CAJETA_XPU_MAX_MANIFESTS];
static int g_xpu_manifest_count;

void* __cajeta_new_array_header(uint64_t header_size, uint64_t elem_size,
                                uint64_t count);

// Record one kernel's manifest JSON under (name, backend, arch), overwriting a repeat.
void __cajeta_xpu_register_kernel_manifest(const char* kernelName,
                                           int32_t backend, const char* arch,
                                           const void* json, uint64_t len) {
    if (!kernelName || !json) return;
    if (!arch) arch = "";
    pthread_mutex_lock(&g_xpu_cuda_lock);
    struct cajeta_xpu_manifest* e = NULL;
    int i;
    for (i = 0; i < g_xpu_manifest_count; i++) {
        if (g_xpu_manifests[i].backend == (int) backend &&
            strncmp(g_xpu_manifests[i].name, kernelName,
                    sizeof(g_xpu_manifests[i].name)) == 0 &&
            strncmp(g_xpu_manifests[i].arch, arch,
                    sizeof(g_xpu_manifests[i].arch)) == 0) {
            e = &g_xpu_manifests[i];
            break;
        }
    }
    if (!e && g_xpu_manifest_count < CAJETA_XPU_MAX_MANIFESTS) {
        e = &g_xpu_manifests[g_xpu_manifest_count++];
        strncpy(e->name, kernelName, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        strncpy(e->arch, arch, sizeof(e->arch) - 1);
        e->arch[sizeof(e->arch) - 1] = '\0';
        e->backend = (int) backend;
    }
    if (!e) {
        fprintf(stderr,
                "cajeta.xpu: kernel manifest registry FULL (%d) — dropping '%s'\n",
                CAJETA_XPU_MAX_MANIFESTS, kernelName);
    } else {
        e->json = (const char*) json;
        e->len = len;
    }
    pthread_mutex_unlock(&g_xpu_cuda_lock);
}

// The active backend's manifest JSON as a fresh cajeta int8[] the caller owns, or NULL.
// STATIC native (no `this`); `nameArr` is an int8[] whose payload starts at +8.
void* __cajeta_xpu_kernel_manifest_json(void* nameArr, int64_t len) {
    if (!nameArr || len <= 0 || len > 255) return NULL;
    char name[256];
    memcpy(name, (const char*) nameArr + 8, (size_t) len);
    name[len] = 0;
    int backend = cajeta_xpu_active_backend();
    if (backend < 0) return NULL;

    int i, candidates = 0;
    struct cajeta_xpu_manifest* pick = NULL;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    for (i = 0; i < g_xpu_manifest_count; i++) {
        if (g_xpu_manifests[i].backend == backend &&
            strncmp(g_xpu_manifests[i].name, name,
                    sizeof(g_xpu_manifests[i].name)) == 0) {
            if (!pick) pick = &g_xpu_manifests[i];
            candidates++;
        }
    }
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    if (!pick) return NULL;

    // The device's arch token may carry a suffix ("gfx1151:xnack-") — match a prefix.
    if (candidates > 1) {
        CajetaXpuRawDevice dev;
        if (cajeta_xpu_query_raw_device(&dev) && dev.archName[0]) {
            pthread_mutex_lock(&g_xpu_cuda_lock);
            for (i = 0; i < g_xpu_manifest_count; i++) {
                struct cajeta_xpu_manifest* e = &g_xpu_manifests[i];
                if (e->backend == backend && e->arch[0] &&
                    strncmp(e->name, name, sizeof(e->name)) == 0 &&
                    strncmp(e->arch, dev.archName, strlen(e->arch)) == 0) {
                    pick = e;
                    break;
                }
            }
            pthread_mutex_unlock(&g_xpu_cuda_lock);
        }
    }

    void* hdr = __cajeta_new_array_header(8, 1, pick->len);
    if (!hdr) return NULL;
    if (pick->len) memcpy((char*) hdr + 8, pick->json, (size_t) pick->len);
    return hdr;
}

// Is `kernelName` LAUNCHABLE on the active backend (device code registered, and on
// Vulkan its kparams too)? Lets a route degrade rather than issue a loud no-op.
// STATIC native (no `this`); `nameArr` is an int8[] whose payload starts at +8.
int32_t __cajeta_xpu_kernel_available(void* nameArr, int64_t len) {
    if (!nameArr || len <= 0 || len > 255) return 0;
    char name[256];
    memcpy(name, (const char*) nameArr + 8, (size_t) len);
    name[len] = 0;
    int backend = cajeta_xpu_active_backend();
    // Registry semantics differ: Vulkan registers one SPIR-V module PER KERNEL, while
    // HIP/CUDA register per-unit fatbins, resolve at launch, and compile every kernel
    // — so availability on those backends is unconditional.
    if (backend != CAJ_XPU_VULKAN) return 1;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    struct cajeta_xpu_module* e = cajeta_xpu_find_module(name, backend);
    int ok = (e && e->image && e->len >= 4) ? 1 : 0;
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    if (ok) {
        struct cajeta_kparams* kp = cajeta_xpu_find_kparams(name);
        if (!kp || kp->count <= 0 || kp->count > 64) ok = 0;
    }
    return ok;
}
