//
// CajetaXPU M2 Phase 3 (3-B) — NVPTX → OptiX count-shape program EMISSION.
//
// GPU-free + SDK-free: proves the NVPTX backend emits the OptiX RT-core program set
// (raygen / intersection / anyhit / miss + the `params` const global) for the
// canonical AABB candidate-count RayQuery kernel, with the `_optix_trace_typed_32`
// inline-asm call OptiX's module compiler recognizes (validated end-to-end on the
// 4090 in Phase 1). The programs are a SEPARATE PTX module (the `_optix_*` asm is
// rejected by ptxas), fed to optixModuleCreate at launch — so this is emitPtx only,
// no assembleCubin, no device. Also checks a non-canonical signature throws XPU-N04.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxOptixRayQuery.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"
#include "cajeta/error/Exception.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace cajeta::xpu::nvidia;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// Parse + resolve a cajeta source to a module (no codegen) — same pattern as the
// other XPU inspection tests; gives a resolved @Kernel method to lower.
CajetaModulePtr compileForInspection(Compiler& compiler, const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_optix_" + std::to_string(rng()));
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
                 / ("cajeta_xpu_optix_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

// The canonical AABB candidate-count kernel (the kRqMinDriver shape): an AS, three
// KernelBuffer origins, a KernelBuffer<uint32> output, a count scalar.
const char* kCountKernel =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class RqCount {\n"
    "    @Kernel\n"
    "    public static void countHits(AccelerationStructure scene,\n"
    "                                 KernelBuffer<float32> qx, KernelBuffer<float32> qy,\n"
    "                                 KernelBuffer<float32> qz, KernelBuffer<uint32> out,\n"
    "                                 uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
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

// The canonical triangle nearest-hit kernel (the kNearestDriver shape): an AS, a
// KernelBuffer<float32> outT, a KernelBuffer<uint32> outI; confirms triangle candidates and
// reads committed getters; a compile-time-constant ray (one with a unary-minus dz).
const char* kNearestKernel =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class RqNear {\n"
    "    @Kernel\n"
    "    public static void nearest(AccelerationStructure scene,\n"
    "                               KernelBuffer<float32> outT, KernelBuffer<uint32> outI) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i == 0) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          0.25f, 0.25f, 10.0f, 0.0f,\n"
    "                          0.0f, 0.0f, -1.0f, 100.0f);\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 0) { rq.confirmIntersection(); }\n"
    "            }\n"
    "            outT[0] = rq.committedDistance();\n"
    "            outI[0] = rq.committedType();\n"
    "            outI[1] = rq.committedPrimitiveIndex();\n"
    "        }\n"
    "    }\n"
    "}\n";

// The canonical triangle candidate-getter kernel (the kBaryDriver shape): an AS and
// a single KernelBuffer<float32> out; reads candidate distance + barycentrics inside the
// proceed loop (no confirm); a compile-time-constant ray.
const char* kBaryKernel =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class RqBary {\n"
    "    @Kernel\n"
    "    public static void getBary(AccelerationStructure scene, KernelBuffer<float32> out) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i == 0) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          0.25f, 0.25f, 5.0f, 0.0f,\n"
    "                          0.0f, 0.0f, -1.0f, 100.0f);\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 0) {\n"
    "                    out[0] = rq.candidateDistance();\n"
    "                    out[1] = rq.candidateBarycentricU();\n"
    "                    out[2] = rq.candidateBarycentricV();\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "}\n";

// The committed-triangle per-launch kernel (the kFrontDriver shape): an AS, two
// KernelBuffer<float32> ray-component buffers (origin-z + dir-z, indexed by launch index),
// a KernelBuffer<uint32> out, a count; confirms triangle candidates and reads committed
// front-face. Per-launch DYNAMIC ray (oz[i] origin-z, dz[i] dir-z; x/y + tMin/tMax
// constant) — exercises the const-or-buffer[i] ray resolver.
const char* kCommittedTriKernel =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class RqFront {\n"
    "    @Kernel\n"
    "    public static void getFront(AccelerationStructure scene,\n"
    "                                KernelBuffer<float32> oz, KernelBuffer<float32> dz,\n"
    "                                KernelBuffer<uint32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          0.25f, 0.25f, oz[i], 0.0f,\n"
    "                          0.0f, 0.0f, dz[i], 100.0f);\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 0) { rq.confirmIntersection(); }\n"
    "            }\n"
    "            uint32 f = 0;\n"
    "            if (rq.committedType() == 1) {\n"
    "                if (rq.committedFrontFace()) { f = 1; } else { f = 2; }\n"
    "            }\n"
    "            out[i] = f;\n"
    "        }\n"
    "    }\n"
    "}\n";

// A ray-query kernel whose signature is NOT the canonical count shape (one origin
// buffer, not three) — must be rejected with XPU-N04.
const char* kNonCanonicalKernel =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class RqOther {\n"
    "    @Kernel\n"
    "    public static void odd(AccelerationStructure scene,\n"
    "                           KernelBuffer<float32> outT, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { outT[i] = 0.0f; }\n"
    "    }\n"
    "}\n";

// A kernel that genuinely USES the ray query (a proceed loop + committed getters) but
// with a non-canonical arity — (AS, one KernelBuffer, count) is neither the nearest-hit
// (AS, 2 KernelBuffer) nor the committed-triangle (AS, 3 KernelBuffer, count) shape. It is the
// "unsupported general-loop case" (cuda-plan 4a): classifyRayQueryShape returns
// Unsupported, so NvptxRegistration emits NO OptiX program and the kernel runs on the
// software floor (the M3 graceful fallback — no fault). Distinct from
// kNonCanonicalKernel, which is not a ray query at all and drives the emit-level guard.
const char* kUnsupportedRqKernel =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class RqUnsupShape {\n"
    "    @Kernel\n"
    "    public static void odd(AccelerationStructure scene,\n"
    "                           KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          0.25f, 0.25f, 10.0f, 0.0f,\n"
    "                          0.0f, 0.0f, -1.0f, 100.0f);\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 0) { rq.confirmIntersection(); }\n"
    "            }\n"
    "            out[i] = rq.committedDistance();\n"   // committed family, but arity (1 buf) is non-canonical
    "        }\n"
    "    }\n"
    "}\n";

} // namespace

// The canonical count kernel emits the full OptiX program set + the `params` block.
TEST(XpuNvptxOptixEmitTests, countShapeEmitsOptixPrograms) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kCountKernel, "test.RqCount");
    auto klass = module->getStructures()["test.RqCount"];
    ASSERT_NE(klass, nullptr);
    auto countHits = findMethod(klass, "countHits");
    ASSERT_NE(countHits, nullptr);
    EXPECT_TRUE(nvptxKernelUsesRayQuery(countHits));

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);

    llvm::LLVMContext deviceCtx;
    llvm::Module optixModule("rq_count_optix", deviceCtx);
    configureDeviceModule(optixModule, *tm);

    std::string raygen = emitOptixCountModule(countHits, optixModule);
    EXPECT_EQ(raygen, "__raygen__countHits");

    std::string ptx = emitPtx(optixModule, *tm);
    ASSERT_FALSE(ptx.empty());

    // The four OptiX program entries (PTX .entry functions).
    EXPECT_NE(ptx.find(".visible .entry __raygen__countHits"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find(".visible .entry __intersection__countHits"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find(".visible .entry __anyhit__countHits"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find(".visible .entry __miss__countHits"), std::string::npos) << ptx;
    // The optixTrace ABI call + the launch-index / counting intrinsics OptiX matches.
    EXPECT_NE(ptx.find("_optix_trace_typed_32"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("_optix_get_launch_index_x"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("_optix_set_payload"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("_optix_ignore_intersection"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("_optix_report_intersection_0"), std::string::npos) << ptx;
    // The launch-params block OptiX populates (pipelineLaunchParamsVariableName).
    EXPECT_NE(ptx.find(".const"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("params"), std::string::npos) << ptx;
}

// The triangle nearest-hit kernel is classified NearestTri and emits the
// raygen / closesthit / miss program set + the `params` block (built-in triangle
// traversal — no intersection/anyhit). The ray literals (incl. the unary-minus dz)
// are baked into raygen via the optixTrace ABI call.
TEST(XpuNvptxOptixEmitTests, nearestShapeEmitsClosesthitPrograms) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kNearestKernel, "test.RqNear");
    auto klass = module->getStructures()["test.RqNear"];
    ASSERT_NE(klass, nullptr);
    auto nearest = findMethod(klass, "nearest");
    ASSERT_NE(nearest, nullptr);
    EXPECT_TRUE(nvptxKernelUsesRayQuery(nearest));
    EXPECT_EQ((int) classifyRayQueryShape(nearest), (int) OptixRqShape::NearestTri);

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module optixModule("rq_near_optix", deviceCtx);
    configureDeviceModule(optixModule, *tm);

    std::string raygen = emitOptixNearestModule(nearest, optixModule);
    EXPECT_EQ(raygen, "__raygen__nearest");

    std::string ptx = emitPtx(optixModule, *tm);
    ASSERT_FALSE(ptx.empty());

    EXPECT_NE(ptx.find(".visible .entry __raygen__nearest"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find(".visible .entry __closesthit__nearest"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find(".visible .entry __miss__nearest"), std::string::npos) << ptx;
    // No custom intersection/anyhit for built-in triangles.
    EXPECT_EQ(ptx.find("__intersection__nearest"), std::string::npos) << ptx;
    EXPECT_EQ(ptx.find("__anyhit__nearest"), std::string::npos) << ptx;
    // optixTrace ABI + the committed getters closesthit reads.
    EXPECT_NE(ptx.find("_optix_trace_typed_32"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("_optix_get_ray_tmax"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("_optix_read_primitive_idx"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("params"), std::string::npos) << ptx;
}

// The triangle candidate-getter kernel is classified BaryCandidate and emits the
// raygen / anyhit / miss program set + the `params` block (built-in triangle
// traversal — anyhit reads the candidate's tmax + barycentrics, no closesthit/
// intersection). The ray literals are baked into raygen.
TEST(XpuNvptxOptixEmitTests, baryShapeEmitsAnyhitGetters) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kBaryKernel, "test.RqBary");
    auto klass = module->getStructures()["test.RqBary"];
    ASSERT_NE(klass, nullptr);
    auto getBary = findMethod(klass, "getBary");
    ASSERT_NE(getBary, nullptr);
    EXPECT_TRUE(nvptxKernelUsesRayQuery(getBary));
    EXPECT_EQ((int) classifyRayQueryShape(getBary), (int) OptixRqShape::BaryCandidate);

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module optixModule("rq_bary_optix", deviceCtx);
    configureDeviceModule(optixModule, *tm);

    std::string raygen = emitOptixBaryModule(getBary, optixModule);
    EXPECT_EQ(raygen, "__raygen__getBary");

    std::string ptx = emitPtx(optixModule, *tm);
    ASSERT_FALSE(ptx.empty());

    EXPECT_NE(ptx.find(".visible .entry __raygen__getBary"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find(".visible .entry __anyhit__getBary"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find(".visible .entry __miss__getBary"), std::string::npos) << ptx;
    // No closesthit/intersection — the kernel never commits.
    EXPECT_EQ(ptx.find("__closesthit__getBary"), std::string::npos) << ptx;
    EXPECT_EQ(ptx.find("__intersection__getBary"), std::string::npos) << ptx;
    // optixTrace ABI + the candidate getters anyhit reads + ignore-intersection.
    EXPECT_NE(ptx.find("_optix_trace_typed_32"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("_optix_get_ray_tmax"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("_optix_get_triangle_barycentrics"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("_optix_ignore_intersection"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("params"), std::string::npos) << ptx;
}

// The committed-triangle per-launch kernel is classified CommittedTri and emits the
// raygen / closesthit / miss program set + the `params` block. The front-face body
// makes closesthit read the hit-kind decoders; the per-launch dynamic ray (oz[i],
// dz[i]) makes raygen load from the ray-component buffers. No anyhit/intersection.
TEST(XpuNvptxOptixEmitTests, committedTriShapeEmitsClosesthitFrontFace) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kCommittedTriKernel, "test.RqFront");
    auto klass = module->getStructures()["test.RqFront"];
    ASSERT_NE(klass, nullptr);
    auto getFront = findMethod(klass, "getFront");
    ASSERT_NE(getFront, nullptr);
    EXPECT_TRUE(nvptxKernelUsesRayQuery(getFront));
    EXPECT_EQ((int) classifyRayQueryShape(getFront), (int) OptixRqShape::CommittedTri);

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module optixModule("rq_front_optix", deviceCtx);
    configureDeviceModule(optixModule, *tm);

    std::string raygen = emitOptixCommittedTriModule(getFront, optixModule);
    EXPECT_EQ(raygen, "__raygen__getFront");

    std::string ptx = emitPtx(optixModule, *tm);
    ASSERT_FALSE(ptx.empty());

    EXPECT_NE(ptx.find(".visible .entry __raygen__getFront"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find(".visible .entry __closesthit__getFront"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find(".visible .entry __miss__getFront"), std::string::npos) << ptx;
    // Built-in triangle: no custom intersection/anyhit.
    EXPECT_EQ(ptx.find("__intersection__getFront"), std::string::npos) << ptx;
    EXPECT_EQ(ptx.find("__anyhit__getFront"), std::string::npos) << ptx;
    // optixTrace ABI + the front-face hit-kind decoders closesthit reads.
    EXPECT_NE(ptx.find("_optix_trace_typed_32"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("_optix_get_hit_kind"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("_optix_get_backface_from_hit_kind"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("params"), std::string::npos) << ptx;
}

// A ray-query kernel whose signature is not the canonical count shape throws XPU-N04
// (never a silent miscompile).
TEST(XpuNvptxOptixEmitTests, nonCanonicalSignatureThrowsN04) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kNonCanonicalKernel, "test.RqOther");
    auto klass = module->getStructures()["test.RqOther"];
    ASSERT_NE(klass, nullptr);
    auto odd = findMethod(klass, "odd");
    ASSERT_NE(odd, nullptr);

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module optixModule("rq_other_optix", deviceCtx);
    configureDeviceModule(optixModule, *tm);

    try {
        emitOptixCountModule(odd, optixModule);
        FAIL() << "expected XPU-N04 for a non-canonical ray-query signature";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "XPU-N04") << "wrong diagnostic: " << e.getMessage();
    }
}

// A real RayQuery kernel that is not one of the four canonical shapes (committed
// getters + a non-canonical arity) classifies as Unsupported — the cuda-plan 4a
// "unsupported general-loop case." Registration emits no OptiX program for it, so it
// runs on the retained software floor (the M3 graceful fallback), never miscompiled.
