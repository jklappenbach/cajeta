//
// CajetaXPU — torch-config probe (gpu-f16-register-blocked-gemm, prep for the
// torch-faithful replica: MIWT 3x4 = 12 accumulators + wide-B). GPU-free.
//
// Confirms the COMBINATION of the two proven levers before writing the full
// bounded 96x128 kernel: a 3x4 = 12-accumulator wave-tile whose matrix-B fragments
// load COLUMN-MAJOR from a transposed-padded LDS tile (Kpad=24). Asserts no spill
// AND that the B reads are wide (ds_read_b128, not u16) — i.e. register blocking +
// wide-B coexist with healthy VGPR.
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

// 3x4 = 12 accumulators; A row-major (wide already), B loaded COLUMN-MAJOR
// (layout=1) from a transposed-padded tile sb[n*24+k] (Kpad=24) → wide ds_read_b128
// B fragments. The torch-config building blocks (MIWT3x4 + wide-B), VGPR-probed.
const char* kProbe12WideBSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Shared;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void probe12wb(KernelBuffer<float32> c, KernelBuffer<float16> a,\n"
    "                                 KernelBuffer<float16> b, uint32 n) {\n"
    "        uint32 tid = KernelThread.x();\n"
    "        Shared<float16> sa = shared float16[3 * 256];\n"   // 3 A-tiles (48x16)
    "        Shared<float16> sb = shared float16[4 * 24 * 16];\n" // 4 N-tiles x (16 N x 24 Kpad)
    "        uint32 e = tid;\n"
    "        while (e < 768) { sa[e] = a[e]; e = e + 256; }\n"
    "        e = tid;\n"
    "        while (e < 1024) {\n"
    "            uint32 nn = e % 16; uint32 kk = e / 16;\n"      // within a 16(N)x16(K) source block
    "            sb[(e / 256) * 384 + nn * 24 + kk] = b[e];\n"   // transposed-padded store (Kpad=24)
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
    "        CooperativeMatrix<float16,16,16,0> wa1; wa1.load(sa, 256, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,0> wa2; wa2.load(sa, 512, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> wb0; wb0.load(sb, 0, 1, 24);\n"
    "        CooperativeMatrix<float16,16,16,1> wb1; wb1.load(sb, 384, 1, 24);\n"
    "        CooperativeMatrix<float16,16,16,1> wb2; wb2.load(sb, 768, 1, 24);\n"
    "        CooperativeMatrix<float16,16,16,1> wb3; wb3.load(sb, 1152, 1, 24);\n"
    "        a00.mma(wa0, wb0); a01.mma(wa0, wb1); a02.mma(wa0, wb2); a03.mma(wa0, wb3);\n"
    "        a10.mma(wa1, wb0); a11.mma(wa1, wb1); a12.mma(wa1, wb2); a13.mma(wa1, wb3);\n"
    "        a20.mma(wa2, wb0); a21.mma(wa2, wb1); a22.mma(wa2, wb2); a23.mma(wa2, wb3);\n"
    "        a00.store(c, 0, 0, n); a01.store(c, 16, 0, n); a02.store(c, 32, 0, n); a03.store(c, 48, 0, n);\n"
    "        a10.store(c, 16 * n, 0, n); a11.store(c, 16 * n + 16, 0, n);\n"
    "        a12.store(c, 16 * n + 32, 0, n); a13.store(c, 16 * n + 48, 0, n);\n"
    "        a20.store(c, 32 * n, 0, n); a21.store(c, 32 * n + 16, 0, n);\n"
    "        a22.store(c, 32 * n + 32, 0, n); a23.store(c, 32 * n + 48, 0, n);\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler, const char* source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_torchcfg_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_torchcfg_arch_" + std::to_string(rng()));
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
    llvm::Module dev("xpu_torchcfg_isa", ctx);
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

// 12 accumulators + wide-B coexist: clean lowering, no spill, B reads are b128.
TEST(XpuTorchConfigProbeTests, regBlock12PlusWideBFitsAndReadsWide) {
    std::string isa = isaOf(kProbe12WideBSrc, "probe12wb");
    ASSERT_FALSE(isa.empty()) << "12-acc + wide-B kernel failed to lower";
    EXPECT_EQ(isa.find("Cannot select"), std::string::npos) << isa;
    int vgpr = parseAmdMeta(isa, "vgpr_count");
    int spill = parseAmdMeta(isa, "vgpr_spill_count");
    int b128 = countOccurrences(isa, "ds_read_b128") + countOccurrences(isa, "ds_load_b128");
    int u16 = countOccurrences(isa, "ds_read_u16") + countOccurrences(isa, "ds_load_u16");
    std::cerr << "[torch-cfg] 12-acc (3x4) + wide-B (col-major, Kpad=24):\n"
              << "  vgpr_count=" << vgpr << "  vgpr_spill=" << spill
              << "  ds_read b128=" << b128 << " u16=" << u16 << "\n";
    EXPECT_GT(vgpr, 0);
    EXPECT_EQ(spill, 0) << "12-acc + wide-B should not spill";
    EXPECT_GT(b128, 0) << "B fragments should read b128 (wide)";
}
