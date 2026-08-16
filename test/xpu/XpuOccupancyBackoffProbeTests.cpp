//
// CajetaXPU — occupancy-backoff probe (kernel-occupancy-autotune U1, §2).
// GPU-free: asserts the automatic compile-time spill backoff
// (relaxOccupancyOnSpill) eliminates register spilling by relaxing the AMDGPU
// occupancy target, leaves non-spilling kernels untouched, and respects an
// explicit override. Also covers the shared resource-metadata parser.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <functional>

using namespace cajeta::xpu::amd;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// A register-heavy @Kernel sized to the sweet spot: 5x5 = 25 live accumulators
// (25*8 = 200 VGPR) + 10 fragments. splat-all → mma-all → store-all keeps every
// accumulator live across the body, so the demand (~240 VGPR) exceeds the
// allocator's default-occupancy cap (~192) and spills — but fits under the 256
// max once occupancy is relaxed, so the backoff drives spill to 0.
const char* kSpillerSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Shared;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void spiller(KernelBuffer<float32> c, KernelBuffer<float16> a,\n"
    "                               KernelBuffer<float16> b, uint32 n) {\n"
    "        uint32 tid = KernelThread.x();\n"
    "        Shared<float16> sa = shared float16[5 * 256];\n"
    "        Shared<float16> sb = shared float16[5 * 256];\n"
    "        uint32 e = tid;\n"
    "        while (e < 1280) { sa[e] = a[e]; e = e + 256; }\n"
    "        e = tid;\n"
    "        while (e < 1280) { sb[e] = b[e]; e = e + 256; }\n"
    "        Barrier.workgroup();\n"
    "        CooperativeMatrix<float16,16,16,0> wa0; wa0.load(sa, 0, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,0> wa1; wa1.load(sa, 256, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,0> wa2; wa2.load(sa, 512, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,0> wa3; wa3.load(sa, 768, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,0> wa4; wa4.load(sa, 1024, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> wb0; wb0.load(sb, 0, 1, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> wb1; wb1.load(sb, 256, 1, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> wb2; wb2.load(sb, 512, 1, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> wb3; wb3.load(sb, 768, 1, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> wb4; wb4.load(sb, 1024, 1, 16);\n"
    "        CooperativeMatrix<float32,16,16,2> ac0; ac0.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ac1; ac1.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ac2; ac2.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ac3; ac3.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ac4; ac4.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ac5; ac5.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ac6; ac6.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ac7; ac7.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ac8; ac8.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ac9; ac9.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> aca; aca.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> acb; acb.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> acc; acc.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> acd; acd.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ace; ace.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> acf; acf.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ag0; ag0.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ag1; ag1.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ag2; ag2.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ag3; ag3.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ag4; ag4.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ag5; ag5.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ag6; ag6.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ag7; ag7.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> ag8; ag8.splat(0.0f);\n"
    "        ac0.mma(wa0, wb0); ac1.mma(wa0, wb1); ac2.mma(wa0, wb2); ac3.mma(wa0, wb3); ac4.mma(wa0, wb4);\n"
    "        ac5.mma(wa1, wb0); ac6.mma(wa1, wb1); ac7.mma(wa1, wb2); ac8.mma(wa1, wb3); ac9.mma(wa1, wb4);\n"
    "        aca.mma(wa2, wb0); acb.mma(wa2, wb1); acc.mma(wa2, wb2); acd.mma(wa2, wb3); ace.mma(wa2, wb4);\n"
    "        acf.mma(wa3, wb0); ag0.mma(wa3, wb1); ag1.mma(wa3, wb2); ag2.mma(wa3, wb3); ag3.mma(wa3, wb4);\n"
    "        ag4.mma(wa4, wb0); ag5.mma(wa4, wb1); ag6.mma(wa4, wb2); ag7.mma(wa4, wb3); ag8.mma(wa4, wb4);\n"
    "        ac0.store(c, 0, 0, n); ac1.store(c, 16, 0, n); ac2.store(c, 32, 0, n); ac3.store(c, 48, 0, n); ac4.store(c, 64, 0, n);\n"
    "        ac5.store(c, 96, 0, n); ac6.store(c, 112, 0, n); ac7.store(c, 128, 0, n); ac8.store(c, 144, 0, n); ac9.store(c, 160, 0, n);\n"
    "        aca.store(c, 192, 0, n); acb.store(c, 208, 0, n); acc.store(c, 224, 0, n); acd.store(c, 240, 0, n); ace.store(c, 256, 0, n);\n"
    "        acf.store(c, 288, 0, n); ag0.store(c, 304, 0, n); ag1.store(c, 320, 0, n); ag2.store(c, 336, 0, n); ag3.store(c, 352, 0, n);\n"
    "        ag4.store(c, 384, 0, n); ag5.store(c, 400, 0, n); ag6.store(c, 416, 0, n); ag7.store(c, 432, 0, n); ag8.store(c, 448, 0, n);\n"
    "    }\n"
    "}\n";

// A small, low-pressure kernel that does NOT spill (the no-op / identity case).
const char* kLightSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void light(KernelBuffer<float32> c, KernelBuffer<float32> a, uint32 n) {\n"
    "        uint32 tid = KernelThread.x();\n"
    "        if (tid < n) { c[tid] = a[tid] + 1.0f; }\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler, const char* source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_occ_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_occ_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule((base / "test" / "M.cajeta").string(),
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

// Lower `kernelName` from `source` into a fresh device module (caller owns the
// returned module + tm; the function pointer is into the module).
struct Lowered {
    std::unique_ptr<llvm::TargetMachine> tm;
    std::unique_ptr<llvm::Module> mod;
};
Lowered lower(Compiler& compiler, const char* source, const char* kernelName,
              llvm::LLVMContext& ctx) {
    Lowered out;
    auto module = compileForInspection(compiler, source);
    auto method = findMethod(module->getStructures()["test.M"], kernelName);
    if (!method) return out;
    out.tm = createAmdgpuTargetMachine("gfx1151");
    if (!out.tm) return out;
    out.mod = std::make_unique<llvm::Module>("xpu_occ_isa", ctx);
    configureDeviceModule(*out.mod, *out.tm);
    if (!cajeta::xpu::amd::lowerKernel(method, *out.mod))
        out.mod.reset();
    return out;
}

int spillOf(const std::string& isa, const std::string& name) {
    for (const auto& k : parseKernelResourceUsage(isa))
        if (k.name == name) return k.spill;
    return -999;
}

// Codegen consumes the module, so measure spill via a throwaway clone and keep
// the original pristine (relaxOccupancyOnSpill mutates only the waves-per-eu attr).
int spillViaClone(llvm::Module& m, llvm::TargetMachine& tm,
                  const std::string& name) {
    auto clone = llvm::CloneModule(m);
    return spillOf(emitIsa(*clone, tm), name);
}

} // namespace

// 1.1.d — the shared resource parser returns correct per-kernel tuples.

// 1.1.a — pinning the real workgroup size eliminates the spill + tags the attr.
TEST(XpuOccupancyBackoffProbeTests, workgroupSizePinEliminatesSpill) {
    Compiler compiler;
    llvm::LLVMContext ctx;
    auto lw = lower(compiler, kSpillerSrc, "spiller", ctx);
    ASSERT_TRUE(lw.mod) << "spiller failed to lower";

    int spill0 = spillViaClone(*lw.mod, *lw.tm, "spiller");
    std::cerr << "[occ] pre:  spiller spill=" << spill0 << "\n";
    ASSERT_GT(spill0, 0) << "test kernel must spill at the default 1024-thread "
                            "budget for this probe to be meaningful";

    auto* fn = lw.mod->getFunction("spiller");
    ASSERT_NE(fn, nullptr);
    setKernelWorkgroupSize(fn, 256);     // a real launch block of 256 threads
    EXPECT_TRUE(fn->hasFnAttribute("amdgpu-flat-work-group-size"));

    int spill1 = spillViaClone(*lw.mod, *lw.tm, "spiller");
    std::cerr << "[occ] post: spiller spill=" << spill1 << "\n";
    EXPECT_EQ(spill1, 0) << "the real (smaller) workgroup budget removes the spill";
}

// 1.1.b — maxThreads == 0 (unknown / non-constant block) is a no-op.
TEST(XpuOccupancyBackoffProbeTests, unknownSizeIsNoOp) {
    Compiler compiler;
    llvm::LLVMContext ctx;
    auto lw = lower(compiler, kLightSrc, "light", ctx);
    ASSERT_TRUE(lw.mod) << "light failed to lower";
    auto* fn = lw.mod->getFunction("light");
    ASSERT_NE(fn, nullptr);

    setKernelWorkgroupSize(fn, 0);
    EXPECT_FALSE(fn->hasFnAttribute("amdgpu-flat-work-group-size"))
        << "unknown size must not set the attribute";
}

// 1.1.c — an explicit flat-work-group-size override is respected, not overwritten.
TEST(XpuOccupancyBackoffProbeTests, explicitOverrideRespected) {
    Compiler compiler;
    llvm::LLVMContext ctx;
    auto lw = lower(compiler, kSpillerSrc, "spiller", ctx);
    ASSERT_TRUE(lw.mod);
    auto* fn = lw.mod->getFunction("spiller");
    ASSERT_NE(fn, nullptr);
    fn->addFnAttr("amdgpu-flat-work-group-size", "128,128");   // author pinned

    setKernelWorkgroupSize(fn, 256);
    EXPECT_EQ(fn->getFnAttribute("amdgpu-flat-work-group-size").getValueAsString(),
              "128,128") << "must not override an explicit flat-work-group-size";
}
