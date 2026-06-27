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

// U1.1.a/b/c: col-major B load vs the row-major control — record the routing verdict.
TEST(XpuWideBReadProbeTests, bColMajorBReadWidensVsRowMajor) {
    std::string isaRow = isaOf(srcWithBLayout(0), "bread");   // current behavior
    std::string isaCol = isaOf(srcWithBLayout(1), "bread");   // hypothesis
    ASSERT_FALSE(isaRow.empty()) << "row-major B kernel failed to lower";
    ASSERT_FALSE(isaCol.empty()) << "col-major B kernel failed to lower";
    EXPECT_EQ(isaCol.find("Cannot select"), std::string::npos) << isaCol;

    Widths row = readWidths(isaRow);
    Widths col = readWidths(isaCol);

    bool kernelOnly = (col.u16 < row.u16) && (col.b128 > row.b128);
    const char* verdict = kernelOnly
        ? "KERNEL-ONLY (col-major B vectorizes; U2 codegen NOT needed → U3 layout)"
        : "CODEGEN (col-major B did NOT widen; U2 codegen required)";

    std::cerr << "[wide-b] matrix-B ds_read widths (1x A-frag + 1x B-frag):\n"
              << "  row-major B (layout 0, current): b128=" << row.b128
              << " b64=" << row.b64 << " u16=" << row.u16 << "\n"
              << "  col-major B (layout 1, hypoth.): b128=" << col.b128
              << " b64=" << col.b64 << " u16=" << col.u16 << "\n"
              << "  VERDICT: " << verdict << "\n";

    SUCCEED() << "probe measured; verdict logged for plan U1.3";
}
