//
// CajetaXPU — workgroup shared memory lowering (NVPTX, addrspace 3).
//
// Pins that `Shared<T> tile = shared T[N];` (the `shared` placement keyword)
// lowers to a per-block addrspace(3) global and that indexing it produces
// shared-space loads/stores. PTX is text, so no GPU is needed; the on-device
// counterpart is XpuSharedDeviceTests.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"
#include "cajeta/error/Exception.h"

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
              / ("cajeta_xpu_sharedemit_" + std::to_string(rng()));
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
                 / ("cajeta_xpu_sharedemit_arch_" + std::to_string(rng()));
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
    llvm::Module deviceModule("xpu_shared_device", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    if (!lowerKernel(method, deviceModule)) return {};
    return emitPtx(deviceModule, *tm);
}

// A 256-wide single-block tree reduction over shared memory: load a tile from
// global, barrier, halve-and-add down to tile[0], write it out.
const char* kReduceSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Workgroup;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Shared;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void reduce(KernelBuffer<int32> out, KernelBuffer<int32> in, uint32 n) {\n"
    "        Shared<int32> tile = shared int32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 g = KernelThread.globalIdX();\n"
    "        if (g < n) {\n"
    "            tile[t] = in[g];\n"
    "        } else {\n"
    "            tile[t] = 0;\n"
    "        }\n"
    "        Barrier.workgroup();\n"
    "        for (uint32 s = 128; s > 0; s >>= 1) {\n"
    "            if (t < s) {\n"
    "                tile[t] += tile[t + s];\n"
    "            }\n"
    "            Barrier.workgroup();\n"
    "        }\n"
    "        if (t == 0) {\n"
    "            out[Workgroup.x()] = tile[0];\n"
    "        }\n"
    "    }\n"
    "}\n";

} // namespace

// `shared int32[256]` lowers to a .shared state-space allocation, indexed by
// shared-space loads/stores, with the workgroup barrier between phases.

// array-literals §4 — `Shared<int32> tile = shared [10,20,30];` allocates a
// per-block shared tile of the literal's length AND populates it with the
// values. Lowers to a .shared global with st.shared populate writes; reading
// tile[t] is a shared-space load.
const char* kSharedLiteralSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n"
    "public class L {\n"
    "    @Kernel\n"
    "    public static void useLit(KernelBuffer<int32> out) {\n"
    "        Shared<int32> tile = shared [10, 20, 30];\n"
    "        uint32 t = KernelThread.x();\n"
    "        if (t < 3) {\n"
    "            out[t] = tile[t];\n"
    "        }\n"
    "    }\n"
    "}\n";

TEST(XpuNvptxSharedEmitTests, lowersSharedArrayLiteral) {
    std::string ptx = lowerToPtx(kSharedLiteralSrc, "test.L", "useLit");
    ASSERT_FALSE(ptx.empty());

    EXPECT_NE(ptx.find(".visible .entry useLit"), std::string::npos) << ptx;
    // The literal tile lands in the .shared state space.
    EXPECT_NE(ptx.find(".shared"), std::string::npos) << ptx;
    // The literal's values are stored in (populate) — shared-space writes.
    EXPECT_NE(ptx.find("st.shared"), std::string::npos) << ptx;
    // Reading tile[t] is a shared-space load.
    EXPECT_NE(ptx.find("ld.shared"), std::string::npos) << ptx;
}

// A runtime-sized `shared T[expr]` (expr is a kernel param, not a constant) is
// DYNAMIC shared memory: an external, unsized .shared region sized by the
// launch's `shared:` byte count. Distinguished in PTX by `.extern .shared`.

// Review fix D1 — a shared literal with a NON-constant (per-thread) element is
// rejected. Every thread runs the populate stores with no barrier, so a
// thread-varying value would race on the shared slots; only compile-time
// constants are allowed (use `shared T[N]` + runtime assignment otherwise).
TEST(XpuNvptxSharedEmitTests, sharedLiteralRejectsNonConstantElement) {
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Shared;\n"
        "public class N {\n"
        "    @Kernel\n"
        "    public static void bad(KernelBuffer<int32> out) {\n"
        "        uint32 t = KernelThread.x();\n"
        "        Shared<int32> tile = shared [t, 1];\n"  // t is per-thread
        "        out[0] = tile[0];\n"
        "    }\n"
        "}\n";
    EXPECT_THROW(lowerToPtx(src, "test.N", "bad"), cajeta::Exception);
}

// `shared` is device-only: used outside an @Kernel (i.e. on the host codegen
// path), it must be rejected (XPU-K03), not mis-lowered as a host allocation.
// compile(module) only parses + builds prototypes, so drive host body codegen
// explicitly (as CompilerOptionTests does) to reach NewExpression::generateCode.
TEST(XpuNvptxSharedEmitTests, sharedOutsideKernelRejected) {
    const char* src =
        "package test;\n"
        "import cajeta.xpu.Shared;\n"
        "public class M {\n"
        "    public static int32 oops() {\n"
        "        Shared<int32> tile = shared int32[8];\n"
        "        return tile[0];\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto oops = findMethod(module->getStructures()["test.M"], "oops");
    ASSERT_NE(oops, nullptr);
    oops->getLlvmFunctionType();
    EXPECT_THROW(oops->generateCode(), cajeta::Exception);
}
