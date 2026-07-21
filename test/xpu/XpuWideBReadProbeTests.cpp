//
// CajetaXPU — wide matrix-B fragment read probe (xpu-coopmatrix-wide-b-read Unit 1).
// GPU-free.
//
// The AMD coopMatrixLoad marshals a WMMA fragment as 16 scalar per-element loads
// (AmdgpuKernelLowering.cpp:866). For matrix-A (use=0) consecutive elements are
// contiguous (lane*stride+e) so LLVM coalesces them to ds_read_b128; for matrix-B
// (use=1, ROW-major) the index is e*stride+lane → strided → 16× ds_read_u16.
//
// fragCoord already computes a COLUMN-major index (col*stride+row = lane*stride+e)
// that is contiguous for B. This probe checks whether loading the B fragment
// column-major (layout=1) makes its reads vectorize to ds_read_b128 — which would
// make the wide-B read a pure kernel LDS-layout technique (no codegen change).
//
//   - bColMajorBReadWidensVsRowMajor: col-major B emits fewer u16 / more b128 than
//     the row-major control → the routing verdict (KERNEL-ONLY vs CODEGEN).
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
#include <sstream>
#include <random>
#include <string>

using namespace cajeta::xpu::amd;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// A single 16x16 WMMA tile: load A (use=0) + B (use=1) from one Shared tile, mma,
// store. `BLAYOUT` is the B-fragment load layout (0 = row-major = current behavior,
// 1 = column-major = contiguous-for-B hypothesis). A stays row-major (already wide).
// The ds_read WIDTH is set by the addressing pattern, independent of data values, so
// this isolates the layout's effect on the B read.
std::string srcWithBLayout(int blayout) {
    return std::string(
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Barrier;\n"
        "import cajeta.xpu.Shared;\n"
        "import cajeta.xpu.CooperativeMatrix;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void bread(KernelBuffer<float32> c, KernelBuffer<float16> g) {\n"
        "        Shared<float16> sa = shared float16[256];\n"
        "        Shared<float16> sb = shared float16[256];\n"
        "        uint32 t = KernelThread.x();\n"
        "        sa[t] = g[t];\n"
        "        sb[t] = g[t];\n"
        "        Barrier.workgroup();\n"
        "        CooperativeMatrix<float16,16,16,0> wa; wa.load(sa, 0, 0, 16);\n"
        "        CooperativeMatrix<float16,16,16,1> wb; wb.load(sb, 0, ") +
        std::to_string(blayout) +
        ", 16);\n"
        "        CooperativeMatrix<float32,16,16,2> acc; acc.splat(0.0f);\n"
        "        acc.mma(wa, wb);\n"
        "        acc.store(c, 0, 0, 16);\n"
        "    }\n"
        "}\n";
}

CajetaModulePtr compileForInspection(Compiler& compiler, const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_widebr_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_widebr_arch_" + std::to_string(rng()));
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

std::string isaOf(const std::string& source, const char* kernelName) {
    Compiler compiler;
    auto module = compileForInspection(compiler, source);
    auto method = findMethod(module->getStructures()["test.M"], kernelName);
    if (!method) return {};
    auto tm = createAmdgpuTargetMachine("gfx1151");
    if (!tm) return {};
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_widebr_isa", ctx);
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

struct Widths { int b128, b64, u16; };
Widths readWidths(const std::string& isa) {
    return {countOccurrences(isa, "ds_read_b128") + countOccurrences(isa, "ds_load_b128"),
            countOccurrences(isa, "ds_read_b64") + countOccurrences(isa, "ds_load_b64"),
            countOccurrences(isa, "ds_read_u16") + countOccurrences(isa, "ds_load_u16")};
}

} // namespace

// Faithful to the SHIPPED bench gemmF16 B path: B stored transposed-padded (N-major, K contiguous,
// stride 66 = 64+2) and read col-major (layout=1, stride 66). Settles whether the real kernel's B
// fragment read is already wide (b128) — i.e. whether the "B u16" occupancy finding is just the
// stale row-major test mirror, not the shipped kernel.
namespace {
// Faithful B path parameterized by LDS row stride (= depthU 64 + pad). depthU 64 contiguous K per
// N-row; col-major (layout=1) fragment read. b128 needs an 8-ELEMENT-aligned per-lane stride
// (stride % 8 == 0); de-conflict needs stride NOT a multiple of 64 (else consecutive lanes share
// banks). 66 (pad 2) de-conflicts but breaks b128 alignment -> ds_load_2addr_b32; 72 (pad 8 =
// torch LdsPadB=8) is 8-aligned AND de-conflicts -> should give b128.
std::string srcBenchB(int stride) {
    std::string s = std::to_string(stride);
    return std::string(
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Barrier;\n"
        "import cajeta.xpu.Shared;\n"
        "import cajeta.xpu.CooperativeMatrix;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void breadbench(KernelBuffer<float32> c, KernelBuffer<float16> g) {\n"
        "        Shared<float16> sa = shared float16[16 * ") + s + "];\n"
        "        Shared<float16> sb = shared float16[128 * " + s + "];\n"
        "        uint32 t = KernelThread.x();\n"
        "        while (t < 1024) {\n"
        "            uint32 nn = t / 64; uint32 kk = t % 64;\n"
        "            sb[nn * " + s + " + kk] = g[t];\n"
        "            if (t < 256) { uint32 mr = t / 16; uint32 mk = t % 16; sa[mr * " + s + " + mk] = g[t]; }\n"
        "            t = t + 128;\n"
        "        }\n"
        "        Barrier.workgroup();\n"
        "        CooperativeMatrix<float16,16,16,0> wa; wa.load(sa, 0, 0, " + s + ");\n"
        "        CooperativeMatrix<float16,16,16,1> wb; wb.load(sb, 0, 1, " + s + ");\n"
        "        CooperativeMatrix<float32,16,16,2> acc; acc.splat(0.0f);\n"
        "        acc.mma(wa, wb);\n"
        "        acc.store(c, 0, 0, 16);\n"
        "    }\n"
        "}\n";
}
void dumpDs(const std::string& tag, const std::string& isa) {
    std::cerr << "[widebr] " << tag
              << ": b128=" << (countOccurrences(isa, "ds_read_b128") + countOccurrences(isa, "ds_load_b128"))
              << " 2addr_b32=" << countOccurrences(isa, "ds_load_2addr_b32")
              << " b32=" << (countOccurrences(isa, "ds_read_b32") + countOccurrences(isa, "ds_load_b32"))
              << " u16=" << (countOccurrences(isa, "ds_read_u16") + countOccurrences(isa, "ds_load_u16"))
              << " v_wmma=" << countOccurrences(isa, "v_wmma") << "\n";
}
} // namespace

// The bench's stride-66 B read is b32-paired, NOT b128 (alignment), but an 8-aligned stride (72,
// torch's LdsPadB=8) should widen it to b128 while still de-conflicting. Records the lever.
TEST(XpuWideBReadProbeTests, benchFaithfulBReadIsWide) {
    std::string isa66 = isaOf(srcBenchB(66), "breadbench");
    std::string isa72 = isaOf(srcBenchB(72), "breadbench");
    std::string isa80 = isaOf(srcBenchB(80), "breadbench");   // torch's pad 16 -> stride 80
    ASSERT_FALSE(isa66.empty()); ASSERT_FALSE(isa72.empty()); ASSERT_FALSE(isa80.empty());
    EXPECT_EQ(isa80.find("Cannot select"), std::string::npos) << isa80;
    dumpDs("stride 66 (pad 2, current)", isa66);
    dumpDs("stride 72 (pad 8)", isa72);
    dumpDs("stride 80 (pad 16, TORCH LdsPad=16)", isa80);
    int b128_80 = countOccurrences(isa80, "ds_read_b128") + countOccurrences(isa80, "ds_load_b128");
    int b128_66 = countOccurrences(isa66, "ds_read_b128") + countOccurrences(isa66, "ds_load_b128");
    EXPECT_GT(b128_80, b128_66)
        << "torch's 8-aligned LDS stride (80, pad 16) should widen the col-major B read to ds_read_b128";
}
