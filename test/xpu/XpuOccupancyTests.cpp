//
// CajetaXPU — GEMM occupancy / VGPR-pressure measurement (gpu-gemm-occupancy Unit 1).
//
// GPU-free: compile a WMMA @Kernel for gfx1151, emit its ISA, and read the VGPRs/wave
// from the AMD resource metadata (`.vgpr_count`) — the input to theoretical occupancy
// (waves/SIMD = VGPR-file / vgprs-per-wave). No device needed. `vgprsOf` is the reusable
// extractor (U1.1.b); `rdna35Occupancy` derives waves/SIMD from a VGPR count on RDNA3.5.
//
// The headline measurement is the REAL `gemmF16` (4 f32 accumulators / wave) — its VGPR
// count + theoretical occupancy are the baseline the occupancy sweep (U2) moves.
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

// A minimal single-tile f16 WMMA kernel (one mma) — the low-VGPR control for the
// extractor (≈33 VGPRs, occupancy-max).
const char* kWmmaSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wmma(KernelBuffer<float16> a, KernelBuffer<float16> b,\n"
    "                            KernelBuffer<float32> c) {\n"
    "        CooperativeMatrix<float16,16,16,0> ma; ma.load(a, 0, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> mb; mb.load(b, 0, 0, 16);\n"
    "        CooperativeMatrix<float32,16,16,2> mc; mc.splat(0.0f);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "}\n";

// The REAL bench kernel — mirrors samples/profile GpuMatMulF16Bench.gemmF16 (the
// double-buffered 128x128 tile, 16 waves, 4 f32 accumulators/wave). This is the
// occupancy baseline U2 sweeps against; keep it in sync with the sample.
const char* kGemmF16Src =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Workgroup;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Shared;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void gemmF16(KernelBuffer<float32> c, KernelBuffer<float16> a,\n"
    "                               KernelBuffer<float16> b, uint32 n) {\n"
    "        uint32 blockRow = Workgroup.y();\n"
    "        uint32 blockCol = Workgroup.x();\n"
    "        uint32 tid = KernelThread.x();\n"
    "        uint32 wave = tid / 32;\n"
    "        uint32 wi = wave / 4;\n"
    "        uint32 wj = wave % 4;\n"
    "        uint32 rb = blockRow * 128;\n"
    "        uint32 cb = blockCol * 128;\n"
    "        Shared<float16> sa = shared float16[2 * 2048];\n"
    "        Shared<float16> sb = shared float16[2 * 2048];\n"
    "        CooperativeMatrix<float32,16,16,2> acc0_0; acc0_0.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> acc0_1; acc0_1.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> acc1_0; acc1_0.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> acc1_1; acc1_1.splat(0.0f);\n"
    "        CooperativeMatrix<float16,16,16,0> wa0;\n"
    "        CooperativeMatrix<float16,16,16,0> wa1;\n"
    "        CooperativeMatrix<float16,16,16,1> wb0;\n"
    "        CooperativeMatrix<float16,16,16,1> wb1;\n"
    "        uint32 e = tid;\n"
    "        while (e < 2048) {\n"
    "            uint32 ar = e / 16; uint32 ac = e % 16;\n"
    "            sa[ar * 16 + ac] = a[(rb + ar) * n + ac];\n"
    "            uint32 br = e / 128; uint32 bc = e % 128;\n"
    "            sb[br * 128 + bc] = b[br * n + cb + bc];\n"
    "            e = e + 512;\n"
    "        }\n"
    "        Barrier.workgroup();\n"
    "        uint32 nsteps = n / 16;\n"
    "        uint32 ph = 0;\n"
    "        uint32 kt = 0;\n"
    "        while (kt < nsteps) {\n"
    "            uint32 konext = (kt + 1) * 16;\n"
    "            if (kt + 1 < nsteps) {\n"
    "                uint32 nb = (1 - ph) * 2048;\n"
    "                uint32 e2 = tid;\n"
    "                while (e2 < 2048) {\n"
    "                    uint32 ar = e2 / 16; uint32 ac = e2 % 16;\n"
    "                    sa[nb + ar * 16 + ac] = a[(rb + ar) * n + konext + ac];\n"
    "                    uint32 br = e2 / 128; uint32 bc = e2 % 128;\n"
    "                    sb[nb + br * 128 + bc] = b[(konext + br) * n + cb + bc];\n"
    "                    e2 = e2 + 512;\n"
    "                }\n"
    "            }\n"
    "            uint32 base = ph * 2048;\n"
    "            wa0.load(sa, base + (wi * 2 + 0) * 256, 0, 16);\n"
    "            wa1.load(sa, base + (wi * 2 + 1) * 256, 0, 16);\n"
    "            wb0.load(sb, base + (wj * 2 + 0) * 16, 0, 128);\n"
    "            wb1.load(sb, base + (wj * 2 + 1) * 16, 0, 128);\n"
    "            acc0_0.mma(wa0, wb0); acc0_1.mma(wa0, wb1);\n"
    "            acc1_0.mma(wa1, wb0); acc1_1.mma(wa1, wb1);\n"
    "            Barrier.workgroup();\n"
    "            ph = 1 - ph;\n"
    "            kt = kt + 1;\n"
    "        }\n"
    "        uint32 rr0 = rb + (wi * 2 + 0) * 16;\n"
    "        uint32 rr1 = rb + (wi * 2 + 1) * 16;\n"
    "        uint32 ccc0 = cb + (wj * 2 + 0) * 16;\n"
    "        uint32 ccc1 = cb + (wj * 2 + 1) * 16;\n"
    "        acc0_0.store(c, rr0 * n + ccc0, 0, n);\n"
    "        acc0_1.store(c, rr0 * n + ccc1, 0, n);\n"
    "        acc1_0.store(c, rr1 * n + ccc0, 0, n);\n"
    "        acc1_1.store(c, rr1 * n + ccc1, 0, n);\n"
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

// Parse an AMD msgpack-metadata integer field from the emitted ISA, e.g.
// "    .vgpr_count:     33" -> 33. Returns -1 if absent.
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

// VGPRs/wave of `kernelName` in `source`, compiled for gfx1151 (GPU-free). The
// reusable extractor (U1.1.b). spill: out-param VGPR spill count (-1 if absent).
int vgprsOf(const char* source, const char* kernelName, int* spill = nullptr) {
    Compiler compiler;
    auto module = compileForInspection(compiler, source);
    auto method = findMethod(module->getStructures()["test.M"], kernelName);
    if (!method) return -1;
    auto tm = createAmdgpuTargetMachine("gfx1151");
    if (!tm) return -1;
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_occ_isa", ctx);
    configureDeviceModule(dev, *tm);
    if (!lowerKernel(method, dev)) return -1;
    std::string isa = cajeta::xpu::amd::emitIsa(dev, *tm);
    if (spill) *spill = parseAmdMeta(isa, "vgpr_spill_count");
    return parseAmdMeta(isa, "vgpr_count");
}

// Theoretical wave32 occupancy (waves/SIMD) for a VGPR count on RDNA3.5 (gfx1151):
// 1536 VGPRs/SIMD, allocation granularity 16, hard cap 16 waves/SIMD.
int rdna35Occupancy(int vgpr) {
    if (vgpr <= 0) return 0;
    int alloc = ((vgpr + 15) / 16) * 16;       // round up to the 16-VGPR block
    int waves = 1536 / alloc;
    return waves > 16 ? 16 : waves;
}

} // namespace

// U1.1.b: the extractor reads a concrete VGPR count for an arbitrary kernel (the
// low-pressure single-tile WMMA control).
TEST(XpuOccupancyTests, extractorReadsVgprCount) {
    int spill = -1;
    int vgpr = vgprsOf(kWmmaSrc, "wmma", &spill);
    EXPECT_GT(vgpr, 0) << "failed to parse .vgpr_count from ISA";
    EXPECT_EQ(spill, 0) << "single-tile WMMA should not spill VGPRs";
    std::cerr << "[occ] single-tile WMMA: vgpr=" << vgpr
              << " occupancy=" << rdna35Occupancy(vgpr) << " waves/SIMD\n";
}

// U1.1.a / U1.3: the REAL gemmF16 baseline — VGPRs/wave + theoretical occupancy,
// the number U2's occupancy sweep moves.
TEST(XpuOccupancyTests, gemmF16BaselineOccupancy) {
    int spill = -1;
    int vgpr = vgprsOf(kGemmF16Src, "gemmF16", &spill);
    ASSERT_GT(vgpr, 0) << "failed to parse gemmF16 .vgpr_count";
    int occ = rdna35Occupancy(vgpr);
    std::cerr << "[occ] BASELINE gemmF16 (4 f32 accs/wave): vgpr=" << vgpr
              << "  vgpr_spill=" << spill
              << "  theoretical_occupancy=" << occ << " waves/SIMD"
              << "  (17.5 TFLOP/s @ n2048)\n";
    EXPECT_GE(occ, 1) << "kernel must be launchable (>=1 wave/SIMD)";
}
