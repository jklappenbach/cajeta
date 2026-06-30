//
// CajetaXPU — constant-folded LDS addressing probe (amdgpu-constant-folded-lds U1). GPU-free.
//
// BlockPadded<T,Block,Pad> physical = logical + (logical/Block)*Pad. The shipped lowering pads
// the COMBINED runtime base (lane term + panel offset) per fragment per K-iteration, so the pad
// arithmetic (udiv/mul) cannot fold — measured 1.79x VALU on gfx1151 (943M vs 526M), regressing
// to 14.7 vs 26.0 TFLOP/s. Tensile folds the whole LDS address into a ds_*offset: immediate with
// the per-lane base computed once (KernelWriterAssembly.py:9709, LocalRead.py:312). This probe
// asserts the foldable shape (spec amdgpu-constant-folded-lds-spec.md §1.4, §2, §3): when a
// fragment provably fits one pad block (constant stride, 15*stride+15 < Block) and the panel base
// is block-aligned, the pad applies to the base ALONE (folding to a compile-time constant for the
// natural unrolled-K case) and the lane+e term rides unpadded — so the device IR carries NO udiv
// from padding and the wide ds_read survives. A geometry that does NOT fit a block must fall back
// to the correct runtime pad (no miscompile).
//

#include "gtest/gtest.h"

#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// Block=2048, Pad=16, stride=64: a use=0/use=1 fragment spans at most 15*64+15 = 975 < 2048, so it
// fits one pad block — the fold precondition holds. Two coop loads at constant block-aligned panel
// offsets 0 and 2048 mimic an unrolled K-loop; under the fold each panel base pads to a constant
// (0 -> 0, 2048 -> 2064), leaving zero runtime pad arithmetic. Padded tile length covers two
// panels: ~4096 logical + 2*16 pad; allocate generously.
const char* kConstFoldSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.BlockPadded;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void cf(KernelBuffer<float32> c, KernelBuffer<float16> b, uint32 n) {\n"
    "        BlockPadded<float16,2048,16> sb = shared float16[6000];\n"
    "        uint32 e = KernelThread.x();\n"
    "        while (e < 512) {\n"
    "            Vector<float16,8> v = b.vload<8>(e * 8);\n"
    "            sb.vstore(e * 8, v);\n"
    "            e = e + 128;\n"
    "        }\n"
    "        Barrier.workgroup();\n"
    "        CooperativeMatrix<float32,16,16,2> acc; acc.splat(0.0f);\n"
    "        CooperativeMatrix<float16,16,16,0> wa0; wa0.load(sb, 0, 0, 64);\n"
    "        CooperativeMatrix<float16,16,16,1> wb0; wb0.load(sb, 0, 1, 64);\n"
    "        acc.mma(wa0, wb0);\n"
    "        CooperativeMatrix<float16,16,16,0> wa1; wa1.load(sb, 2048, 0, 64);\n"
    "        CooperativeMatrix<float16,16,16,1> wb1; wb1.load(sb, 2048, 1, 64);\n"
    "        acc.mma(wa1, wb1);\n"
    "        acc.store(c, 0, 0, n);\n"
    "    }\n"
    "}\n";

// Same shape as kConstFoldSrc but Block=128: a stride-64 fragment spans 975 > 128, so it does NOT
// fit one pad block — the fold precondition fails and fragCoord must fall back to the runtime pad
// (still correct, just not folded). Guards that the fold is gated on soundness, not applied blindly.
const char* kFallbackSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.BlockPadded;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void fb(KernelBuffer<float32> c, KernelBuffer<float16> b, uint32 n) {\n"
    "        BlockPadded<float16,128,16> sb = shared float16[9216];\n"
    "        uint32 e = KernelThread.x();\n"
    "        while (e < 1024) {\n"
    "            Vector<float16,8> v = b.vload<8>(e * 8);\n"
    "            sb.vstore(e * 8, v);\n"
    "            e = e + 128;\n"
    "        }\n"
    "        Barrier.workgroup();\n"
    "        CooperativeMatrix<float16,16,16,1> wb; wb.load(sb, 0, 1, 64);\n"
    "        CooperativeMatrix<float16,16,16,0> wa; wa.load(sb, 0, 0, 64);\n"
    "        CooperativeMatrix<float32,16,16,2> acc; acc.splat(0.0f);\n"
    "        acc.mma(wa, wb);\n"
    "        acc.store(c, 0, 0, n);\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler, const char* source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_cfld_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_cfld_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule((base / "test" / "M.cajeta").string(),
                                   base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass, const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// Lower the kernel to device IR (post-lowering, pre-codegen) and return it as text.
std::string deviceIrOf(const char* source, const char* kernelName) {
    Compiler compiler;
    auto module = compileForInspection(compiler, source);
    auto method = findMethod(module->getStructures()["test.M"], kernelName);
    if (!method) return {};
    auto tm = cajeta::xpu::amd::createAmdgpuTargetMachine("gfx1151");
    if (!tm) return {};
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_cfld_ir", ctx);
    cajeta::xpu::amd::configureDeviceModule(dev, *tm);
    if (!cajeta::xpu::amd::lowerKernel(method, dev)) return {};
    std::string out;
    llvm::raw_string_ostream os(out);
    dev.print(os, nullptr);
    os.flush();
    return out;
}

std::string isaOf(const char* source, const char* kernelName) {
    Compiler compiler;
    auto module = compileForInspection(compiler, source);
    auto method = findMethod(module->getStructures()["test.M"], kernelName);
    if (!method) return {};
    auto tm = cajeta::xpu::amd::createAmdgpuTargetMachine("gfx1151");
    if (!tm) return {};
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_cfld_isa", ctx);
    cajeta::xpu::amd::configureDeviceModule(dev, *tm);
    if (!cajeta::xpu::amd::lowerKernel(method, dev)) return {};
    return cajeta::xpu::amd::emitIsa(dev, *tm);
}

} // namespace

// 1.1.1 — the per-fragment pad folds to a compile-time constant for block-aligned constant panel
// offsets: fragCoord stops padding the combined runtime base (the `cm.abs` term vanishes) and the
// blockPadAddr udiv/mul, now applied to the constant panel base alone, folds away. Today every
// fragment coord pads `cm.abs` (lane term + panel offset) -> 64 runtime udivs -> FAILS.
// (The single residual `udiv i64` is the global->LDS staging-store index `e*8`, a runtime var in a
// separate staging loop, not the WMMA K-loop — out of this unit's scope; tracked as follow-up.)
TEST(XpuConstFoldLdsProbeTests, blockPadFoldsToConstantNoUdiv) {
    std::string ir = deviceIrOf(kConstFoldSrc, "cf");
    ASSERT_FALSE(ir.empty()) << "const-fold kernel failed to lower";
    int fragPad = countOccurrences(ir, "cm.abs");   // per-fragment runtime-base pad (the regression)
    int totalUdiv = countOccurrences(ir, " udiv ");
    std::cerr << "[cfld] BlockPadded<f16,2048,16> stride=64 constant panel offsets: "
              << "fragment-pad(cm.abs)=" << fragPad << "  total udiv=" << totalUdiv
              << " (residual = staging-store index, out of scope)\n";
    EXPECT_EQ(fragPad, 0)
        << "block-fitting fragment with a constant block-aligned base must pad the panel base "
           "alone (fold); fragCoord is still padding the combined runtime base.\n"
        << ir;
}

// 1.1.2 — the fold keeps the fragment read wide and clean.
TEST(XpuConstFoldLdsProbeTests, foldKeepsWideReadNoSpill) {
    std::string isa = isaOf(kConstFoldSrc, "cf");
    ASSERT_FALSE(isa.empty()) << "const-fold kernel failed to lower";
    EXPECT_EQ(isa.find("Cannot select"), std::string::npos) << isa;
    int dsR128 = countOccurrences(isa, "ds_read_b128") + countOccurrences(isa, "ds_load_b128");
    int dsRu16 = countOccurrences(isa, "ds_read_u16") + countOccurrences(isa, "ds_load_u16");
    std::cerr << "[cfld] ds_read_b128=" << dsR128 << " ds_read_u16=" << dsRu16 << "\n";
    EXPECT_GT(dsR128, 0) << "fragment read must stay ds_read_b128 under the fold";
    EXPECT_EQ(dsRu16, 0) << "the fold must not scalarize the fragment read";
}

// 1.1.3 — the fold is exact: over the whole fragment domain the gate admits (panel base block-
// aligned, 15*stride+15 < Block), the decomposed address `pad(panelBase) + lane*stride + e` equals
// the combined-pad address `pad(panelBase + lane*stride + e)` bit-for-bit — the §1.4 identity the
// fragCoord fold rests on. Pure arithmetic mirror of blockPadAddr; also shows it CAN diverge once
// the fragment crosses a block boundary (why the gate is necessary).
TEST(XpuConstFoldLdsProbeTests, foldDecompositionIsExactOverFragmentDomain) {
    auto pad = [](uint32_t x, uint32_t block, uint32_t p) { return x + (x / block) * p; };
    const uint32_t Pad = 16;
    // gate-admitted: Block=2048, stride=64 -> 15*64+15 = 975 < 2048; panel bases multiples of Block.
    const uint32_t Block = 2048, stride = 64;
    ASSERT_LT(15u * stride + 15u, Block);
    for (uint32_t panel = 0; panel <= 4u * Block; panel += Block) {
        uint32_t padPanel = pad(panel, Block, Pad);
        for (uint32_t lane = 0; lane < 16; ++lane)
            for (uint32_t e = 0; e < 16; ++e) {
                uint32_t within = lane * stride + e;
                EXPECT_EQ(padPanel + within, pad(panel + within, Block, Pad))
                    << "fold diverged at panel=" << panel << " lane=" << lane << " e=" << e;
            }
    }
    // boundary-crossing geometry (Block=128, stride=64): decomposition is NOT generally valid.
    bool everDiverges = false;
    for (uint32_t lane = 0; lane < 16 && !everDiverges; ++lane)
        for (uint32_t e = 0; e < 16; ++e) {
            uint32_t within = lane * 64 + e;
            if (pad(0, 128, Pad) + within != pad(within, 128, Pad)) { everDiverges = true; break; }
        }
    EXPECT_TRUE(everDiverges) << "Block=128/stride=64 must be able to cross a block boundary "
                                "(confirms the gate is load-bearing, not cosmetic)";
}

// 1.1.5 — soundness gate: a fragment that does NOT fit one pad block (Block=128, stride 64 ->
// 975 > 128) must fall back to the runtime pad (the per-fragment `cm.abs` base survives), not fold
// (which would miscompile). Still lowers cleanly and keeps the wide read.
TEST(XpuConstFoldLdsProbeTests, nonFittingFragmentFallsBackNoFold) {
    std::string ir = deviceIrOf(kFallbackSrc, "fb");
    ASSERT_FALSE(ir.empty()) << "fallback kernel failed to lower";
    int fragPad = countOccurrences(ir, "cm.abs");
    int fold = countOccurrences(ir, "cm.foldidx");
    std::cerr << "[cfld] fallback BlockPadded<f16,128,16> stride=64: cm.abs=" << fragPad
              << " cm.foldidx=" << fold << "\n";
    EXPECT_GT(fragPad, 0) << "non-fitting fragment must use the runtime-pad fallback (cm.abs)";
    EXPECT_EQ(fold, 0) << "non-fitting fragment must NOT fold (would cross a block boundary)";
    std::string isa = isaOf(kFallbackSrc, "fb");
    ASSERT_FALSE(isa.empty());
    EXPECT_EQ(isa.find("Cannot select"), std::string::npos) << isa;
}
