//
// AmdgpuAssemblyKeepsModuleTests — assembling a code object leaves the
// caller's device module as it was.
//
// Why this exists. assembleHsaco runs the device pipeline on the module it is
// handed, in place. For several arches assembleHsacoPerArch cloned the module
// first, but for one arch it handed over the caller's own module, and the
// registration emitter still held the kernel's llvm::Function* from lowering.
// Building the kernel package for gfx942 alone (2026-11-09) crashed the
// compiler with SIGSEGV in applyNativeOps reading that pointer after the
// pipeline had replaced the function. The single-arch path now clones too.
//
#include <gtest/gtest.h>

#include "KernelLoweringProbe.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

using namespace cajeta::xpu;
using namespace cajeta::xpu::probe;

namespace {

const char* kSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void mad(KernelBuffer<float32> out, KernelBuffer<float32> a,\n"
    "                           KernelBuffer<float32> b, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { out[i] = a[i] * b[i] + out[i]; }\n"
    "    }\n"
    "}\n";

std::string print(llvm::Module& m) {
    std::string s;
    llvm::raw_string_ostream os(s);
    m.print(os, nullptr);
    os.flush();
    return s;
}

// Lowers `mad`, assembles it for `arch` alone, and reports whether the device
// module and the kernel's Function are what they were before assembly.
void assembleOne(const std::string& arch) {
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, kSource, "test.M", "asmkeep");
    auto k = findMethod(module->getStructures()["test.M"], "mad");
    ASSERT_NE(k, nullptr);
    auto tm = amd::createAmdgpuTargetMachine(arch);
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("asmkeep_dev", ctx);
    amd::configureDeviceModule(dev, *tm);
    llvm::Function* kfn = amd::lowerKernel(k, dev);
    ASSERT_NE(kfn, nullptr);
    const std::string name = kfn->getName().str();
    const std::string before = print(dev);
    std::vector<amd::ArchHsaco> objs = amd::assembleHsacoPerArch(dev, {arch});
    if (objs.empty()) GTEST_SKIP() << "no code object (ld.lld not on PATH?)";
    EXPECT_EQ(dev.getFunction(name), kfn) << "the kernel's Function was replaced";
    EXPECT_EQ(print(dev), before) << "assembly rewrote the caller's device module";
}

} // namespace

TEST(AmdgpuAssemblyKeepsModuleTests, oneArchLeavesTheModuleAsItWas) {
    assembleOne("gfx1151");
}

TEST(AmdgpuAssemblyKeepsModuleTests, oneDatacenterArchLeavesTheModuleAsItWas) {
    assembleOne("gfx942");
}
