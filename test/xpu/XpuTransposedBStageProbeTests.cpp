//
// CajetaXPU — transposed+padded B staging probe (gpu-f16-torch-recipe U1). GPU-free.
//
// torch's verified gfx1151 f16 GEMM stores B TransposeLDS=1 + LdsPadB=8: a wide 128-bit
// global load of a B panel, written transposed into a padded LDS tile [N][Kpad] so the WMMA
// B fragment read is wide (ds_read_b128) and bank-conflict-free. The local write is
// necessarily strided (the transpose) — that's expected; U2's prefetch hides it. This probe
// asserts the new `CoopStage.panelTransposed` primitive emits the WIDE global load
// (global_load_b128) + WIDE fragment read (ds_read_b128), no per-element ds_read_u16, no
// spill. (Same GPU-free harness as XpuVectorStagingProbeTests.)
//

#include "gtest/gtest.h"

#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuBackend.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

using namespace cajeta::xpu::amd;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// Stage a 64(K)x128(N) B panel transposed+padded into sb[N=128][Kpad=72], then read two
// col-major (wide) fragments out of it and mma so the reads stay live. The wide global load
// lives in panelTransposed; the wide reads are the col-major coop loads (layout 1, stride 72).
const char* kTransposedBSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Shared;\n"
    "import cajeta.xpu.CoopStage;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void stageBT(KernelBuffer<float32> c, KernelBuffer<float16> a,\n"
    "                               KernelBuffer<float16> b, uint32 n) {\n"
    "        Shared<float16> sa = shared float16[16 * 16];\n"     // A 16x16 row-major
    "        Shared<float16> sb = shared float16[128 * 72];\n"    // B [N=128][Kpad=64+8]
    "        CoopStage.panel(sa, a, 0, 0, 16, 16, n);\n"          // A: scalar stage, wide read
    "        CoopStage.panelTransposed(sb, b, 0, 0, 64, 128, n, 8);\n"  // K=64 x N=128, pad 8
    "        Barrier.workgroup();\n"
    "        CooperativeMatrix<float16,16,16,0> wa; wa.load(sa, 0, 0, 16);\n"   // use=0 layout=0
    "        CooperativeMatrix<float16,16,16,1> wb; wb.load(sb, 0, 1, 72);\n"   // use=1 layout=1
    "        CooperativeMatrix<float32,16,16,2> acc; acc.splat(0.0f);\n"
    "        acc.mma(wa, wb);\n"
    "        acc.store(c, 0, 0, n);\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler, const char* source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_btstage_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_btstage_arch_" + std::to_string(rng()));
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

int parseAmdMeta(const std::string& isa, const std::string& key) {
    std::istringstream ss(isa);
    std::string line;
    std::string needle = "." + key + ":";
    while (std::getline(ss, line)) {
        auto p = line.find(needle);
        if (p == std::string::npos) continue;
        p += needle.size();
        while (p < line.size() && !std::isdigit((unsigned char) line[p])) ++p;
        if (p >= line.size()) return -1;
        return std::atoi(line.c_str() + p);
    }
    return -1;
}

std::string isaOf(const char* source, const char* kernelName) {
    Compiler compiler;
    auto module = compileForInspection(compiler, source);
    auto method = findMethod(module->getStructures()["test.M"], kernelName);
    if (!method) return {};
    auto tm = createAmdgpuTargetMachine("gfx1151");
    if (!tm) return {};
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_btstage_isa", ctx);
    configureDeviceModule(dev, *tm);
    if (!lowerKernel(method, dev)) return {};
    return cajeta::xpu::amd::emitIsa(dev, *tm);
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

} // namespace

// panelTransposed: wide global load + wide (col-major) fragment read, no per-element u16,
// no spill, clean lowering.
TEST(XpuTransposedBStageProbeTests, panelTransposedWideLoadWideRead) {
    std::string isa = isaOf(kTransposedBSrc, "stageBT");
    ASSERT_FALSE(isa.empty()) << "transposed-B stage kernel failed to lower";
    EXPECT_EQ(isa.find("Cannot select"), std::string::npos) << isa;
    int spill = parseAmdMeta(isa, "vgpr_spill_count");
    int glWide = countOccurrences(isa, "global_load_b128")
               + countOccurrences(isa, "global_load_dwordx4");
    int dsR128 = countOccurrences(isa, "ds_read_b128") + countOccurrences(isa, "ds_load_b128");
    int dsRu16 = countOccurrences(isa, "ds_read_u16") + countOccurrences(isa, "ds_load_u16");
    std::cerr << "[bt-stage] panelTransposed 64x128 pad8:\n"
              << "  vgpr_spill=" << spill << "  global wide=" << glWide
              << "  ds_read_b128=" << dsR128 << "  ds_read_u16=" << dsRu16 << "\n";
    EXPECT_EQ(spill, 0) << "transposed-B stage should not spill";
    EXPECT_GT(glWide, 0) << "panel global read should be wide (global_load_b128)";
    EXPECT_GT(dsR128, 0) << "B fragment read should be wide (ds_read_b128)";
    EXPECT_EQ(dsRu16, 0) << "B fragment read must not be per-element (ds_read_u16)";
}

// 1.1.2 — correctness of the transpose+pad index math via a SINGLE-THREAD CPU JIT run
// (one thread does the whole cooperative copy, so no cross-thread LDS is needed). f32 to keep
// the JIT ABI simple; the index logic is type-independent. Stage an 8(K)x8(N) panel into
// sb[N=8][Kpad=10], copy it out, and assert out[c*10+r] == b[r*8+c] (transposed).
namespace {
const char* kTransposedCpuSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Workgroup;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Shared;\n"
    "import cajeta.xpu.CoopStage;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void stageCheck(KernelBuffer<float32> out, KernelBuffer<float32> b,\n"
    "                                  uint32 n) {\n"
    "        Shared<float32> sb = shared float32[8 * 10];\n"        // [N=8][Kpad=8+2]
    "        CoopStage.panelTransposed(sb, b, 0, 0, 8, 8, n, 2);\n" // rows=K=8, cols=N=8, pad=2
    "        uint32 e = KernelThread.x();\n"
    "        while (e < 80) { out[e] = sb[e]; e = e + Workgroup.dimX(); }\n"
    "    }\n"
    "}\n";
using StageFn = void (*)(float*, float*, uint32_t,
                         int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                         int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
} // namespace

TEST(XpuTransposedBStageProbeTests, panelTransposedTransposesCorrectlyOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kTransposedCpuSrc);
    auto k = findMethod(module->getStructures()["test.M"], "stageCheck");
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr);
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_bt_cpu_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    auto* fn = cajeta::xpu::cpu::lowerKernel(k, *host);
    ASSERT_NE(fn, nullptr);
    std::string fnName = fn->getName().str();
    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr)) << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto symOrErr = jit->lookup(fnName);
    ASSERT_TRUE(static_cast<bool>(symOrErr)) << llvm::toString(symOrErr.takeError());
    auto stage = symOrErr->toPtr<StageFn>();

    std::vector<float> b(64), out(80, -1.0f);
    for (int i = 0; i < 64; ++i) b[i] = static_cast<float>(i);   // b[r*8+c] = r*8+c
    // single thread (block 1, grid 1) does the entire cooperative copy
    stage(out.data(), b.data(), 8, 0,0,0, 0,0,0, 1,1,1, 1,1,1);
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
            EXPECT_FLOAT_EQ(out[c * 10 + r], b[r * 8 + c])
                << "transpose mismatch at r=" << r << " c=" << c;
}
