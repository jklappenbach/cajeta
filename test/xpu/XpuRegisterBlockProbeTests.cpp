//
// CajetaXPU — register-blocking feasibility probe (gpu-f16-register-blocked-gemm
// Unit 1). GPU-free.
//
// The pivotal de-risking measurement that ROUTES the rest of the plan: can the
// compiler hold a 3x4 = 12-accumulator WMMA wave-tile (torch/Tensile's MIWT3x4
// register block) per wave WITHOUT pathological spill, and does the padded
// tile-major LDS layout emit WIDE ds_read (b128) fragment loads?
//
//   - probe12LowersCleanly: the 12-accumulator + 7-fragment kernel lowers and
//     emits gfx1151 ISA with no Cannot-select (the abstraction can express it).
//   - probe12ReportsRoutingDecision: read .vgpr_count / .vgpr_spill_count and the
//     ds_read widths; LOG a KERNEL-ONLY vs COMPILER-DEEP verdict (spill==0 + wide
//     reads ⇒ kernel-only; else compiler-deep). The verdict is recorded into the
//     plan's U1.3 — it decides whether Unit 2 (AMD-deep codegen) is needed.
//
// vs the shipped gemmF16 (4 accumulators, vgpr≈113) measured in XpuOccupancyTests.
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

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace cajeta::xpu::amd;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// A 3x4 = 12-accumulator register-blocked WMMA wave-tile: 3 A-fragments x 4
// B-fragments, each fragment loaded ONCE from LDS and reused across the grid (the
// arithmetic-intensity lever). A/B tiles are stored tile-major in LDS — each 16x16
// sub-tile contiguous (stride 16 ⇒ a wide ds_read_b128 fragment load), padded by 8
// f16/tile to break 32-bank periodicity across tiles. This is the structure Unit 3
// adopts in gemmF16; the probe measures whether it spills.
const char* kProbe12Src =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Shared;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void probe12(KernelBuffer<float32> c, KernelBuffer<float16> a,\n"
    "                               KernelBuffer<float16> b, uint32 n) {\n"
    "        uint32 tid = KernelThread.x();\n"
    "        Shared<float16> sa = shared float16[3 * 264];\n"   // 3 A-tiles, 256 + 8 pad
    "        Shared<float16> sb = shared float16[4 * 264];\n"   // 4 B-tiles, 256 + 8 pad
    "        uint32 e = tid;\n"
    "        while (e < 768) {\n"
    "            sa[(e / 256) * 264 + e % 256] = a[e];\n"
    "            e = e + 256;\n"
    "        }\n"
    "        e = tid;\n"
    "        while (e < 1024) {\n"
    "            sb[(e / 256) * 264 + e % 256] = b[e];\n"
    "            e = e + 256;\n"
    "        }\n"
    "        Barrier.workgroup();\n"
    "        CooperativeMatrix<float32,16,16,2> a00; a00.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> a01; a01.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> a02; a02.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> a03; a03.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> a10; a10.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> a11; a11.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> a12; a12.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> a13; a13.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> a20; a20.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> a21; a21.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> a22; a22.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> a23; a23.splat(0.0f);\n"
    "        CooperativeMatrix<float16,16,16,0> wa0; wa0.load(sa, 0, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,0> wa1; wa1.load(sa, 264, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,0> wa2; wa2.load(sa, 528, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> wb0; wb0.load(sb, 0, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> wb1; wb1.load(sb, 264, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> wb2; wb2.load(sb, 528, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> wb3; wb3.load(sb, 792, 0, 16);\n"
    "        a00.mma(wa0, wb0); a01.mma(wa0, wb1); a02.mma(wa0, wb2); a03.mma(wa0, wb3);\n"
    "        a10.mma(wa1, wb0); a11.mma(wa1, wb1); a12.mma(wa1, wb2); a13.mma(wa1, wb3);\n"
    "        a20.mma(wa2, wb0); a21.mma(wa2, wb1); a22.mma(wa2, wb2); a23.mma(wa2, wb3);\n"
    "        a00.store(c, 0, 0, n); a01.store(c, 16, 0, n);\n"
    "        a02.store(c, 32, 0, n); a03.store(c, 48, 0, n);\n"
    "        a10.store(c, 16 * n, 0, n); a11.store(c, 16 * n + 16, 0, n);\n"
    "        a12.store(c, 16 * n + 32, 0, n); a13.store(c, 16 * n + 48, 0, n);\n"
    "        a20.store(c, 32 * n, 0, n); a21.store(c, 32 * n + 16, 0, n);\n"
    "        a22.store(c, 32 * n + 32, 0, n); a23.store(c, 32 * n + 48, 0, n);\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler, const char* source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_rbprobe_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_rbprobe_arch_" + std::to_string(rng()));
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
    llvm::Module dev("xpu_rbprobe_isa", ctx);
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

int rdna35Occupancy(int vgpr) {
    if (vgpr <= 0) return 0;
    int alloc = ((vgpr + 15) / 16) * 16;
    int waves = 1536 / alloc;
    return waves > 16 ? 16 : waves;
}

} // namespace

// U1.1.a: the 12-accumulator register-blocked kernel LOWERS and emits gfx1151 ISA
// cleanly (no Cannot-select) — i.e. CooperativeMatrix can express a 3x4 wave-tile.
TEST(XpuRegisterBlockProbeTests, probe12LowersCleanly) {
    std::string isa = isaOf(kProbe12Src, "probe12");
    ASSERT_FALSE(isa.empty()) << "12-accumulator kernel failed to lower/emit ISA";
    EXPECT_GT(countOccurrences(isa, "v_wmma"), 0)
        << "expected v_wmma in the register-blocked kernel";
    EXPECT_EQ(isa.find("Cannot select"), std::string::npos) << isa;
}

// U1.1.b + U1.1.c + U1.3: read VGPR/spill + ds_read widths and LOG the routing
// verdict. The kernel must lower (measurement succeeded); the verdict (KERNEL-ONLY
// vs COMPILER-DEEP) is recorded into the plan — it is data, not a pass/fail gate.
TEST(XpuRegisterBlockProbeTests, probe12ReportsRoutingDecision) {
    std::string isa = isaOf(kProbe12Src, "probe12");
    ASSERT_FALSE(isa.empty()) << "probe failed to lower";
    int vgpr = parseAmdMeta(isa, "vgpr_count");
    int spill = parseAmdMeta(isa, "vgpr_spill_count");
    ASSERT_GT(vgpr, 0) << "failed to parse .vgpr_count";

    int b128 = countOccurrences(isa, "ds_read_b128")
             + countOccurrences(isa, "ds_load_b128");
    int b64 = countOccurrences(isa, "ds_read_b64")
            + countOccurrences(isa, "ds_load_b64");
    int u16 = countOccurrences(isa, "ds_read_u16")
            + countOccurrences(isa, "ds_load_u16");

    bool noSpill = (spill == 0);
    bool wideReads = (b128 > 0) && (u16 == 0);
    const char* verdict = (noSpill && wideReads) ? "KERNEL-ONLY (Unit 2 not needed)"
                                                 : "COMPILER-DEEP (Unit 2 required)";

    std::cerr << "[rb-probe] 12-accumulator (3x4) register block on gfx1151:\n"
              << "  vgpr_count=" << vgpr
              << "  vgpr_spill=" << spill
              << "  theoretical_occupancy=" << rdna35Occupancy(vgpr) << " waves/SIMD\n"
              << "  ds_read widths: b128=" << b128 << " b64=" << b64
              << " u16=" << u16 << "\n"
              << "  VERDICT: " << verdict << "\n"
              << "  (vs shipped gemmF16: 4 accs, vgpr~113; see XpuOccupancyTests)\n";

    SUCCEED() << "probe measured; verdict logged for plan U1.3";
}
