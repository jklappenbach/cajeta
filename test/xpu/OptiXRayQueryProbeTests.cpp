//
// OptiX RT-core ray-query parity probe (cajeta-gpu — NVIDIA-CUDA native RT path,
// Milestone 0). Proves the OptiX runtime stack + the inline-RayQuery → pipeline
// mapping reproduce cajeta's software-BVH oracle ON the RT cores of an NVIDIA GPU:
//
//   * AABB candidate count  — custom-primitive AS + raygen/intersection/anyhit;
//     anyhit accumulates and optixIgnoreIntersection()s to visit every overlapping
//     box. Same scene/rays as minimalRayQueryOnCpuSoftwareBvh -> counts {1,0,1,1}.
//   * Triangle nearest hit  — built-in triangle AS + closesthit committed getters
//     (T / primitive index / barycentrics). Same scene as nearestHitOnCpuSoftwareBvh
//     + candidateGettersOnCpuSoftwareBvh -> t=6, prim=1, u=v=0.25.
//
// This is the de-risking milestone for the OptiX-backed AccelerationStructure tier
// (documents/gpu-rayquery-optix/): the canonical raygen/anyhit/closesthit bodies the
// future NVPTX codegen (M2) must emit live as the device sources next to this file
// (test/xpu/optix/optix_progs.cu, optix_tri.cu), checked in alongside the portable
// compute_60 PTX the OptiX module compiler re-JITs to the device SM.
//
// DEPENDENCY MODEL (why this needs no heavy install): OptiX is a HEADER-ONLY SDK;
// the engine is nvoptix.dll, shipped with the NVIDIA driver. So at runtime only the
// driver is needed (exactly like cajeta's nvcuda.dll dlopen). At BUILD time only the
// headers are needed — detected by CMake; absent -> CAJETA_TEST_HAS_OPTIX is undefined
// and this whole TU compiles to nothing (the software floor is unaffected).
//

#ifdef CAJETA_TEST_HAS_OPTIX

#include "gtest/gtest.h"

#include "cajeta/xpu/nvidia/CudaDriver.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <fstream>
#include <sstream>
#include <windows.h>

#include <optix.h>
#include <optix_stubs.h>   // stubs call through the table OptixAccel.cpp defines

// The OptiX runtime glue (src/cajeta/xpu/nvidia/OptixAccel.cpp) owns the single
// function-table definition + the lazily-initialized device context over the CUDA
// primary context. The probe reuses that context (one owner, no duplicate table).
extern "C" void*    cajeta_xpu_optix_context(void);
extern "C" int      cajeta_xpu_optix_available(void);
extern "C" int64_t  cajeta_xpu_optix_accel_build_aabbs(const float* boxes, unsigned count);
extern "C" uint64_t cajeta_xpu_optix_traversable(int64_t handle);
extern "C" void     cajeta_xpu_optix_accel_free(int64_t handle);

namespace {

// ---- CUDA driver API, dynamically loaded from nvcuda.dll (no MSVC import lib
// under mingw; cuda.h via optix declares the bare names, so load into p_-ptrs).
// Init + the CUDA primary context (made current) are the glue's job; the probe
// only needs the memory/stream ops, which act on the current (glue's) context.
#define DRV(name, ret, ...) typedef ret (*name##_t)(__VA_ARGS__); name##_t p_##name = nullptr;
DRV(cuMemAlloc, CUresult, CUdeviceptr*, size_t)
DRV(cuMemFree, CUresult, CUdeviceptr)
DRV(cuMemcpyHtoD, CUresult, CUdeviceptr, const void*, size_t)
DRV(cuMemcpyDtoH, CUresult, void*, CUdeviceptr, size_t)
DRV(cuStreamCreate, CUresult, CUstream*, unsigned)
DRV(cuStreamSynchronize, CUresult, CUstream)
#undef DRV

bool loadCuda() {
    HMODULE h = LoadLibraryA("nvcuda.dll");
    if (!h) return false;
#define BIND(name, sym) p_##name = (name##_t) GetProcAddress(h, sym); if (!p_##name) return false;
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
struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) SbtRecord {
    char header[OPTIX_SBT_RECORD_HEADER_SIZE];
    T data;
};
struct Empty {};
typedef SbtRecord<Empty> EmptyRecord;

std::string sourceRoot() {
    const char* e = std::getenv("CAJETA_SOURCE_ROOT");
    return (e && *e) ? std::string(e) : std::string(CAJETA_SOURCE_ROOT_DEFAULT);
}

std::string readPtx(const char* leaf) {
    std::ifstream f(sourceRoot() + "/test/xpu/optix/" + leaf, std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

CUdeviceptr upload(const void* host, size_t bytes) {
    CUdeviceptr d = 0; p_cuMemAlloc(&d, bytes); p_cuMemcpyHtoD(d, host, bytes); return d;
}

// The OptiX device context from the runtime glue (which lazily runs optixInit +
// retains/sets-current the CUDA primary context the memory ops below act on).
// Returns nullptr if OptiX/CUDA is unavailable, so the caller can SKIP.
OptixDeviceContext glueContext() {
    if (!loadCuda()) return nullptr;
    return (OptixDeviceContext) cajeta_xpu_optix_context();
}

} // namespace

// The runtime glue's AS build/free path directly (the CUDA noun provider's OptiX
// arm calls exactly these). Isolates the glue from the JIT/active-backend path: if
// this passes but optixRecordsImplOnNvptxDevice falls back to software, the issue
// is in the JIT resolution, not the glue build.
TEST(OptiXRayQueryProbe, glueBuildsAndFreesAabbAs) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    if (!cajeta_xpu_optix_available()) {
        GTEST_SKIP() << "OptiX runtime unavailable (SDK built in but nvoptix/CUDA absent)";
    }
    float boxes[18] = {
        -0.5f,-0.5f,-0.5f,  0.5f, 0.5f, 0.5f,
         9.5f,-0.5f,-0.5f, 10.5f, 0.5f, 0.5f,
        19.5f,-0.5f,-0.5f, 20.5f, 0.5f, 0.5f,
    };
    int64_t h = cajeta_xpu_optix_accel_build_aabbs(boxes, 3);
    EXPECT_NE(h, 0) << "glue optixAccelBuild (custom prim) returned no handle";
    if (h) {
        EXPECT_NE(cajeta_xpu_optix_traversable(h), 0ull) << "null traversable";
        cajeta_xpu_optix_accel_free(h);
    }
}

// AABB candidate count on the RT cores == the software oracle's {1,0,1,1}.
TEST(OptiXRayQueryProbe, aabbCandidateCountMatchesSoftwareOracle) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    std::string ptx = readPtx("optix_progs.ptx");
    ASSERT_FALSE(ptx.empty()) << "test/xpu/optix/optix_progs.ptx not found";

    OptixDeviceContext octx = glueContext();
    ASSERT_NE(octx, nullptr) << "OptiX/CUDA context unavailable from the runtime glue";

    const unsigned np = 3;
    float boxes[np * 6] = {
        -0.5f,-0.5f,-0.5f,  0.5f, 0.5f, 0.5f,
         9.5f,-0.5f,-0.5f, 10.5f, 0.5f, 0.5f,
        19.5f,-0.5f,-0.5f, 20.5f, 0.5f, 0.5f,
    };
    OptixAabb aabbs[np];
    for (unsigned i = 0; i < np; i++)
        aabbs[i] = { boxes[i*6+0],boxes[i*6+1],boxes[i*6+2],boxes[i*6+3],boxes[i*6+4],boxes[i*6+5] };
    CUdeviceptr d_aabbs = upload(aabbs, sizeof(aabbs));
    CUdeviceptr d_boxes = upload(boxes, sizeof(boxes));

    unsigned int geomFlags[1] = { OPTIX_GEOMETRY_FLAG_NONE };
    OptixBuildInput bi = {};
    bi.type = OPTIX_BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES;
    bi.customPrimitiveArray.aabbBuffers = &d_aabbs;
    bi.customPrimitiveArray.numPrimitives = np;
    bi.customPrimitiveArray.flags = geomFlags;
    bi.customPrimitiveArray.numSbtRecords = 1;
    OptixAccelBuildOptions ao = {};
    ao.buildFlags = OPTIX_BUILD_FLAG_NONE; ao.operation = OPTIX_BUILD_OPERATION_BUILD;
    OptixAccelBufferSizes sizes;
    ASSERT_EQ(optixAccelComputeMemoryUsage(octx, &ao, &bi, 1, &sizes), OPTIX_SUCCESS);
    CUdeviceptr d_temp = 0, d_out = 0;
    p_cuMemAlloc(&d_temp, sizes.tempSizeInBytes);
    p_cuMemAlloc(&d_out, sizes.outputSizeInBytes);
    OptixTraversableHandle gas = 0;
    ASSERT_EQ(optixAccelBuild(octx, 0, &ao, &bi, 1, d_temp, sizes.tempSizeInBytes,
                              d_out, sizes.outputSizeInBytes, &gas, nullptr, 0), OPTIX_SUCCESS);

    OptixModuleCompileOptions mco = {};
    OptixPipelineCompileOptions pco = {};
    pco.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
    pco.numPayloadValues = 1; pco.numAttributeValues = 2;
    pco.pipelineLaunchParamsVariableName = "params";
    pco.usesPrimitiveTypeFlags = OPTIX_PRIMITIVE_TYPE_FLAGS_CUSTOM;
    char log[4096]; size_t logSize = sizeof(log);
    OptixModule mod;
    ASSERT_EQ(optixModuleCreate(octx, &mco, &pco, ptx.c_str(), ptx.size(),
                                log, &logSize, &mod), OPTIX_SUCCESS) << log;

    OptixProgramGroupOptions pgo = {};
    OptixProgramGroupDesc rgD = {}; rgD.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    rgD.raygen.module = mod; rgD.raygen.entryFunctionName = "__raygen__count";
    OptixProgramGroupDesc msD = {}; msD.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    msD.miss.module = mod; msD.miss.entryFunctionName = "__miss__nop";
    OptixProgramGroupDesc hgD = {}; hgD.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    hgD.hitgroup.moduleIS = mod; hgD.hitgroup.entryFunctionNameIS = "__intersection__box";
    hgD.hitgroup.moduleAH = mod; hgD.hitgroup.entryFunctionNameAH = "__anyhit__count";
    OptixProgramGroup rg, ms, hg;
    logSize = sizeof(log); ASSERT_EQ(optixProgramGroupCreate(octx, &rgD, 1, &pgo, log, &logSize, &rg), OPTIX_SUCCESS);
    logSize = sizeof(log); ASSERT_EQ(optixProgramGroupCreate(octx, &msD, 1, &pgo, log, &logSize, &ms), OPTIX_SUCCESS);
    logSize = sizeof(log); ASSERT_EQ(optixProgramGroupCreate(octx, &hgD, 1, &pgo, log, &logSize, &hg), OPTIX_SUCCESS);

    OptixProgramGroup groups[] = { rg, ms, hg };
    OptixPipelineLinkOptions plo = {}; plo.maxTraceDepth = 1;
    OptixPipeline pipeline; logSize = sizeof(log);
    ASSERT_EQ(optixPipelineCreate(octx, &pco, &plo, groups, 3, log, &logSize, &pipeline), OPTIX_SUCCESS) << log;

    EmptyRecord rgR, msR, hgR;
    optixSbtRecordPackHeader(rg, &rgR); optixSbtRecordPackHeader(ms, &msR); optixSbtRecordPackHeader(hg, &hgR);
    CUdeviceptr d_rg = upload(&rgR, sizeof(rgR)), d_ms = upload(&msR, sizeof(msR)), d_hg = upload(&hgR, sizeof(hgR));
    OptixShaderBindingTable sbt = {};
    sbt.raygenRecord = d_rg;
    sbt.missRecordBase = d_ms; sbt.missRecordStrideInBytes = sizeof(EmptyRecord); sbt.missRecordCount = 1;
    sbt.hitgroupRecordBase = d_hg; sbt.hitgroupRecordStrideInBytes = sizeof(EmptyRecord); sbt.hitgroupRecordCount = 1;

    struct F3 { float x, y, z; };
    const unsigned n = 4;
    F3 q[n] = { {0,0,0}, {5,0,0}, {10,0,0}, {19.7f,0,0} };
    CUdeviceptr d_q = upload(q, sizeof(q));
    CUdeviceptr d_counts = 0; p_cuMemAlloc(&d_counts, n * sizeof(unsigned));

    struct Params { OptixTraversableHandle handle; CUdeviceptr queries, counts; unsigned n; CUdeviceptr boxes; };
    Params p = {}; p.handle = gas; p.queries = d_q; p.counts = d_counts; p.n = n; p.boxes = d_boxes;
    CUdeviceptr d_params = upload(&p, sizeof(p));

    CUstream stream; p_cuStreamCreate(&stream, 0);
    ASSERT_EQ(optixLaunch(pipeline, stream, d_params, sizeof(Params), &sbt, n, 1, 1), OPTIX_SUCCESS);
    p_cuStreamSynchronize(stream);

    unsigned counts[n] = {99,99,99,99};
    p_cuMemcpyDtoH(counts, d_counts, sizeof(counts));
    EXPECT_EQ(counts[0], 1u); EXPECT_EQ(counts[1], 0u);
    EXPECT_EQ(counts[2], 1u); EXPECT_EQ(counts[3], 1u);
}

// Triangle nearest hit on the RT cores == the software oracle (t=6/prim=1/u=v=0.25).
TEST(OptiXRayQueryProbe, triangleNearestHitMatchesSoftwareOracle) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    std::string ptx = readPtx("optix_tri.ptx");
    ASSERT_FALSE(ptx.empty()) << "test/xpu/optix/optix_tri.ptx not found";

    OptixDeviceContext octx = glueContext();
    ASSERT_NE(octx, nullptr) << "OptiX/CUDA context unavailable from the runtime glue";

    float verts[18] = { 0,0,2, 1,0,2, 0,1,2,   0,0,4, 1,0,4, 0,1,4 };
    CUdeviceptr d_verts = upload(verts, sizeof(verts));
    unsigned int triFlags[1] = { OPTIX_GEOMETRY_FLAG_NONE };
    OptixBuildInput bi = {};
    bi.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
    bi.triangleArray.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
    bi.triangleArray.vertexStrideInBytes = sizeof(float) * 3;
    bi.triangleArray.numVertices = 6;
    bi.triangleArray.vertexBuffers = &d_verts;
    bi.triangleArray.flags = triFlags;
    bi.triangleArray.numSbtRecords = 1;
    OptixAccelBuildOptions ao = {};
    ao.buildFlags = OPTIX_BUILD_FLAG_NONE; ao.operation = OPTIX_BUILD_OPERATION_BUILD;
    OptixAccelBufferSizes sizes;
    ASSERT_EQ(optixAccelComputeMemoryUsage(octx, &ao, &bi, 1, &sizes), OPTIX_SUCCESS);
    CUdeviceptr d_temp = 0, d_out = 0;
    p_cuMemAlloc(&d_temp, sizes.tempSizeInBytes);
    p_cuMemAlloc(&d_out, sizes.outputSizeInBytes);
    OptixTraversableHandle gas = 0;
    ASSERT_EQ(optixAccelBuild(octx, 0, &ao, &bi, 1, d_temp, sizes.tempSizeInBytes,
                              d_out, sizes.outputSizeInBytes, &gas, nullptr, 0), OPTIX_SUCCESS);

    OptixModuleCompileOptions mco = {};
    OptixPipelineCompileOptions pco = {};
    pco.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
    pco.numPayloadValues = 0; pco.numAttributeValues = 2;
    pco.pipelineLaunchParamsVariableName = "params";
    pco.usesPrimitiveTypeFlags = OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE;
    char log[4096]; size_t logSize = sizeof(log);
    OptixModule mod;
    ASSERT_EQ(optixModuleCreate(octx, &mco, &pco, ptx.c_str(), ptx.size(),
                                log, &logSize, &mod), OPTIX_SUCCESS) << log;

    OptixProgramGroupOptions pgo = {};
    OptixProgramGroupDesc rgD = {}; rgD.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    rgD.raygen.module = mod; rgD.raygen.entryFunctionName = "__raygen__nearest";
    OptixProgramGroupDesc msD = {}; msD.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    msD.miss.module = mod; msD.miss.entryFunctionName = "__miss__nearest";
    OptixProgramGroupDesc hgD = {}; hgD.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    hgD.hitgroup.moduleCH = mod; hgD.hitgroup.entryFunctionNameCH = "__closesthit__nearest";
    OptixProgramGroup rg, ms, hg;
    logSize = sizeof(log); ASSERT_EQ(optixProgramGroupCreate(octx, &rgD, 1, &pgo, log, &logSize, &rg), OPTIX_SUCCESS);
    logSize = sizeof(log); ASSERT_EQ(optixProgramGroupCreate(octx, &msD, 1, &pgo, log, &logSize, &ms), OPTIX_SUCCESS);
    logSize = sizeof(log); ASSERT_EQ(optixProgramGroupCreate(octx, &hgD, 1, &pgo, log, &logSize, &hg), OPTIX_SUCCESS);

    OptixProgramGroup groups[] = { rg, ms, hg };
    OptixPipelineLinkOptions plo = {}; plo.maxTraceDepth = 1;
    OptixPipeline pipeline; logSize = sizeof(log);
    ASSERT_EQ(optixPipelineCreate(octx, &pco, &plo, groups, 3, log, &logSize, &pipeline), OPTIX_SUCCESS) << log;

    EmptyRecord rgR, msR, hgR;
    optixSbtRecordPackHeader(rg, &rgR); optixSbtRecordPackHeader(ms, &msR); optixSbtRecordPackHeader(hg, &hgR);
    CUdeviceptr d_rg = upload(&rgR, sizeof(rgR)), d_ms = upload(&msR, sizeof(msR)), d_hg = upload(&hgR, sizeof(hgR));
    OptixShaderBindingTable sbt = {};
    sbt.raygenRecord = d_rg;
    sbt.missRecordBase = d_ms; sbt.missRecordStrideInBytes = sizeof(EmptyRecord); sbt.missRecordCount = 1;
    sbt.hitgroupRecordBase = d_hg; sbt.hitgroupRecordStrideInBytes = sizeof(EmptyRecord); sbt.hitgroupRecordCount = 1;

    float ht = -9; unsigned hp = 99; float hu = -9, hv = -9; unsigned hh = 99;
    CUdeviceptr d_t = upload(&ht,4), d_p = upload(&hp,4), d_u = upload(&hu,4), d_v = upload(&hv,4), d_h = upload(&hh,4);
    struct ParamsTri { OptixTraversableHandle handle; CUdeviceptr outT, outPrim, outU, outV, outHit; };
    ParamsTri p = {}; p.handle = gas; p.outT = d_t; p.outPrim = d_p; p.outU = d_u; p.outV = d_v; p.outHit = d_h;
    CUdeviceptr d_params = upload(&p, sizeof(p));

    CUstream stream; p_cuStreamCreate(&stream, 0);
    ASSERT_EQ(optixLaunch(pipeline, stream, d_params, sizeof(ParamsTri), &sbt, 1, 1, 1), OPTIX_SUCCESS);
    p_cuStreamSynchronize(stream);

    p_cuMemcpyDtoH(&ht, d_t, 4); p_cuMemcpyDtoH(&hp, d_p, 4);
    p_cuMemcpyDtoH(&hu, d_u, 4); p_cuMemcpyDtoH(&hv, d_v, 4); p_cuMemcpyDtoH(&hh, d_h, 4);
    EXPECT_EQ(hh, 1u);
    EXPECT_NEAR(ht, 6.0f, 1e-3f);
    EXPECT_EQ(hp, 1u);
    EXPECT_NEAR(hu, 0.25f, 1e-3f);
    EXPECT_NEAR(hv, 0.25f, 1e-3f);
}

#endif // CAJETA_TEST_HAS_OPTIX
