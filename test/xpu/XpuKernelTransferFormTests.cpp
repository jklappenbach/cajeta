//
// CajetaXPU — `#` and `#=` inside a kernel body, and the diagnostic that names
// an expression form the kernel walk cannot lower.
//
// WHY THE TWO BELONG IN ONE FILE. They were found together. Every backend
// skipped `ExpandProbe.f32MatVecKernel` with
//
//     unsupported construct — expression form in kernel body
//
// and that message names neither the line nor the construct, so the only way
// to learn the culprit was `int64 c #= 0;` was to delete statements from the
// kernel one at a time until it lowered. A skip that cannot say what it choked
// on is the same invisible-absence failure as a vacuous pass: the kernel is
// gone from the build and the note does not tell you how to get it back.
//
// WHY `#=` LOWERS RATHER THAN BEING REJECTED BY NAME. A device kernel has no
// heap, no drop chain, and no live-allocation set — every value in it is a
// register or device-local memory, and `heap` is not reachable from a kernel
// body at all. `#` carries a title; in a kernel there is no title to carry.
// So `int64 c #= 0` denotes exactly what `int64 c = 0` denotes, and the
// lowering forwards to the operand.
//
// The first two tests assert that as an IR EQUALITY rather than as "it
// compiled". "It compiled" would also hold for a lowering that dropped the
// initializer on the floor, which is the failure mode that actually matters:
// the kernel would come back and quietly compute from an undefined `c`.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/cpu/CpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"
#include "cajeta/xpu/nvidia/NvptxBackend.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"
#include "cajeta/error/Exception.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_xfer_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_xfer_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = base / "test" / "M.cajeta";
    auto m = compiler.createModule(full.string(), base.string(),
                                   archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

std::string functionIr(llvm::Function* f) {
    std::string s;
    llvm::raw_string_ostream os(s);
    f->print(os);
    os.flush();
    return s;
}

// The two kernels differ ONLY in the initializer's spelling, so their bodies
// must be character-identical once the function name is out of the way.
const char* kPair =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void plainInit(KernelBuffer<int64> out, uint32 n,\n"
    "            int64 cols) {\n"
    "        uint32 gi = KernelThread.globalIdX();\n"
    "        if (gi < n) {\n"
    "            int64 c = 0;\n"
    "            int64 s = 0;\n"
    "            while (c < cols) { s = s + c; c = c + 4L; }\n"
    "            out[(int64) gi] = s;\n"
    "        }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void sharpInit(KernelBuffer<int64> out, uint32 n,\n"
    "            int64 cols) {\n"
    "        uint32 gi = KernelThread.globalIdX();\n"
    "        if (gi < n) {\n"
    "            int64 c #= 0;\n"
    "            int64 s = 0;\n"
    "            while (c < cols) { s = s + c; c = c + 4L; }\n"
    "            out[(int64) gi] = s;\n"
    "        }\n"
    "    }\n"
    "}\n";

// `#` in value position (`s #= c` on a later store, and `f(#v)`-shaped
// forwarding) rides the same node, so one more kernel pins the assignment
// spelling as well as the declaration spelling.
const char* kAssignPair =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void plainAssign(KernelBuffer<int64> out, uint32 n) {\n"
    "        uint32 gi = KernelThread.globalIdX();\n"
    "        int64 v = (int64) gi;\n"
    "        int64 w = 0;\n"
    "        w = v * 3L;\n"
    "        if (gi < n) { out[(int64) gi] = w; }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void sharpAssign(KernelBuffer<int64> out, uint32 n) {\n"
    "        uint32 gi = KernelThread.globalIdX();\n"
    "        int64 v = (int64) gi;\n"
    "        int64 w = 0;\n"
    "        w #= v * 3L;\n"
    "        if (gi < n) { out[(int64) gi] = w; }\n"
    "    }\n"
    "}\n";

} // namespace

TEST(XpuKernelTransferFormTests, aTransferInitializerLowersLikeAPlainOneOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kPair);
    auto plain = findMethod(module->getStructures()["test.M"], "plainInit");
    auto sharp = findMethod(module->getStructures()["test.M"], "sharpInit");
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(sharp, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_xfer_cpu", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);

    llvm::Function* pf = cajeta::xpu::cpu::lowerKernel(plain, host);
    llvm::Function* sf = cajeta::xpu::cpu::lowerKernel(sharp, host);
    ASSERT_NE(pf, nullptr);
    ASSERT_NE(sf, nullptr);

    std::string pi = functionIr(pf);
    std::string si = functionIr(sf);
    // Only the symbol differs; rename so a mismatch anywhere else is visible.
    for (size_t at = si.find("sharpInit"); at != std::string::npos;
         at = si.find("sharpInit", at))
        si.replace(at, 9, "plainInit");
    EXPECT_EQ(pi, si);
}

TEST(XpuKernelTransferFormTests, aTransferAssignmentLowersLikeAPlainOneOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kAssignPair);
    auto plain = findMethod(module->getStructures()["test.M"], "plainAssign");
    auto sharp = findMethod(module->getStructures()["test.M"], "sharpAssign");
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(sharp, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_xfer_cpu2", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);

    llvm::Function* pf = cajeta::xpu::cpu::lowerKernel(plain, host);
    llvm::Function* sf = cajeta::xpu::cpu::lowerKernel(sharp, host);
    std::string pi = functionIr(pf);
    std::string si = functionIr(sf);
    for (size_t at = si.find("sharpAssign"); at != std::string::npos;
         at = si.find("sharpAssign", at))
        si.replace(at, 11, "plainAssign");
    EXPECT_EQ(pi, si);
}

// The NVIDIA leg of the same fix. Emit-time only: the rejection (or its
// absence) is decided during body lowering, long before any driver call, so
// this runs on a box with no GPU.
TEST(XpuKernelTransferFormTests, aTransferInitializerLowersOnNvptx) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kPair);
    auto sharp = findMethod(module->getStructures()["test.M"], "sharpInit");
    ASSERT_NE(sharp, nullptr);

    auto tm = cajeta::xpu::nvidia::createNvptxTargetMachine("sm_70");
    ASSERT_NE(tm, nullptr) << "nvptx target not registered";
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_xfer_nvptx", ctx);
    cajeta::xpu::nvidia::configureDeviceModule(dev, *tm);
    llvm::Function* f = nullptr;
    ASSERT_NO_THROW(f = cajeta::xpu::nvidia::lowerKernel(sharp, dev));
    ASSERT_NE(f, nullptr);
    // The initializer survived: the loop counter is stored before the loop.
    EXPECT_NE(functionIr(f).find("store i64 0"), std::string::npos)
        << functionIr(f);
}

// The other half. An expression form the walk genuinely cannot lower must say
// WHICH form and WHERE, so the next person does not bisect the kernel by hand.
// A ternary is the probe because nothing in the walk handles it today; if that
// changes, move this to whatever is still unhandled rather than deleting it.
TEST(XpuKernelTransferFormTests, anUnloweredExpressionNamesItsKindAndLine) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void k(KernelBuffer<int32> out, uint32 n) {\n"
        "        uint32 gi = KernelThread.globalIdX();\n"
        "        int32 a = 3;\n"
        "        int32 b = (a > 2) ? a : 7;\n"
        "        if (gi < n) { out[(int64) gi] = b; }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src);
    auto k = findMethod(module->getStructures()["test.M"], "k");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_xfer_reject", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);
    try {
        cajeta::xpu::cpu::lowerKernel(k, host);
        FAIL() << "expected XPU-N01 for an expression form the walk cannot "
                  "lower";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "XPU-N01");
        const std::string& m = e.getMessage();
        // The line the ternary is on, so the note points at source.
        EXPECT_NE(m.find("line 9"), std::string::npos) << m;
        // And the node kind, so the note says what to stop writing.
        EXPECT_NE(m.find("BooleanSwitch"), std::string::npos) << m;
    }
}
