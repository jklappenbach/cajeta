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
    "            uint32 ie0 = tid; uint32 ie1 = tid + 512;\n"
    "            uint32 ie2 = tid + 1024; uint32 ie3 = tid + 1536;\n"
    "            float16 ra0 = (float16) 0.0; float16 ra1 = (float16) 0.0;\n"
    "            float16 ra2 = (float16) 0.0; float16 ra3 = (float16) 0.0;\n"
    "            float16 rb0 = (float16) 0.0; float16 rb1 = (float16) 0.0;\n"
    "            float16 rb2 = (float16) 0.0; float16 rb3 = (float16) 0.0;\n"
    "            boolean pf = kt + 1 < nsteps;\n"
    "            if (pf) {\n"
    "                ra0 = a[(rb + ie0 / 16) * n + konext + ie0 % 16];\n"
    "                ra1 = a[(rb + ie1 / 16) * n + konext + ie1 % 16];\n"
    "                ra2 = a[(rb + ie2 / 16) * n + konext + ie2 % 16];\n"
    "                ra3 = a[(rb + ie3 / 16) * n + konext + ie3 % 16];\n"
    "                rb0 = b[(konext + ie0 / 128) * n + cb + ie0 % 128];\n"
    "                rb1 = b[(konext + ie1 / 128) * n + cb + ie1 % 128];\n"
    "                rb2 = b[(konext + ie2 / 128) * n + cb + ie2 % 128];\n"
    "                rb3 = b[(konext + ie3 / 128) * n + cb + ie3 % 128];\n"
    "            }\n"
    "            uint32 base = ph * 2048;\n"
    "            wa0.load(sa, base + (wi * 2 + 0) * 256, 0, 16);\n"
    "            wa1.load(sa, base + (wi * 2 + 1) * 256, 0, 16);\n"
    "            wb0.load(sb, base + (wj * 2 + 0) * 16, 0, 128);\n"
    "            wb1.load(sb, base + (wj * 2 + 1) * 16, 0, 128);\n"
    "            acc0_0.mma(wa0, wb0); acc0_1.mma(wa0, wb1);\n"
    "            acc1_0.mma(wa1, wb0); acc1_1.mma(wa1, wb1);\n"
    "            if (pf) {\n"
    "                uint32 nb = (1 - ph) * 2048;\n"
    "                sa[nb + (ie0 / 16) * 16 + ie0 % 16] = ra0;\n"
    "                sa[nb + (ie1 / 16) * 16 + ie1 % 16] = ra1;\n"
    "                sa[nb + (ie2 / 16) * 16 + ie2 % 16] = ra2;\n"
    "                sa[nb + (ie3 / 16) * 16 + ie3 % 16] = ra3;\n"
    "                sb[nb + (ie0 / 128) * 128 + ie0 % 128] = rb0;\n"
    "                sb[nb + (ie1 / 128) * 128 + ie1 % 128] = rb1;\n"
    "                sb[nb + (ie2 / 128) * 128 + ie2 % 128] = rb2;\n"
    "                sb[nb + (ie3 / 128) * 128 + ie3 % 128] = rb3;\n"
    "            }\n"
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

// Emit the gfx1151 ISA for `kernelName` in `source` (GPU-free).
std::string isaOf(const char* source, const char* kernelName) {
    Compiler compiler;
    auto module = compileForInspection(compiler, source);
    auto method = findMethod(module->getStructures()["test.M"], kernelName);
    if (!method) return {};
    auto tm = createAmdgpuTargetMachine("gfx1151");
    if (!tm) return {};
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_occ_isa", ctx);
    configureDeviceModule(dev, *tm);
    if (!lowerKernel(method, dev)) return {};
    return cajeta::xpu::amd::emitIsa(dev, *tm);
}

// VGPRs/wave of `kernelName` in `source`, compiled for gfx1151 (GPU-free). The
// reusable extractor (U1.1.b). spill: out-param VGPR spill count (-1 if absent).
int vgprsOf(const char* source, const char* kernelName, int* spill = nullptr) {
    std::string isa = isaOf(source, kernelName);
    if (isa.empty()) return -1;
    if (spill) *spill = parseAmdMeta(isa, "vgpr_spill_count");
    return parseAmdMeta(isa, "vgpr_count");
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
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
    // The shipped gemmF16 = the U3 register-prefetch pipeline (4 f32 accs/wave +
    // 8 prefetch registers). Linear-staging baseline was vgpr=94 / occ=16 / 17.5
    // TFLOP/s; this trades some occupancy for global-latency hiding → 20.6 TFLOP/s.
    std::cerr << "[occ] gemmF16 (U3 register-prefetch): vgpr=" << vgpr
              << "  vgpr_spill=" << spill
              << "  theoretical_occupancy=" << occ << " waves/SIMD"
              << "  (20.6 TFLOP/s @ n2048; baseline was vgpr=94/occ=16/17.5)\n";
    EXPECT_GE(occ, 1) << "kernel must be launchable (>=1 wave/SIMD)";
}

// GPU-free ISA diagnosis substitute for rocprof (PMC profiling segfaults on this
// gfx1151 APU — see reference_rocprof_broken_gfx1151). Static instruction mix of
// the shipped gemmF16: how much of the body is memory-wait (s_waitcnt) + barrier
// vs the WMMA compute + LDS/global traffic. Logs the counts (informational).
TEST(XpuOccupancyTests, gemmF16IsaInstructionMix) {
    std::string isa = isaOf(kGemmF16Src, "gemmF16");
    ASSERT_FALSE(isa.empty());
    std::cerr << "[isa] gemmF16 instruction mix (gfx1151):"
              << "  s_waitcnt=" << countOccurrences(isa, "s_waitcnt")
              << "  s_barrier=" << (countOccurrences(isa, "s_barrier")
                                    + countOccurrences(isa, "s_wait_barrier")
                                    + countOccurrences(isa, "barrier"))
              << "  v_wmma=" << countOccurrences(isa, "v_wmma")
              << "  ds_read=" << countOccurrences(isa, "ds_load")
                              + countOccurrences(isa, "ds_read")
              << "  ds_write=" << countOccurrences(isa, "ds_store")
                               + countOccurrences(isa, "ds_write")
              << "  global_load=" << countOccurrences(isa, "global_load")
              << "  v_wmma_present=" << (countOccurrences(isa, "v_wmma") > 0)
              << "\n";
    EXPECT_GT(countOccurrences(isa, "v_wmma"), 0) << "expected WMMA in the GEMM";
}
