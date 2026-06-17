// M2 Phase-1 feasibility spike (standalone): prove the fork's NVPTX backend emits
// PTX that optixModuleCreate/optixPipelineCreate accept, by running the hand-authored
// optix_progs_ll.ptx (LLVM IR -> llc -> PTX) through the full count pipeline and
// checking the {1,0,1,1} software oracle on the 4090.
//
// Self-contained (own optixInit + CUDA primary context) so it builds + runs in
// seconds without the cajeta test-suite rebuild. Once green, the check is folded
// into test/xpu/OptiXRayQueryProbeTests.cpp.
//
// build (from this dir, mingw on PATH):
//   g++ -std=c++17 spike_optix_ll.cpp -o spike_optix_ll \
//       -I"$OPTIX/include" -I"$CUDA/include" -lcfgmgr32 -ladvapi32

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <windows.h>

#include <optix.h>
#include <optix_stubs.h>
#include <optix_function_table_definition.h>  // defines the table in this one TU

// ---- CUDA driver API (nvcuda.dll) -----------------------------------------
#define DRV(name, ret, ...) typedef ret (*name##_t)(__VA_ARGS__); static name##_t p_##name = nullptr;
DRV(cuInit, CUresult, unsigned)
DRV(cuDeviceGet, CUresult, CUdevice*, int)
DRV(cuDevicePrimaryCtxRetain, CUresult, CUcontext*, CUdevice)
DRV(cuCtxSetCurrent, CUresult, CUcontext)
DRV(cuMemAlloc, CUresult, CUdeviceptr*, size_t)
DRV(cuMemFree, CUresult, CUdeviceptr)
DRV(cuMemcpyHtoD, CUresult, CUdeviceptr, const void*, size_t)
DRV(cuMemcpyDtoH, CUresult, void*, CUdeviceptr, size_t)
DRV(cuStreamCreate, CUresult, CUstream*, unsigned)
DRV(cuStreamSynchronize, CUresult, CUstream)
#undef DRV

static bool loadCuda() {
    HMODULE h = LoadLibraryA("nvcuda.dll");
    if (!h) { printf("FAIL: nvcuda.dll not found\n"); return false; }
#define BIND(name, sym) p_##name = (name##_t) GetProcAddress(h, sym); if (!p_##name) { printf("FAIL: %s missing\n", sym); return false; }
    BIND(cuInit, "cuInit")
    BIND(cuDeviceGet, "cuDeviceGet")
    BIND(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain")
    BIND(cuCtxSetCurrent, "cuCtxSetCurrent")
    BIND(cuMemAlloc, "cuMemAlloc_v2")
    BIND(cuMemFree, "cuMemFree_v2")
    BIND(cuMemcpyHtoD, "cuMemcpyHtoD_v2")
    BIND(cuMemcpyDtoH, "cuMemcpyDtoH_v2")
    BIND(cuStreamCreate, "cuStreamCreate")
    BIND(cuStreamSynchronize, "cuStreamSynchronize")
#undef BIND
    return true;
}

template <typename T>
struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) SbtRecord { char header[OPTIX_SBT_RECORD_HEADER_SIZE]; T data; };
struct Empty {};
typedef SbtRecord<Empty> EmptyRecord;

static CUdeviceptr upload(const void* host, size_t bytes) {
    CUdeviceptr d = 0; p_cuMemAlloc(&d, bytes); p_cuMemcpyHtoD(d, host, bytes); return d;
}

#define CK(call) do { OptixResult _r = (call); if (_r != OPTIX_SUCCESS) { printf("FAIL %s -> OptiX %d\n  log: %s\n", #call, (int)_r, log); return 2; } } while(0)
#define CKC(call) do { CUresult _r = (call); if (_r != CUDA_SUCCESS) { printf("FAIL %s -> CU %d\n", #call, (int)_r); return 2; } } while(0)

int main() {
    if (!loadCuda()) return 2;

    CKC(p_cuInit(0));
    CUdevice dev; CKC(p_cuDeviceGet(&dev, 0));
    CUcontext ctx; CKC(p_cuDevicePrimaryCtxRetain(&ctx, dev));
    CKC(p_cuCtxSetCurrent(ctx));

    char log[8192]; size_t logSize = sizeof(log);
    if (optixInit() != OPTIX_SUCCESS) { printf("FAIL optixInit\n"); return 2; }
    OptixDeviceContext octx;
    if (optixDeviceContextCreate(ctx, nullptr, &octx) != OPTIX_SUCCESS) { printf("FAIL deviceContextCreate\n"); return 2; }

    // PTX from the hand-authored .ll
    std::ifstream f("optix_progs_ll.ptx", std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf(); std::string ptx = ss.str();
    if (ptx.empty()) { printf("FAIL: optix_progs_ll.ptx empty/missing\n"); return 2; }
    printf("PTX loaded: %zu bytes\n", ptx.size());

    // AABB AS (same scene as the M0 oracle)
    const unsigned np = 3;
    float boxes[np*6] = {
        -0.5f,-0.5f,-0.5f,  0.5f, 0.5f, 0.5f,
         9.5f,-0.5f,-0.5f, 10.5f, 0.5f, 0.5f,
        19.5f,-0.5f,-0.5f, 20.5f, 0.5f, 0.5f,
    };
    OptixAabb aabbs[np];
    for (unsigned i=0;i<np;i++) aabbs[i]={boxes[i*6+0],boxes[i*6+1],boxes[i*6+2],boxes[i*6+3],boxes[i*6+4],boxes[i*6+5]};
    CUdeviceptr d_aabbs = upload(aabbs, sizeof(aabbs));
    CUdeviceptr d_boxes = upload(boxes, sizeof(boxes));
    unsigned geomFlags[1] = { OPTIX_GEOMETRY_FLAG_NONE };
    OptixBuildInput bi = {};
    bi.type = OPTIX_BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES;
    bi.customPrimitiveArray.aabbBuffers = &d_aabbs;
    bi.customPrimitiveArray.numPrimitives = np;
    bi.customPrimitiveArray.flags = geomFlags;
    bi.customPrimitiveArray.numSbtRecords = 1;
    OptixAccelBuildOptions ao = {};
    ao.buildFlags = OPTIX_BUILD_FLAG_NONE; ao.operation = OPTIX_BUILD_OPERATION_BUILD;
    OptixAccelBufferSizes sizes;
    CK(optixAccelComputeMemoryUsage(octx, &ao, &bi, 1, &sizes));
    CUdeviceptr d_temp=0, d_out=0;
    p_cuMemAlloc(&d_temp, sizes.tempSizeInBytes);
    p_cuMemAlloc(&d_out, sizes.outputSizeInBytes);
    OptixTraversableHandle gas = 0;
    CK(optixAccelBuild(octx, 0, &ao, &bi, 1, d_temp, sizes.tempSizeInBytes, d_out, sizes.outputSizeInBytes, &gas, nullptr, 0));
    printf("AS built\n");

    // ---- THE GATE: module + program groups + pipeline from LLVM-emitted PTX ----
    OptixModuleCompileOptions mco = {};
    OptixPipelineCompileOptions pco = {};
    pco.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
    pco.numPayloadValues = 1; pco.numAttributeValues = 2;
    pco.pipelineLaunchParamsVariableName = "params";
    pco.usesPrimitiveTypeFlags = OPTIX_PRIMITIVE_TYPE_FLAGS_CUSTOM;
    OptixModule mod;
    logSize = sizeof(log);
    CK(optixModuleCreate(octx, &mco, &pco, ptx.c_str(), ptx.size(), log, &logSize, &mod));
    printf("optixModuleCreate ACCEPTED LLVM-emitted PTX  (log: %s)\n", logSize>1?log:"<empty>");

    OptixProgramGroupOptions pgo = {};
    OptixProgramGroupDesc rgD = {}; rgD.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    rgD.raygen.module = mod; rgD.raygen.entryFunctionName = "__raygen__count";
    OptixProgramGroupDesc msD = {}; msD.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    msD.miss.module = mod; msD.miss.entryFunctionName = "__miss__nop";
    OptixProgramGroupDesc hgD = {}; hgD.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    hgD.hitgroup.moduleIS = mod; hgD.hitgroup.entryFunctionNameIS = "__intersection__box";
    hgD.hitgroup.moduleAH = mod; hgD.hitgroup.entryFunctionNameAH = "__anyhit__count";
    OptixProgramGroup rg, ms, hg;
    logSize=sizeof(log); CK(optixProgramGroupCreate(octx, &rgD, 1, &pgo, log, &logSize, &rg));
    logSize=sizeof(log); CK(optixProgramGroupCreate(octx, &msD, 1, &pgo, log, &logSize, &ms));
    logSize=sizeof(log); CK(optixProgramGroupCreate(octx, &hgD, 1, &pgo, log, &logSize, &hg));
    OptixProgramGroup groups[] = { rg, ms, hg };
    OptixPipelineLinkOptions plo = {}; plo.maxTraceDepth = 1;
    OptixPipeline pipeline; logSize=sizeof(log);
    CK(optixPipelineCreate(octx, &pco, &plo, groups, 3, log, &logSize, &pipeline));
    printf("optixPipelineCreate ACCEPTED\n");

    // ---- launch + oracle check ----
    EmptyRecord rgR, msR, hgR;
    optixSbtRecordPackHeader(rg, &rgR); optixSbtRecordPackHeader(ms, &msR); optixSbtRecordPackHeader(hg, &hgR);
    CUdeviceptr d_rg=upload(&rgR,sizeof(rgR)), d_ms=upload(&msR,sizeof(msR)), d_hg=upload(&hgR,sizeof(hgR));
    OptixShaderBindingTable sbt = {};
    sbt.raygenRecord = d_rg;
    sbt.missRecordBase = d_ms; sbt.missRecordStrideInBytes = sizeof(EmptyRecord); sbt.missRecordCount = 1;
    sbt.hitgroupRecordBase = d_hg; sbt.hitgroupRecordStrideInBytes = sizeof(EmptyRecord); sbt.hitgroupRecordCount = 1;

    struct F3 { float x,y,z; };
    const unsigned n = 4;
    F3 q[n] = { {0,0,0}, {5,0,0}, {10,0,0}, {19.7f,0,0} };
    CUdeviceptr d_q = upload(q, sizeof(q));
    CUdeviceptr d_counts = 0; p_cuMemAlloc(&d_counts, n*sizeof(unsigned));

    struct Params { OptixTraversableHandle handle; CUdeviceptr queries, counts; unsigned n; CUdeviceptr boxes; };
    Params p = {}; p.handle = gas; p.queries = d_q; p.counts = d_counts; p.n = n; p.boxes = d_boxes;
    CUdeviceptr d_params = upload(&p, sizeof(p));

    CUstream stream; p_cuStreamCreate(&stream, 0);
    CK(optixLaunch(pipeline, stream, d_params, sizeof(Params), &sbt, n, 1, 1));
    p_cuStreamSynchronize(stream);

    unsigned counts[n] = {99,99,99,99};
    p_cuMemcpyDtoH(counts, d_counts, sizeof(counts));
    printf("counts = {%u,%u,%u,%u}  (oracle {1,0,1,1})\n", counts[0],counts[1],counts[2],counts[3]);
    bool ok = counts[0]==1 && counts[1]==0 && counts[2]==1 && counts[3]==1;
    printf("%s\n", ok ? "SPIKE PASS: LLVM->PTX runs on RT cores, matches oracle" : "SPIKE FAIL: oracle mismatch");
    return ok ? 0 : 1;
}
