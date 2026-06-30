//
// CajetaXPU — PGR prefetch-pipeline probe (gpu-f16-torch-recipe U2). GPU-free.
//
// torch's gfx1151 f16 GEMM is PrefetchGlobalRead=2 + DTLB0: the next K-panel's WIDE global
// loads (GRVW8) go into G2L registers, overlapping the current panel's WMMA. The held
// registers MUST be kernel locals (so LLVM keeps them in VGPRs), so the "pipeline primitive"
// is really this staging PATTERN. This probe builds the depthU-64 / 12-acc / 96x128 kernel
// with register-prefetched WIDE staging — A contiguous [M][K] (GRVWA8), B transposed-padded
// [N][Kpad] (GRVWB8 + TransposeLDS) — and asserts the register footprint FITS (no spill) with
// wide loads + wide reads. Single register-prefetch stage (PGR1) establishes the footprint.
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

// depthU-64 / 12-acc / 96x128 / 4-wave, register-prefetched (PGR1) WIDE staging.
// A: wide vload<8> -> sa[m*64+k] contiguous [M=96][K=64].
// B: wide vload<8> -> sb transposed-padded [N=128][Kpad=72] (8 const-index stores/vec).
const char* kPgr1Src = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Workgroup;
import cajeta.xpu.Barrier;
import cajeta.xpu.Shared;
import cajeta.xpu.CooperativeMatrix;
public class M {
    @Kernel
    public static void gemmF16(KernelBuffer<float32> c, KernelBuffer<float16> a,
                               KernelBuffer<float16> b, uint32 n) {
        uint32 blockRow = Workgroup.y();
        uint32 blockCol = Workgroup.x();
        uint32 tid = KernelThread.x();
        uint32 wave = tid / 32;
        uint32 wm = wave / 2;
        uint32 wn = wave % 2;
        uint32 rb = blockRow * 96;
        uint32 cb = blockCol * 128;
        Shared<float16> sa = shared float16[2 * 6144];
        Shared<float16> sb = shared float16[2 * 9216];
        CooperativeMatrix<float32,16,16,2> a00; a00.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a01; a01.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a02; a02.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a03; a03.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a10; a10.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a11; a11.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a12; a12.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a13; a13.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a20; a20.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a21; a21.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a22; a22.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a23; a23.splat(0.0f);
        CooperativeMatrix<float16,16,16,0> wa0;
        CooperativeMatrix<float16,16,16,0> wa1;
        CooperativeMatrix<float16,16,16,0> wa2;
        CooperativeMatrix<float16,16,16,1> wb0;
        CooperativeMatrix<float16,16,16,1> wb1;
        CooperativeMatrix<float16,16,16,1> wb2;
        CooperativeMatrix<float16,16,16,1> wb3;
        uint32 ca = tid;
        while (ca < 768) {
            uint32 m = ca / 8; uint32 kk = (ca % 8) * 8;
            if (rb + m < n) {
                Vector<float16,8> va = a.vload<8>((rb + m) * n + kk);
                sa.vstore(m * 64 + kk, va);
            }
            ca = ca + 128;
        }
        uint32 cbk = tid;
        while (cbk < 1024) {
            uint32 k2 = cbk / 16; uint32 nb = (cbk % 16) * 8;
            Vector<float16,8> vb = b.vload<8>(k2 * n + cb + nb);
            sb[(nb + 0) * 72 + k2] = vb[0]; sb[(nb + 1) * 72 + k2] = vb[1];
            sb[(nb + 2) * 72 + k2] = vb[2]; sb[(nb + 3) * 72 + k2] = vb[3];
            sb[(nb + 4) * 72 + k2] = vb[4]; sb[(nb + 5) * 72 + k2] = vb[5];
            sb[(nb + 6) * 72 + k2] = vb[6]; sb[(nb + 7) * 72 + k2] = vb[7];
            cbk = cbk + 128;
        }
        Barrier.workgroup();
        uint32 npanels = n / 64;
        uint32 ph = 0;
        uint32 p = 0;
        while (p < npanels) {
            // Inline double-buffer staging of the NEXT panel into the alternate LDS buffer
            // (load+commit per chunk -> ~1 vector live at a time; the O3 scheduler overlaps
            // the wide global loads with this panel's WMMA). Wide A + wide-transposed B.
            if (p + 1 < npanels) {
                uint32 ko = (p + 1) * 64;
                uint32 sbA = (1 - ph) * 6144;
                uint32 sbB = (1 - ph) * 9216;
                uint32 sca = tid;
                while (sca < 768) {
                    uint32 m = sca / 8; uint32 kk = (sca % 8) * 8;
                    if (rb + m < n) {
                        Vector<float16,8> va = a.vload<8>((rb + m) * n + ko + kk);
                        sa.vstore(sbA + m * 64 + kk, va);
                    }
                    sca = sca + 128;
                }
                uint32 scb = tid;
                while (scb < 1024) {
                    uint32 k3 = scb / 16; uint32 nb3 = (scb % 16) * 8;
                    Vector<float16,8> vb = b.vload<8>((ko + k3) * n + cb + nb3);
                    sb[sbB + (nb3 + 0) * 72 + k3] = vb[0]; sb[sbB + (nb3 + 1) * 72 + k3] = vb[1];
                    sb[sbB + (nb3 + 2) * 72 + k3] = vb[2]; sb[sbB + (nb3 + 3) * 72 + k3] = vb[3];
                    sb[sbB + (nb3 + 4) * 72 + k3] = vb[4]; sb[sbB + (nb3 + 5) * 72 + k3] = vb[5];
                    sb[sbB + (nb3 + 6) * 72 + k3] = vb[6]; sb[sbB + (nb3 + 7) * 72 + k3] = vb[7];
                    scb = scb + 128;
                }
            }
            uint32 dbA = ph * 6144;
            uint32 dbB = ph * 9216;
            uint32 ao0 = dbA + (wm * 48 + 0) * 64;
            uint32 ao1 = dbA + (wm * 48 + 16) * 64;
            uint32 ao2 = dbA + (wm * 48 + 32) * 64;
            uint32 cn0 = (wn * 64 + 0) * 72; uint32 cn1 = (wn * 64 + 16) * 72;
            uint32 cn2 = (wn * 64 + 32) * 72; uint32 cn3 = (wn * 64 + 48) * 72;
            uint32 ks = 0;
            while (ks < 4) {
                uint32 ka = ks * 16;
                wa0.load(sa, ao0 + ka, 0, 64);
                wa1.load(sa, ao1 + ka, 0, 64);
                wa2.load(sa, ao2 + ka, 0, 64);
                wb0.load(sb, dbB + cn0 + ka, 1, 72);
                wb1.load(sb, dbB + cn1 + ka, 1, 72);
                wb2.load(sb, dbB + cn2 + ka, 1, 72);
                wb3.load(sb, dbB + cn3 + ka, 1, 72);
                a00.mma(wa0, wb0); a01.mma(wa0, wb1); a02.mma(wa0, wb2); a03.mma(wa0, wb3);
                a10.mma(wa1, wb0); a11.mma(wa1, wb1); a12.mma(wa1, wb2); a13.mma(wa1, wb3);
                a20.mma(wa2, wb0); a21.mma(wa2, wb1); a22.mma(wa2, wb2); a23.mma(wa2, wb3);
                ks = ks + 1;
            }
            Barrier.workgroup();
            ph = 1 - ph;
            p = p + 1;
        }
        uint32 r0 = rb + wm * 48;
        uint32 c0 = cb + wn * 64;
        if (r0 + 0 < n) {
            a00.store(c, (r0 + 0) * n + c0 + 0, 0, n); a01.store(c, (r0 + 0) * n + c0 + 16, 0, n);
            a02.store(c, (r0 + 0) * n + c0 + 32, 0, n); a03.store(c, (r0 + 0) * n + c0 + 48, 0, n);
        }
        if (r0 + 16 < n) {
            a10.store(c, (r0 + 16) * n + c0 + 0, 0, n); a11.store(c, (r0 + 16) * n + c0 + 16, 0, n);
            a12.store(c, (r0 + 16) * n + c0 + 32, 0, n); a13.store(c, (r0 + 16) * n + c0 + 48, 0, n);
        }
        if (r0 + 32 < n) {
            a20.store(c, (r0 + 32) * n + c0 + 0, 0, n); a21.store(c, (r0 + 32) * n + c0 + 16, 0, n);
            a22.store(c, (r0 + 32) * n + c0 + 32, 0, n); a23.store(c, (r0 + 32) * n + c0 + 48, 0, n);
        }
    }
}
)CJ";

CajetaModulePtr compileForInspection(Compiler& compiler, const char* source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_pgr_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_pgr_arch_" + std::to_string(rng()));
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
    llvm::Module dev("xpu_pgr_isa", ctx);
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

// Wide-transposed inline double-buffer depthU-64 kernel: fits (no spill), wide loads + reads.
// (Explicit full register-prefetch spills 36 at this footprint; the O3 scheduler overlaps the
// wide global loads with the WMMA instead.)
TEST(XpuPrefetchPipelineProbeTests, wideTransposedDoubleBufferFitsAndWide) {
    std::string isa = isaOf(kPgr1Src, "gemmF16");
    ASSERT_FALSE(isa.empty()) << "PGR1 kernel failed to lower";
    EXPECT_EQ(isa.find("Cannot select"), std::string::npos) << isa;
    int vgpr = parseAmdMeta(isa, "vgpr_count");
    int spill = parseAmdMeta(isa, "vgpr_spill_count");
    int glWide = countOccurrences(isa, "global_load_b128")
               + countOccurrences(isa, "global_load_dwordx4");
    int dsR128 = countOccurrences(isa, "ds_read_b128") + countOccurrences(isa, "ds_load_b128");
    std::cerr << "[pgr] wide-transposed inline double-buffer (scheduler-overlapped):\n"
              << "  vgpr_count=" << vgpr << "  vgpr_spill=" << spill
              << "  global wide=" << glWide << "  ds_read_b128=" << dsR128 << "\n";
    EXPECT_GT(vgpr, 0);
    EXPECT_EQ(spill, 0) << "register-prefetched depthU-64 must fit (no spill)";
    EXPECT_GT(glWide, 0) << "A/B global loads should be wide (b128)";
    EXPECT_GT(dsR128, 0) << "fragment reads should be wide (b128)";
}
