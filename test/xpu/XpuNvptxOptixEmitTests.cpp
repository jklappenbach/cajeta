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
// Buffer origins, a Buffer<uint32> output, a count scalar.
const char* kCountKernel =
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

// A ray-query kernel whose signature is NOT the canonical count shape (one origin
// buffer, not three) — must be rejected with XPU-N04.
const char* kNonCanonicalKernel =
    "package test;\n"
    "import cajeta.gpu.core.AccelerationStructure;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class RqOther {\n"
    "    @Kernel\n"
    "    public static void odd(AccelerationStructure scene,\n"
    "                           Buffer<float32> outT, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) { outT[i] = 0.0f; }\n"
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
