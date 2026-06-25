//
// CajetaXPU — NVPTX lowering beyond the SAXPY subset.
//
// Pins that the device lowerer now handles general compute bodies: loops
// (with mutable counters/accumulators), compound assignment, and the full
// integer operator set (mod, shifts, bitwise). PTX is text, so no GPU is
// needed; the real-device counterpart is XpuLoopDeviceTests.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace cajeta::xpu::nvidia;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_loopemit_" + std::to_string(rng()));
    std::filesystem::create_directories(base);
    std::filesystem::path rel;
    size_t start = 0;
    for (size_t i = 0; i <= fqClassName.size(); ++i) {
        if (i == fqClassName.size() || fqClassName[i] == '.') {
            rel /= fqClassName.substr(start, i - start);
            start = i + 1;
        }
    }
    rel += ".cajeta";
    auto full = base / rel;
    std::filesystem::create_directories(full.parent_path());
    std::ofstream out(full); out << source; out.close();
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_loopemit_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

std::string lowerToPtx(const std::string& src, const std::string& fqClass,
                       const std::string& kernel) {
    Compiler compiler;
    auto module = compileForInspection(compiler, src, fqClass);
    auto klass = module->getStructures()[fqClass];
    if (!klass) return {};
    auto method = findMethod(klass, kernel);
    if (!method) return {};
    auto tm = createNvptxTargetMachine("sm_89");
    if (!tm) return {};
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_loop_device", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    if (!lowerKernel(method, deviceModule)) return {};
    return emitPtx(deviceModule, *tm);
}

} // namespace

// A for-loop kernel with a mutable counter and accumulator + compound
// assignment lowers to PTX with a real loop (backedge branch + predicate)
// and the expected arithmetic. Each thread strided-sums `in` into `out[i]`.
TEST(XpuNvptxLoopEmitTests, lowersStridedSumLoop) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void strideSum(KernelBuffer<int32> out, KernelBuffer<int32> in,\n"
        "                                  uint32 n, uint32 stride) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        int32 acc = 0;\n"
        "        for (uint32 j = i; j < n; j += stride) {\n"
        "            acc += in[j];\n"
        "        }\n"
        "        out[i] = acc;\n"
        "    }\n"
        "}\n";
    std::string ptx = lowerToPtx(src, "test.M", "strideSum");
    ASSERT_FALSE(ptx.empty());

    EXPECT_NE(ptx.find(".visible .entry strideSum"), std::string::npos) << ptx;
    // A loop: a branch (backedge) and an unsigned predicate guard on j < n.
    EXPECT_NE(ptx.find("bra"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("setp"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find(".u32"), std::string::npos) << ptx;
    // acc += in[j] : signed 32-bit add of a global load.
    EXPECT_NE(ptx.find("add.s32"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("ld.global"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("st.global"), std::string::npos) << ptx;
}

// The integer operator set beyond +-*/ : modulo, shifts, and bitwise ops
// all lower. i is unsigned, so `>>` is a logical shift.
TEST(XpuNvptxLoopEmitTests, lowersIntegerOpsMix) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void opsmix(KernelBuffer<uint32> out, uint32 n, uint32 d) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            out[i] = ((i % d) ^ (i << 1)) | (i >> 2);\n"
        "        }\n"
        "    }\n"
        "}\n";
    std::string ptx = lowerToPtx(src, "test.M", "opsmix");
    ASSERT_FALSE(ptx.empty());

    EXPECT_NE(ptx.find(".visible .entry opsmix"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("rem"), std::string::npos) << ptx;  // i % 7
    EXPECT_NE(ptx.find("shl"), std::string::npos) << ptx;  // i << 1
    EXPECT_NE(ptx.find("shr"), std::string::npos) << ptx;  // i >> 2 (logical)
    EXPECT_NE(ptx.find("xor.b32"), std::string::npos) << ptx;
}

// Item 7: a POD struct passed by value as a kernel arg lowers to PTX. The kernel
// takes `Params { int32 mul; int32 add; }` by value and reads its fields to
// compute out[i] = i*mul + add — the struct rides param space, the fields feed a
// signed multiply/add, and the result stores to global memory.
TEST(XpuNvptxLoopEmitTests, lowersPodStructArg) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class Params {\n"
        "    int32 mul;\n"
        "    int32 add;\n"
        "    public Params(int32 mul, int32 add)"
        " { this.mul = mul; this.add = add; }\n"
        "}\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void k(KernelBuffer<int32> out, Params p) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        out[i] = (int32)i * p.mul + p.add;\n"
        "    }\n"
        "}\n";
    std::string ptx = lowerToPtx(src, "test.M", "k");
    ASSERT_FALSE(ptx.empty());

    EXPECT_NE(ptx.find(".visible .entry k"), std::string::npos) << ptx;
    // The struct fields arrive through kernel param space.
    EXPECT_NE(ptx.find("ld.param"), std::string::npos) << ptx;
    // i*mul + add : a signed multiply (possibly fused as mad) and a store.
    bool hasMul = ptx.find("mul.lo.s32") != std::string::npos
               || ptx.find("mad.lo.s32") != std::string::npos;
    EXPECT_TRUE(hasMul) << ptx;
    EXPECT_NE(ptx.find("st.global"), std::string::npos) << ptx;
}
