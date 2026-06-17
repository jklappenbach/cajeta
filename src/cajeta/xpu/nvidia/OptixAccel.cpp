//
// OptixAccel.cpp — NVIDIA OptiX acceleration-structure runtime glue (cajeta-gpu,
// NVIDIA-CUDA native RT-core ray query, Milestone 1).
//
// Owns the single OptiX function-table definition and a lazily-initialized
// OptixDeviceContext over the CUDA primary context, and builds/frees OptiX
// acceleration structures (optixAccelBuild) over AABB and triangle geometry. The
// CUDA noun provider in the embedded runtime (cajeta_runtime.c) calls the
// extern "C" entry points below; the JIT resolves them via its process-symbol
// generator (the same path the TLS engine uses), so they live in cajeta_lib as a
// standalone host object — NOT in the embedded bitcode (OptiX is C++ + needs the
// driver loader, which can't live in JIT bitcode).
//
// DEPENDENCY MODEL: OptiX is a COMPILE-TIME-only dependency (header-only SDK; the
// engine is the driver's nvoptix.dll, loaded lazily by optixInit). CMake defines
// CAJETA_HAS_OPTIX + adds the include dirs only when the SDK is found; otherwise
// every entry point below is a stub returning "unavailable" (0), so the runtime
// always links and the software-BVH floor is unaffected.
//
// M1 SCOPE: build + free + the device context, validated in isolation. The VERB
// (traversal via an OptiX pipeline / optixTrace) and AUTO-on-CUDA selection are
// Milestone 2 — until then the OptiX AS is opt-in and not consumed by a kernel.
//

#include <cstdint>

#ifdef CAJETA_HAS_OPTIX

#include <cstring>
#include <mutex>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include <optix.h>
#include <optix_function_table_definition.h>  // the single function-table definition
#include <optix_stubs.h>

namespace {

// --- minimal CUDA driver API, dynamically loaded from nvcuda.dll (the runtime
// already dlopen's it for kernels; the glue loads its own pointers to stay a
// self-contained TU). cuda.h (via optix) declares the bare names, so use p_-ptrs.
#define DRV(name, ret, ...) typedef ret (*name##_t)(__VA_ARGS__); name##_t p_##name = nullptr;
DRV(cuInit, CUresult, unsigned)
DRV(cuDeviceGet, CUresult, CUdevice*, int)
DRV(cuDevicePrimaryCtxRetain, CUresult, CUcontext*, CUdevice)
DRV(cuCtxSetCurrent, CUresult, CUcontext)
DRV(cuCtxGetCurrent, CUresult, CUcontext*)
DRV(cuCtxPushCurrent, CUresult, CUcontext)
DRV(cuCtxPopCurrent, CUresult, CUcontext*)
DRV(cuMemAlloc, CUresult, CUdeviceptr*, size_t)
DRV(cuMemFree, CUresult, CUdeviceptr)
DRV(cuMemcpyHtoD, CUresult, CUdeviceptr, const void*, size_t)
DRV(cuStreamSynchronize, CUresult, CUstream)
#undef DRV

bool loadCudaDriver() {
#ifdef _WIN32
    HMODULE h = LoadLibraryA("nvcuda.dll");
    if (!h) return false;
#  define BIND(name, sym) p_##name = (name##_t) GetProcAddress(h, sym); if (!p_##name) return false;
#else
    void* h = nullptr;  // POSIX OptiX is a separate follow-up; M1 targets Windows.
    return false;
#  define BIND(name, sym)
#endif
    BIND(cuInit, "cuInit")
    BIND(cuDeviceGet, "cuDeviceGet")
    BIND(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain")
    BIND(cuCtxSetCurrent, "cuCtxSetCurrent")
    BIND(cuCtxGetCurrent, "cuCtxGetCurrent")
    BIND(cuCtxPushCurrent, "cuCtxPushCurrent_v2")
    BIND(cuCtxPopCurrent, "cuCtxPopCurrent_v2")
    BIND(cuMemAlloc, "cuMemAlloc_v2")
    BIND(cuMemFree, "cuMemFree_v2")
    BIND(cuMemcpyHtoD, "cuMemcpyHtoD_v2")
    BIND(cuStreamSynchronize, "cuStreamSynchronize")
#undef BIND
    return true;
}

// Lazily-initialized OptiX state. The per-device PRIMARY CUDA context is retained;
// post-M2-Phase-2 (R4) the cajeta runtime ALSO retains the primary (instead of its
// own cuCtxCreate ctx), so the glue's AS build, the OptiX pipeline, and the runtime's
// kernel launch all share this one context — the M1 split is resolved.
struct OptixState {
    bool tried = false;
    bool ok = false;
    CUcontext cuCtx = nullptr;
    OptixDeviceContext ctx = nullptr;
};
OptixState g_state;
std::mutex g_lock;

OptixState& ensureInit() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_state.tried) return g_state;
    g_state.tried = true;
    if (!loadCudaDriver()) return g_state;
    if (p_cuInit(0) != 0) return g_state;
    CUdevice dev;
    if (p_cuDeviceGet(&dev, 0) != 0) return g_state;
    if (p_cuDevicePrimaryCtxRetain(&g_state.cuCtx, dev) != 0) return g_state;
    // CRITICAL: do NOT leave a different CUDA context current. The cajeta runtime
    // uses its OWN cuCtxCreate context (per-thread current), and the resolver
    // probes cajeta_xpu_optix_available() even on the software path — so clobbering
    // the current context here would send the runtime's cuMemAlloc/launch to the
    // wrong context. Push the primary only for the OptiX init calls, then pop back.
    CUcontext prev = nullptr;
    p_cuCtxGetCurrent(&prev);
    if (p_cuCtxPushCurrent(g_state.cuCtx) != 0) return g_state;
    bool initOk = (optixInit() == OPTIX_SUCCESS);
    OptixDeviceContextOptions opts = {};
    if (initOk && optixDeviceContextCreate(g_state.cuCtx, &opts, &g_state.ctx) == OPTIX_SUCCESS)
        g_state.ok = true;
    CUcontext popped = nullptr;
    p_cuCtxPopCurrent(&popped);          // restore: pop our pushed primary
    if (prev) p_cuCtxSetCurrent(prev);   // and re-assert the caller's context
    return g_state;
}

// RAII: make the OptiX primary context current for the enclosed CUDA/OptiX calls
// (AS build, free), restoring the caller's current context on scope exit — so the
// runtime's per-thread context is never disturbed by an OptiX AS operation.
struct ScopedPrimary {
    bool pushed = false;
    explicit ScopedPrimary(CUcontext primary) {
        if (primary && p_cuCtxPushCurrent(primary) == 0) pushed = true;
    }
    ~ScopedPrimary() { if (pushed) { CUcontext p = nullptr; p_cuCtxPopCurrent(&p); } }
};

// The handle the CUDA noun provider records for an OptiX AS: the traversable plus
// the device buffers to free. Returned as an int64 (pointer to this heap struct).
struct OptixAs {
    OptixTraversableHandle trav = 0;
    CUdeviceptr output = 0;   // the AS storage; freed on accel_free
    CUdeviceptr boxes = 0;    // (AABB AS only) the count*6 raw box floats, kept on
                              // device for the M2 count intersection program's
                              // point-in-box test (params.boxes); 0 for triangle AS.
};

// Shared accel-build core for one OptixBuildInput. Returns a heap OptixAs* (as
// int64) or 0 on any failure (caller falls back to the software floor).
int64_t buildAccel(OptixState& s, const OptixBuildInput& bi) {
    OptixAccelBuildOptions ao = {};
    ao.buildFlags = OPTIX_BUILD_FLAG_NONE;
    ao.operation = OPTIX_BUILD_OPERATION_BUILD;
    OptixAccelBufferSizes sizes;
    if (optixAccelComputeMemoryUsage(s.ctx, &ao, &bi, 1, &sizes) != OPTIX_SUCCESS)
        return 0;
    CUdeviceptr d_temp = 0, d_out = 0;
    if (p_cuMemAlloc(&d_temp, sizes.tempSizeInBytes) != 0) return 0;
    if (p_cuMemAlloc(&d_out, sizes.outputSizeInBytes) != 0) { p_cuMemFree(d_temp); return 0; }
    OptixTraversableHandle trav = 0;
    OptixResult r = optixAccelBuild(s.ctx, 0, &ao, &bi, 1,
                                    d_temp, sizes.tempSizeInBytes,
                                    d_out, sizes.outputSizeInBytes, &trav, nullptr, 0);
    p_cuMemFree(d_temp);
    if (r != OPTIX_SUCCESS) { p_cuMemFree(d_out); return 0; }
    OptixAs* as = new OptixAs{trav, d_out};
    return (int64_t) (intptr_t) as;
}

} // namespace

extern "C" {

// 1 iff the OptiX runtime initialized (SDK built in AND nvoptix.dll loadable AND a
// CUDA device present). The CUDA noun provider / capability gate on this.
int cajeta_xpu_optix_available(void) {
    return ensureInit().ok ? 1 : 0;
}

// The OptixDeviceContext (as void*) for callers that build their own pipelines on
// top of the shared context (the M0 probe reuses this instead of init'ing its own,
// keeping a single function-table owner). NULL when unavailable.
void* cajeta_xpu_optix_context(void) {
    OptixState& s = ensureInit();
    if (!s.ok) return nullptr;
    // Make the OptiX primary context current on the CALLING thread: callers that
    // build their own pipelines (the M0 probe) issue cuMemAlloc/optixLaunch against
    // it. Safe because this is invoked from a host test thread, not the runtime's
    // launch path (which re-asserts its own context per launch).
    p_cuCtxSetCurrent(s.cuCtx);
    return (void*) s.ctx;
}

// The underlying CUDA context (the per-device PRIMARY) the OptiX device context was
// created over, as a void*. Post-R4 the cajeta runtime retains the SAME primary, so
// this equals cajeta_xpu_cuda_context() — the M2 Phase-2 probe asserts that to prove
// the AS build / pipeline / launch share ONE context. NULL when unavailable.
void* cajeta_xpu_optix_cuda_context(void) {
    OptixState& s = ensureInit();
    return s.ok ? (void*) s.cuCtx : nullptr;
}

// Build a custom-primitive (AABB) OptiX AS. `boxes` is count*6 floats
// (minX,minY,minZ,maxX,maxY,maxZ). Returns an int64 handle (OptixAs*) or 0.
int64_t cajeta_xpu_optix_accel_build_aabbs(const float* boxes, uint32_t count) {
    OptixState& s = ensureInit();
    if (!s.ok || !boxes || count == 0) return 0;
    ScopedPrimary sp(s.cuCtx);   // ops on the OptiX context; caller's restored on exit
    // Upload the AABBs as OptixAabb (same 6-float layout) for the build input.
    CUdeviceptr d_aabbs = 0;
    size_t bytes = (size_t) count * sizeof(OptixAabb);
    if (p_cuMemAlloc(&d_aabbs, bytes) != 0) return 0;
    if (p_cuMemcpyHtoD(d_aabbs, boxes, bytes) != 0) { p_cuMemFree(d_aabbs); return 0; }
    unsigned int flags[1] = { OPTIX_GEOMETRY_FLAG_NONE };
    OptixBuildInput bi = {};
    bi.type = OPTIX_BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES;
    bi.customPrimitiveArray.aabbBuffers = &d_aabbs;
    bi.customPrimitiveArray.numPrimitives = count;
    bi.customPrimitiveArray.flags = flags;
    bi.customPrimitiveArray.numSbtRecords = 1;
    int64_t h = buildAccel(s, bi);
    // OptixAabb IS the count*6-float box layout the M2 count intersection program
    // reads (params.boxes), so KEEP d_aabbs as the AS's boxes buffer rather than
    // freeing it; accel_free releases it. (Only on success — else it would leak.)
    if (h) ((OptixAs*) (intptr_t) h)->boxes = d_aabbs;
    else   p_cuMemFree(d_aabbs);
    return h;
}

// Build a triangle OptiX AS. `verts` is a vertex soup; vertex v of triangle t is
// at (t*3+v)*stride floats (3 = tight). Returns an int64 handle (OptixAs*) or 0.
int64_t cajeta_xpu_optix_accel_build_triangles(const float* verts, uint32_t triCount,
                                               uint32_t stride) {
    OptixState& s = ensureInit();
    if (!s.ok || !verts || triCount == 0 || stride < 3u) return 0;
    ScopedPrimary sp(s.cuCtx);   // ops on the OptiX context; caller's restored on exit
    uint32_t numVerts = triCount * 3u;
    CUdeviceptr d_verts = 0;
    size_t bytes = (size_t) numVerts * stride * sizeof(float);
    if (p_cuMemAlloc(&d_verts, bytes) != 0) return 0;
    if (p_cuMemcpyHtoD(d_verts, verts, bytes) != 0) { p_cuMemFree(d_verts); return 0; }
    unsigned int flags[1] = { OPTIX_GEOMETRY_FLAG_NONE };
    OptixBuildInput bi = {};
    bi.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
    bi.triangleArray.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
    bi.triangleArray.vertexStrideInBytes = stride * sizeof(float);
    bi.triangleArray.numVertices = numVerts;
    bi.triangleArray.vertexBuffers = &d_verts;
    bi.triangleArray.flags = flags;
    bi.triangleArray.numSbtRecords = 1;
    int64_t h = buildAccel(s, bi);
    p_cuMemFree(d_verts);   // built into AS storage; vertex input is transient (no refit in v1)
    return h;
}

// The OptixTraversableHandle for an OptiX AS handle (M2's launch passes this to
// optixTrace via the launch params). 0 if the handle is null/invalid.
uint64_t cajeta_xpu_optix_traversable(int64_t handle) {
    if (!handle) return 0;
    return (uint64_t) ((OptixAs*) (intptr_t) handle)->trav;
}

// The AS's boxes buffer (count*6 floats) as a device pointer (u64), for the M2 count
// intersection program's params.boxes. 0 for a triangle AS or a null handle.
uint64_t cajeta_xpu_optix_accel_boxes(int64_t handle) {
    if (!handle) return 0;
    return (uint64_t) ((OptixAs*) (intptr_t) handle)->boxes;
}

// Free an OptiX AS handle (its device storage + boxes + the bookkeeping struct).
void cajeta_xpu_optix_accel_free(int64_t handle) {
    if (!handle) return;
    OptixAs* as = (OptixAs*) (intptr_t) handle;
    ScopedPrimary sp(ensureInit().cuCtx);   // free on the OptiX context, restore caller's
    if (as->output && p_cuMemFree) p_cuMemFree(as->output);
    if (as->boxes && p_cuMemFree) p_cuMemFree(as->boxes);
    delete as;
}

// Build the OptiX count pipeline from emitted PTX and optixLaunch it. The PTX is the
// cajeta NVPTX-emitted OptiX program module (NvptxOptixRayQuery::emitOptixCountModule);
// `paramsHost`/`paramsLen` is the already-marshalled `params` launch block (the
// {handle,originX,originY,originZ,out,n,boxes} contract). `width` is the launch grid
// (one thread per query). Runs on the shared (primary) context. Returns 0 on success,
// nonzero on any failure (caller may fall back to the software path). v1 builds the
// module/pipeline/SBT per call (no caching yet — a noted follow-up) and is the COUNT
// shape only (payload=1, attributes=2, custom-primitive AABB).
int cajeta_xpu_optix_launch(const char* ptx, uint64_t ptxLen,
                            const char* raygenName, const char* isName,
                            const char* anyhitName, const char* missName,
                            const void* paramsHost, uint64_t paramsLen,
                            uint32_t width) {
    OptixState& s = ensureInit();
    if (!s.ok || !ptx || !ptxLen || !paramsHost || !paramsLen || width == 0) return -1;
    ScopedPrimary sp(s.cuCtx);   // build + launch on the OptiX (primary) context

    char log[4096]; size_t logSize = sizeof(log);
    OptixModuleCompileOptions mco = {};
    OptixPipelineCompileOptions pco = {};
    pco.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
    pco.numPayloadValues = 1;            // the candidate counter
    pco.numAttributeValues = 2;          // OptiX custom-prim minimum
    pco.pipelineLaunchParamsVariableName = "params";
    pco.usesPrimitiveTypeFlags = OPTIX_PRIMITIVE_TYPE_FLAGS_CUSTOM;

    OptixModule mod = nullptr;
    OptixProgramGroup rg = nullptr, ms = nullptr, hg = nullptr;
    OptixPipeline pipeline = nullptr;
    auto cleanup = [&]() {
        if (pipeline) optixPipelineDestroy(pipeline);
        if (hg) optixProgramGroupDestroy(hg);
        if (ms) optixProgramGroupDestroy(ms);
        if (rg) optixProgramGroupDestroy(rg);
        if (mod) optixModuleDestroy(mod);
    };

    if (optixModuleCreate(s.ctx, &mco, &pco, ptx, (size_t) ptxLen, log, &logSize, &mod)
            != OPTIX_SUCCESS) { cleanup(); return -2; }

    OptixProgramGroupOptions pgo = {};
    OptixProgramGroupDesc rgD = {}; rgD.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    rgD.raygen.module = mod; rgD.raygen.entryFunctionName = raygenName;
    OptixProgramGroupDesc msD = {}; msD.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    msD.miss.module = mod; msD.miss.entryFunctionName = missName;
    OptixProgramGroupDesc hgD = {}; hgD.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    hgD.hitgroup.moduleIS = mod; hgD.hitgroup.entryFunctionNameIS = isName;
    hgD.hitgroup.moduleAH = mod; hgD.hitgroup.entryFunctionNameAH = anyhitName;
    logSize = sizeof(log);
    if (optixProgramGroupCreate(s.ctx, &rgD, 1, &pgo, log, &logSize, &rg) != OPTIX_SUCCESS) { cleanup(); return -3; }
    logSize = sizeof(log);
    if (optixProgramGroupCreate(s.ctx, &msD, 1, &pgo, log, &logSize, &ms) != OPTIX_SUCCESS) { cleanup(); return -3; }
    logSize = sizeof(log);
    if (optixProgramGroupCreate(s.ctx, &hgD, 1, &pgo, log, &logSize, &hg) != OPTIX_SUCCESS) { cleanup(); return -3; }

    OptixProgramGroup groups[] = {rg, ms, hg};
    OptixPipelineLinkOptions plo = {}; plo.maxTraceDepth = 1;
    logSize = sizeof(log);
    if (optixPipelineCreate(s.ctx, &pco, &plo, groups, 3, log, &logSize, &pipeline)
            != OPTIX_SUCCESS) { cleanup(); return -4; }

    // SBT: a header-only record per group (no per-record data).
    struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) Rec { char h[OPTIX_SBT_RECORD_HEADER_SIZE]; };
    Rec rgR, msR, hgR;
    optixSbtRecordPackHeader(rg, &rgR);
    optixSbtRecordPackHeader(ms, &msR);
    optixSbtRecordPackHeader(hg, &hgR);
    CUdeviceptr d_rg = 0, d_ms = 0, d_hg = 0, d_params = 0;
    auto up = [&](CUdeviceptr* d, const void* src, size_t n) -> bool {
        if (p_cuMemAlloc(d, n) != 0) return false;
        return p_cuMemcpyHtoD(*d, src, n) == 0;
    };
    int rc = -5;
    if (up(&d_rg, &rgR, sizeof(Rec)) && up(&d_ms, &msR, sizeof(Rec)) &&
        up(&d_hg, &hgR, sizeof(Rec)) && up(&d_params, paramsHost, (size_t) paramsLen)) {
        OptixShaderBindingTable sbt = {};
        sbt.raygenRecord = d_rg;
        sbt.missRecordBase = d_ms; sbt.missRecordStrideInBytes = sizeof(Rec); sbt.missRecordCount = 1;
        sbt.hitgroupRecordBase = d_hg; sbt.hitgroupRecordStrideInBytes = sizeof(Rec); sbt.hitgroupRecordCount = 1;
        if (optixLaunch(pipeline, /*stream=*/0, d_params, (size_t) paramsLen, &sbt,
                        width, 1, 1) == OPTIX_SUCCESS &&
            p_cuStreamSynchronize(0) == 0)
            rc = 0;
    }
    if (d_rg) p_cuMemFree(d_rg);
    if (d_ms) p_cuMemFree(d_ms);
    if (d_hg) p_cuMemFree(d_hg);
    if (d_params) p_cuMemFree(d_params);
    cleanup();
    return rc;
}

// Triangle nearest-hit launch (M2 Phase 4). Built-in triangle traversal, so the
// hitgroup has a CLOSESTHIT program (no intersection / anyhit) and no payload —
// closesthit commits the nearest and writes T / type / prim to the params buffers.
// Otherwise structurally identical to cajeta_xpu_optix_launch (the AABB-count path).
int cajeta_xpu_optix_launch_tri(const char* ptx, uint64_t ptxLen,
                                const char* raygenName, const char* closesthitName,
                                const char* missName,
                                const void* paramsHost, uint64_t paramsLen,
                                uint32_t width) {
    OptixState& s = ensureInit();
    if (!s.ok || !ptx || !ptxLen || !paramsHost || !paramsLen || width == 0) return -1;
    ScopedPrimary sp(s.cuCtx);

    char log[4096]; size_t logSize = sizeof(log);
    OptixModuleCompileOptions mco = {};
    OptixPipelineCompileOptions pco = {};
    pco.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
    pco.numPayloadValues = 0;            // closesthit writes via params, not payload
    pco.numAttributeValues = 2;          // built-in triangle barycentrics
    pco.pipelineLaunchParamsVariableName = "params";
    pco.usesPrimitiveTypeFlags = OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE;

    OptixModule mod = nullptr;
    OptixProgramGroup rg = nullptr, ms = nullptr, hg = nullptr;
    OptixPipeline pipeline = nullptr;
    auto cleanup = [&]() {
        if (pipeline) optixPipelineDestroy(pipeline);
        if (hg) optixProgramGroupDestroy(hg);
        if (ms) optixProgramGroupDestroy(ms);
        if (rg) optixProgramGroupDestroy(rg);
        if (mod) optixModuleDestroy(mod);
    };

    if (optixModuleCreate(s.ctx, &mco, &pco, ptx, (size_t) ptxLen, log, &logSize, &mod)
            != OPTIX_SUCCESS) { cleanup(); return -2; }

    OptixProgramGroupOptions pgo = {};
    OptixProgramGroupDesc rgD = {}; rgD.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    rgD.raygen.module = mod; rgD.raygen.entryFunctionName = raygenName;
    OptixProgramGroupDesc msD = {}; msD.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    msD.miss.module = mod; msD.miss.entryFunctionName = missName;
    OptixProgramGroupDesc hgD = {}; hgD.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    hgD.hitgroup.moduleCH = mod; hgD.hitgroup.entryFunctionNameCH = closesthitName;
    logSize = sizeof(log);
    if (optixProgramGroupCreate(s.ctx, &rgD, 1, &pgo, log, &logSize, &rg) != OPTIX_SUCCESS) { cleanup(); return -3; }
    logSize = sizeof(log);
    if (optixProgramGroupCreate(s.ctx, &msD, 1, &pgo, log, &logSize, &ms) != OPTIX_SUCCESS) { cleanup(); return -3; }
    logSize = sizeof(log);
    if (optixProgramGroupCreate(s.ctx, &hgD, 1, &pgo, log, &logSize, &hg) != OPTIX_SUCCESS) { cleanup(); return -3; }

    OptixProgramGroup groups[] = {rg, ms, hg};
    OptixPipelineLinkOptions plo = {}; plo.maxTraceDepth = 1;
    logSize = sizeof(log);
    if (optixPipelineCreate(s.ctx, &pco, &plo, groups, 3, log, &logSize, &pipeline)
            != OPTIX_SUCCESS) { cleanup(); return -4; }

    struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) Rec { char h[OPTIX_SBT_RECORD_HEADER_SIZE]; };
    Rec rgR, msR, hgR;
    optixSbtRecordPackHeader(rg, &rgR);
    optixSbtRecordPackHeader(ms, &msR);
    optixSbtRecordPackHeader(hg, &hgR);
    CUdeviceptr d_rg = 0, d_ms = 0, d_hg = 0, d_params = 0;
    auto up = [&](CUdeviceptr* d, const void* src, size_t n) -> bool {
        if (p_cuMemAlloc(d, n) != 0) return false;
        return p_cuMemcpyHtoD(*d, src, n) == 0;
    };
    int rc = -5;
    if (up(&d_rg, &rgR, sizeof(Rec)) && up(&d_ms, &msR, sizeof(Rec)) &&
        up(&d_hg, &hgR, sizeof(Rec)) && up(&d_params, paramsHost, (size_t) paramsLen)) {
        OptixShaderBindingTable sbt = {};
        sbt.raygenRecord = d_rg;
        sbt.missRecordBase = d_ms; sbt.missRecordStrideInBytes = sizeof(Rec); sbt.missRecordCount = 1;
        sbt.hitgroupRecordBase = d_hg; sbt.hitgroupRecordStrideInBytes = sizeof(Rec); sbt.hitgroupRecordCount = 1;
        if (optixLaunch(pipeline, /*stream=*/0, d_params, (size_t) paramsLen, &sbt,
                        width, 1, 1) == OPTIX_SUCCESS &&
            p_cuStreamSynchronize(0) == 0)
            rc = 0;
    }
    if (d_rg) p_cuMemFree(d_rg);
    if (d_ms) p_cuMemFree(d_ms);
    if (d_hg) p_cuMemFree(d_hg);
    if (d_params) p_cuMemFree(d_params);
    cleanup();
    return rc;
}

} // extern "C"

#else // !CAJETA_HAS_OPTIX — stubs so the runtime always links; software floor only.

extern "C" {
int      cajeta_xpu_optix_available(void) { return 0; }
void*    cajeta_xpu_optix_context(void) { return nullptr; }
void*    cajeta_xpu_optix_cuda_context(void) { return nullptr; }
int64_t  cajeta_xpu_optix_accel_build_aabbs(const float*, uint32_t) { return 0; }
int64_t  cajeta_xpu_optix_accel_build_triangles(const float*, uint32_t, uint32_t) { return 0; }
uint64_t cajeta_xpu_optix_traversable(int64_t) { return 0; }
uint64_t cajeta_xpu_optix_accel_boxes(int64_t) { return 0; }
void     cajeta_xpu_optix_accel_free(int64_t) {}
int      cajeta_xpu_optix_launch(const char*, uint64_t, const char*, const char*,
                                 const char*, const char*, const void*, uint64_t,
                                 uint32_t) { return -1; }
int      cajeta_xpu_optix_launch_tri(const char*, uint64_t, const char*, const char*,
                                     const char*, const void*, uint64_t,
                                     uint32_t) { return -1; }
}

#endif // CAJETA_HAS_OPTIX
