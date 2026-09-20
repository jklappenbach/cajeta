//
// AmdgpuCoopIntAccumTests — the INTEGER epilogue accumulate
// (`scaledAccumI32`) and the scalar-column rank-1 (`rank1AccumS`).
//
// The k-quant GEMM folds its 6-bit sub-block scale into an int32
// accumulator per 32-k step and drains to float ONCE per 256-k block,
// so the float epilogue (convert + two multiplies + a rank-1 term per
// fragment element) stops running per sub-block. The fold has to be the
// FULL-rate 24-bit multiply — a plain i32 multiply lowers to
// quarter-rate v_mul_lo_u32 and gives the saving straight back — which
// is what the ISA case measures.
//
// Compile-level only (the AmdgpuCoopEpilogueTests discipline): AMDGCN
// ISA emission needs no device.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta_test::CajetaJit;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using namespace cajeta::xpu::amd;

namespace {

int32_t runI32On(cajeta::xpu::Backend be, const std::string& src,
                 std::string* errOut) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D", o);
    auto fn = jit->lookup<int32_t (*)()>("run");
    int32_t r = fn();
    *errOut = testing::internal::GetCapturedStderr();
    return r;
}

// The consumer shape: two 32-k sub-blocks folded into an int32
// accumulator with a per-lane 6-bit integer scale, then ONE float drain
// (scaledAccumIntoS) plus the separable min term (rank1AccumS).
const char* kIntAccumSrc =
    "package test;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n"
    "public final class D {\n"
    "    @Kernel\n"
    "    public static void iepi(KernelBuffer<float32> y,\n"
    "            KernelBuffer<int8> a, KernelBuffer<int8> b,\n"
    "            KernelBuffer<float32> rf, KernelBuffer<int32> sc) {\n"
    "        Shared<float32> rowF = shared float32[16];\n"
    "        Shared<float32> rowG = shared float32[16];\n"
    "        uint32 lane = KernelThread.x();\n"
    "        if (lane < 16) { rowF[lane] = rf[lane]; rowG[lane] = rf[lane]; }\n"
    "        Barrier.workgroup();\n"
    "        int32 colS = sc[lane % 16];\n"
    "        float32 xs = rf[lane % 16];\n"
    "        float32 g = 0.0f - xs;\n"
    "        CooperativeMatrix<int8,16,16,0> ma;\n"
    "        CooperativeMatrix<int8,16,16,1> mb;\n"
    "        CooperativeMatrix<int32,16,16,2> mc;\n"
    "        CooperativeMatrix<int32,16,16,2> iacc;\n"
    "        CooperativeMatrix<float32,16,16,2> facc;\n"
    "        facc.splat(0.0f);\n"
    "        iacc.splat(0);\n"
    "        mc.splat(0);\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        mb.load(b, 0, 0, 16);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.scaledAccumI32(iacc, colS);\n"
    "        mc.splat(0);\n"
    "        ma.load(a, 256, 0, 16);\n"
    "        mb.load(b, 256, 0, 16);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.scaledAccumI32(iacc, colS);\n"
    "        iacc.scaledAccumIntoS(facc, rowF, xs);\n"
    "        facc.rank1AccumS(rowG, g);\n"
    "        facc.store(y, 0, 0, 16);\n"
    "    }\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler, const char* source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_iaccum_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "D.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_iaccum_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule((base / "test" / "D.cajeta").string(),
                                   base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

std::string isaOf(const char* source, const char* kernelName) {
    Compiler compiler;
    auto module = compileForInspection(compiler, source);
    auto method = findMethod(module->getStructures()["test.D"], kernelName);
    if (!method) return {};
    auto tm = createAmdgpuTargetMachine("gfx1151");
    if (!tm) return {};
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_iaccum_isa", ctx);
    configureDeviceModule(dev, *tm);
    if (!lowerKernel(method, dev)) return {};
    return cajeta::xpu::amd::emitIsa(dev, *tm);
}

} // namespace

// The integer fold + the scalar-column drain LOWER NATIVELY on amdgpu:
// no skip note and no demote-to-portable note. int8 16x16 is native
// RDNA3 WMMA and the backend implements both seams, so nothing may fall
// back.
TEST(AmdgpuCoopIntAccumTests, intAccumVerbsLowerNativelyOnAmdgpu) {
    std::string err;
    EXPECT_EQ(runI32On(cajeta::xpu::Backend::Amdgpu, kIntAccumSrc, &err), 1);
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "the integer-accumulate kernel must lower, not skip:\n" << err;
    EXPECT_EQ(err.find("[mma-tiering]"), std::string::npos)
        << "int8 + integer epilogue is native on amdgpu - a tier note "
           "means it demoted:\n" << err;
}

// A backend without native epilogue support (SPIR-V) demotes the tiles, and
// the replicated software tile then REJECTS the kernel by name rather than
// taking the calling work-item's factor for all sixteen columns, which would
// be a plausible wrong answer.
//
// The anchor moved off the literal "NATIVE-ONLY" when the contract was
// restated in tile coordinates: that word named a TIER, when the real line
// is whether a backend has a wave to distribute the per-lane slices across.
// NVPTX has one and supports these verbs, which "native-only" could not
// express. What the refusal must still do is state the contract and name the
// spelling that works.
TEST(AmdgpuCoopIntAccumTests, intAccumVerbsRejectLoudlyOffNative) {
    std::string err;
    EXPECT_EQ(runI32On(cajeta::xpu::Backend::Spirv, kIntAccumSrc, &err), 1);
    EXPECT_NE(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "off-native the integer verbs must SKIP the kernel loudly:\n"
        << err;
    EXPECT_NE(err.find("lane L supplies the factor for column"),
              std::string::npos)
        << "the skip must state the contract it could not meet:\n" << err;
    EXPECT_NE(err.find("Shared-vector"), std::string::npos)
        << "the skip must name the spelling that works here:\n" << err;
}

// The whole point of the verb: the fold must be the FULL-rate 24-bit
// multiply. v_mad_i32_i24 is the folded mul24+add; v_mul_i32_i24 is the
// unfolded pair. v_mul_lo_u32 would be the quarter-rate 32-bit form that
// gives the epilogue saving back.
TEST(AmdgpuCoopIntAccumTests, intAccumEmitsTwentyFourBitMultiply) {
    std::string isa = isaOf(kIntAccumSrc, "iepi");
    ASSERT_FALSE(isa.empty()) << "the integer-accumulate kernel failed to "
                                 "lower/emit gfx1151 ISA";
    EXPECT_EQ(isa.find("Cannot select"), std::string::npos) << isa;
    const bool mad24 = isa.find("v_mad_i32_i24") != std::string::npos;
    const bool mul24 = isa.find("v_mul_i32_i24") != std::string::npos;
    EXPECT_TRUE(mad24 || mul24)
        << "expected the 24-bit multiply in the integer epilogue; a "
           "32-bit v_mul_lo_u32 is quarter rate and loses the saving:\n"
        << isa;
}
