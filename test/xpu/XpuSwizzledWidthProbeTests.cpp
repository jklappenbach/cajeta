//
// CajetaXPU — Swizzled-tile access-WIDTH probe (gpu-f16-torch-recipe, conflict-free layout).
// The XOR swizzle (col ^ (row&(S-1))) is ELEMENT-granular; the open question is whether it
// keeps the WMMA fragment read wide (ds_read_b128) and the staging store wide (ds_store_b128),
// or scrambles the per-lane 8xf16 into scalar ds_*_u16. Decides swizzle-vs-block-pad for the
// conflict-free layout. GPU-free.
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

// Swizzled<float16,64> B tile [N=128][K=64], staged wide (vload<8> -> vstore), read col-major
// (the transposed-B WMMA fragment). Check the ISA read/store widths.
const char* kSwizSrc = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Barrier;
import cajeta.xpu.Swizzled;
import cajeta.xpu.CooperativeMatrix;
public class M {
    @Kernel
    public static void swz(KernelBuffer<float32> c, KernelBuffer<float16> b, uint32 n) {
        uint32 tid = KernelThread.x();
        Swizzled<float16,64> sb = shared float16[128 * 64];
        uint32 e = tid;
        while (e < 1024) {
            Vector<float16,8> v = b.vload<8>(e * 8);
            sb.vstore(e * 8, v);
            e = e + 128;
        }
        Barrier.workgroup();
        CooperativeMatrix<float16,16,16,1> wb; wb.load(sb, 0, 1, 64);
        CooperativeMatrix<float16,16,16,0> wa; wa.load(sb, 0, 1, 64);
        CooperativeMatrix<float32,16,16,2> acc; acc.splat(0.0f);
        acc.mma(wa, wb);
        acc.store(c, 0, 0, n);
    }
}
)CJ";

CajetaModulePtr compileForInspection(Compiler& compiler, const char* source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path() / ("cajeta_swizw_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path() / ("cajeta_swizw_a_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule((base / "test" / "M.cajeta").string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}
cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& k, const std::string& n) {
    for (auto& [kk, m] : k->getMethods()) if (m && m->getName() == n) return m;
    return nullptr;
}
std::string isaOf(const char* src, const char* kn) {
    Compiler c; auto mod = compileForInspection(c, src);
    auto meth = findMethod(mod->getStructures()["test.M"], kn);
    if (!meth) return {};
    auto tm = createAmdgpuTargetMachine("gfx1151"); if (!tm) return {};
    llvm::LLVMContext ctx; llvm::Module dev("swizw", ctx);
    configureDeviceModule(dev, *tm);
    if (!lowerKernel(meth, dev)) return {};
    return cajeta::xpu::amd::emitIsa(dev, *tm);
}
int cnt(const std::string& h, const std::string& nd) {
    int n=0; for (size_t p=h.find(nd); p!=std::string::npos; p=h.find(nd,p+nd.size())) ++n; return n;
}
} // namespace

TEST(XpuSwizzledWidthProbeTests, swizzledTileAccessWidth) {
    std::string isa = isaOf(kSwizSrc, "swz");
    ASSERT_FALSE(isa.empty()) << "swizzled kernel failed to lower";
    EXPECT_EQ(isa.find("Cannot select"), std::string::npos) << isa;
    int dsR128 = cnt(isa,"ds_read_b128")+cnt(isa,"ds_load_b128");
    int dsRu16 = cnt(isa,"ds_read_u16")+cnt(isa,"ds_load_u16");
    int dsW128 = cnt(isa,"ds_write_b128")+cnt(isa,"ds_store_b128");
    int dsWnar = cnt(isa,"ds_write_b16")+cnt(isa,"ds_store_b16");
    std::cerr << "[swiz-width] Swizzled<f16,64> WMMA read + vstore:\n"
              << "  ds_read_b128=" << dsR128 << " ds_read_u16=" << dsRu16
              << "  ds_store_b128=" << dsW128 << " ds_store_b16=" << dsWnar << "\n";
    // Descriptive: records whether the swizzle keeps accesses wide.
}
