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
#include "cajeta/xpu/nvidia/NvptxOptixRayQuery.h"  // emitOptixCountModule (M2 Phase 3)
#include "cajeta/xpu/nvidia/NvptxBackend.h"        // createNvptxTargetMachine/emitPtx
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <random>
#include <string>
#include <fstream>
#include <sstream>

// windows.h comes LAST, after every cajeta/LLVM header. Two collisions force
// this ordering + the guards:
//   * WIN32_LEAN_AND_MEAN drops the OLE/COM headers (rpcndr.h, objidl.h,
//     wtypes.h) that declare a global `byte`, which otherwise becomes ambiguous
//     against std::byte (the cajeta headers do `using namespace std`).
//   * NOMINMAX drops the min/max macros.
//   * Including it AFTER the ANTLR-generated CajetaLexer.h (pulled in via
//     Compiler.h) avoids windows.h's `CONST` macro mangling that header's
//     `CONST = 8` token enumerator.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
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
// M2 Phase 2 (R4): the underlying CUDA context behind the OptiX device context, and the
// cajeta runtime's CUDA context. Post-R4 both are the per-device primary — they must be
// equal, proving the AS build / pipeline / launch share ONE context.
extern "C" void*    cajeta_xpu_optix_cuda_context(void);
extern "C" void*    cajeta_xpu_cuda_context(void);
// M2 Phase 3-C: the AS's boxes (device ptr) for the count intersection program, and
// the OptiX count-pipeline launch from emitted PTX (see NvptxOptixRayQuery.h contract).
extern "C" uint64_t cajeta_xpu_optix_accel_boxes(int64_t handle);
extern "C" int      cajeta_xpu_optix_launch(const char* ptx, uint64_t ptxLen,
                                            const char* raygenName, const char* isName,
                                            const char* anyhitName, const char* missName,
                                            const void* paramsHost, uint64_t paramsLen,
                                            uint32_t width);

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

// Parse + resolve a cajeta source to a resolved @Kernel method (no codegen), so the
// M2 OptiX emitter can lower it. Same pattern as the XPU inspection tests.
cajeta::CajetaModulePtr compileForInspection(cajeta::Compiler& compiler,
                                             const std::string& source,
                                             const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_optix_dev_" + std::to_string(rng()));
    std::filesystem::create_directories(base);
    std::filesystem::path rel;
    size_t start = 0;
    for (size_t i = 0; i <= fqClassName.size(); ++i) {
        if (i == fqClassName.size() || fqClassName[i] == '.') {
            rel /= fqClassName.substr(start, i - start);
            start = i + 1;
        }
    }
    rel += ".cajeta";
    auto full = base / rel;
    std::filesystem::create_directories(full.parent_path());
    std::ofstream out(full); out << source; out.close();
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_optix_dev_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass, const std::string& name) {
    for (auto& [k, mth] : klass->getMethods())
        if (mth && mth->getName() == name) return mth;
    return nullptr;
}

// The canonical AABB candidate-count kernel (the kRqMinDriver shape).
const char* kCountKernelSrc =
    "package test;\n"
    "import cajeta.gpu.core.AccelerationStructure;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.RayQuery;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class RqCount {\n"
    "    @Kernel\n"
    "    public static void countHits(AccelerationStructure scene,\n"
    "                                 Buffer<float32> qx, Buffer<float32> qy,\n"
    "                                 Buffer<float32> qz, Buffer<uint32> out,\n"
    "                                 uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255, qx[i], qy[i], qz[i], 0.0f,\n"
    "                          0.0f, 0.0f, 1.0f, 0.001f);\n"
    "            uint32 c = 0;\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 1) { c = c + 1; }\n"
    "            }\n"
    "            out[i] = c;\n"
    "        }\n"
    "    }\n"
    "}\n";

// The compiler ↔ runtime launch-params layout (NvptxOptixRayQuery.h contract):
// { handle, originX, originY, originZ, out, n, boxes }. Natural alignment matches the
// emitted LLVM struct {i64,i64,i64,i64,i64,i32,i64} (n at +40, boxes at +48, size 56).
struct RqCountParams {
    uint64_t handle;
    uint64_t ox, oy, oz, out;
    uint32_t n;
    uint64_t boxes;
};

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

// Module + program-groups + pipeline + SBT + launch of the count programs in `ptx`
// against an already-built traversable `gas` (the boxes live in `d_boxes` for the
// intersection point-in-box test), filling out[4] with the per-query candidate counts.
// Split out so the AS can come from EITHER an inline optixAccelBuild (the M0/M1 PTX
// fixtures) OR the runtime glue provider (the M2 Phase-2 unification probe) — the
// launch path is identical. ASSERT_* on fatal failures (gtest aborts the caller).
void launchCountAgainst(OptixDeviceContext octx, const std::string& ptx,
                        OptixTraversableHandle gas, CUdeviceptr d_boxes, unsigned out[4]) {
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

    p_cuMemcpyDtoH(out, d_counts, n * sizeof(unsigned));
}

// The full AABB candidate-count pipeline with the AS built INLINE (optixAccelBuild) on
// the glue's context. Shared by the nvcc-PTX fixture and the LLVM-emitted-PTX fixture
// (M2 Phase 1) — both differ ONLY in the PTX producer. `out[4]` receives the counts.
void runAabbCountPipeline(const std::string& ptx, unsigned out[4]) {
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

    launchCountAgainst(octx, ptx, gas, d_boxes, out);
}

// AABB candidate count on the RT cores == the software oracle's {1,0,1,1}.
TEST(OptiXRayQueryProbe, aabbCandidateCountMatchesSoftwareOracle) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    std::string ptx = readPtx("optix_progs.ptx");
    ASSERT_FALSE(ptx.empty()) << "test/xpu/optix/optix_progs.ptx not found";

    unsigned counts[4] = {99,99,99,99};
    runAabbCountPipeline(ptx, counts);
    EXPECT_EQ(counts[0], 1u); EXPECT_EQ(counts[1], 0u);
    EXPECT_EQ(counts[2], 1u); EXPECT_EQ(counts[3], 1u);
}

// M2 Phase-1 feasibility spike, formalized: the SAME count pipeline driven by PTX
// the fork's NVPTX backend emitted from a hand-authored .ll (the representative path
// — cajeta lowers to LLVM IR, never via clang-CUDA). Proves optixModuleCreate +
// optixPipelineCreate accept LLVM-emitted PTX (recognizing the `_optix_trace_typed_32`
// inline-asm call) and that it runs on the RT cores matching the oracle. The fixture
// optix_progs_ll.ptx is checked in (regen: test/xpu/optix/gen_optix_ll.py -> .ll ->
// the fork's llc -mcpu=sm_60). See documents/gpu-rayquery-optix/rayquery-optix-m2-*.
TEST(OptiXRayQueryProbe, aabbCandidateCountFromLlvmEmittedPtx) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    std::string ptx = readPtx("optix_progs_ll.ptx");
    ASSERT_FALSE(ptx.empty()) << "test/xpu/optix/optix_progs_ll.ptx not found "
                                 "(regen via gen_optix_ll.py + the fork's llc)";

    unsigned counts[4] = {99,99,99,99};
    runAabbCountPipeline(ptx, counts);
    EXPECT_EQ(counts[0], 1u); EXPECT_EQ(counts[1], 0u);
    EXPECT_EQ(counts[2], 1u); EXPECT_EQ(counts[3], 1u);
}

// M2 Phase 2 — context unification (R4). Two things, together: (1) an OptiX AS built by
// the runtime GLUE PROVIDER (cajeta_xpu_optix_accel_build_aabbs — the exact call the
// CUDA noun provider makes, vs. the inline optixAccelBuild the fixtures above use) is
// traversable by a launch and matches the oracle; (2) the runtime's CUDA context, the
// glue's CUDA context, and that launch all coincide. Post-R4 the runtime retains the
// per-device PRIMARY context — the same singleton the glue retains — so
// cajeta_xpu_cuda_context() == cajeta_xpu_optix_cuda_context(). This resolves the M1
// split (glue=primary, runtime=its own cuCtxCreate ctx) that blocked traversal, and is
// the precondition for the Phase-3 verb to optixLaunch on the runtime's context.
TEST(OptiXRayQueryProbe, glueBuiltAsTraversableOnUnifiedContext) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    if (!cajeta_xpu_optix_available()) {
        GTEST_SKIP() << "OptiX runtime unavailable (SDK built in but nvoptix/CUDA absent)";
    }
    std::string ptx = readPtx("optix_progs_ll.ptx");
    ASSERT_FALSE(ptx.empty()) << "test/xpu/optix/optix_progs_ll.ptx not found";

    OptixDeviceContext octx = glueContext();   // also makes the (primary) context current
    ASSERT_NE(octx, nullptr) << "OptiX/CUDA context unavailable from the runtime glue";

    // ONE context: the runtime's CUDA context == the OptiX glue's CUDA context (both the
    // per-device primary). The R4 unification the M1 split lacked.
    void* runtimeCtx = cajeta_xpu_cuda_context();
    void* glueCtx = cajeta_xpu_optix_cuda_context();
    ASSERT_NE(runtimeCtx, nullptr) << "runtime CUDA context unavailable";
    EXPECT_EQ(runtimeCtx, glueCtx) << "runtime and OptiX glue must share ONE CUDA context";

    // Build the AS via the GLUE PROVIDER (not an inline optixAccelBuild).
    float boxes[18] = {
        -0.5f,-0.5f,-0.5f,  0.5f, 0.5f, 0.5f,
         9.5f,-0.5f,-0.5f, 10.5f, 0.5f, 0.5f,
        19.5f,-0.5f,-0.5f, 20.5f, 0.5f, 0.5f,
    };
    int64_t h = cajeta_xpu_optix_accel_build_aabbs(boxes, 3);
    ASSERT_NE(h, 0) << "glue optixAccelBuild (custom prim) returned no handle";
    OptixTraversableHandle gas = (OptixTraversableHandle) cajeta_xpu_optix_traversable(h);
    ASSERT_NE(gas, 0ull) << "null traversable from the glue-built AS";

    // glueContext() left the primary current; the glue build pushes/pops the primary
    // (net no change), so this launch runs on the same context the AS was built on.
    CUdeviceptr d_boxes = upload(boxes, sizeof(boxes));
    unsigned counts[4] = {99,99,99,99};
    launchCountAgainst(octx, ptx, gas, d_boxes, counts);
    EXPECT_EQ(counts[0], 1u); EXPECT_EQ(counts[1], 0u);
    EXPECT_EQ(counts[2], 1u); EXPECT_EQ(counts[3], 1u);

    cajeta_xpu_optix_accel_free(h);
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

// M2 Phase 3-C — the emitted-PTX count pipeline runs on the RT cores via the glue.
// End-to-end through the NEW path: the cajeta count @Kernel → NvptxOptixRayQuery
// emitter → PTX → cajeta_xpu_optix_launch (module/pipeline/SBT + optixLaunch) against
// a provider-built AS, with the params marshalled per the layout contract. This is the
// compiler-emitted analogue of aabbCandidateCountFromLlvmEmittedPtx (which used a
// hand-authored .ll), and the precursor to the full JIT launch wiring (3-D). → {1,0,1,1}.
TEST(OptiXRayQueryProbe, aabbCountFromEmittedPtxRunsOnDevice) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    if (!cajeta_xpu_optix_available()) {
        GTEST_SKIP() << "OptiX runtime unavailable (SDK built in but nvoptix/CUDA absent)";
    }

    // cajeta count kernel → OptiX program PTX (the compiler emitter).
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, kCountKernelSrc, "test.RqCount");
    auto klass = module->getStructures()["test.RqCount"];
    ASSERT_NE(klass, nullptr);
    auto countHits = findMethod(klass, "countHits");
    ASSERT_NE(countHits, nullptr);

    auto tm = cajeta::xpu::nvidia::createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext dctx;
    llvm::Module optixModule("rq_count_optix_dev", dctx);
    cajeta::xpu::nvidia::configureDeviceModule(optixModule, *tm);
    std::string raygen = cajeta::xpu::nvidia::emitOptixCountModule(countHits, optixModule);
    ASSERT_EQ(raygen, "__raygen__countHits");
    std::string ptx = cajeta::xpu::nvidia::emitPtx(optixModule, *tm);
    ASSERT_FALSE(ptx.empty());

    OptixDeviceContext octx = glueContext();   // makes the (primary) context current
    ASSERT_NE(octx, nullptr);

    // AS via the provider — same scene/queries as the M0/M1 count oracle.
    float boxes[18] = {
        -0.5f,-0.5f,-0.5f,  0.5f, 0.5f, 0.5f,
         9.5f,-0.5f,-0.5f, 10.5f, 0.5f, 0.5f,
        19.5f,-0.5f,-0.5f, 20.5f, 0.5f, 0.5f,
    };
    int64_t h = cajeta_xpu_optix_accel_build_aabbs(boxes, 3);
    ASSERT_NE(h, 0);
    uint64_t trav = cajeta_xpu_optix_traversable(h);
    uint64_t d_boxes = cajeta_xpu_optix_accel_boxes(h);
    ASSERT_NE(trav, 0ull);
    ASSERT_NE(d_boxes, 0ull);

    const unsigned n = 4;
    float qx[n] = {0.0f, 5.0f, 10.0f, 19.7f};
    float qy[n] = {0,0,0,0};
    float qz[n] = {0,0,0,0};
    CUdeviceptr d_qx = upload(qx, sizeof(qx)), d_qy = upload(qy, sizeof(qy)),
                d_qz = upload(qz, sizeof(qz));
    CUdeviceptr d_out = 0; p_cuMemAlloc(&d_out, n * sizeof(unsigned));

    RqCountParams p = {};
    p.handle = trav;
    p.ox = d_qx; p.oy = d_qy; p.oz = d_qz; p.out = d_out;
    p.n = n; p.boxes = d_boxes;

    int rc = cajeta_xpu_optix_launch(
        ptx.c_str(), ptx.size(), raygen.c_str(), "__intersection__countHits",
        "__anyhit__countHits", "__miss__countHits", &p, sizeof(p), n);
    EXPECT_EQ(rc, 0) << "cajeta_xpu_optix_launch failed (rc=" << rc << ")";

    unsigned out[n] = {99,99,99,99};
    p_cuMemcpyDtoH(out, d_out, sizeof(out));
    EXPECT_EQ(out[0], 1u); EXPECT_EQ(out[1], 0u);
    EXPECT_EQ(out[2], 1u); EXPECT_EQ(out[3], 1u);

    cajeta_xpu_optix_accel_free(h);
}

#endif // CAJETA_TEST_HAS_OPTIX
