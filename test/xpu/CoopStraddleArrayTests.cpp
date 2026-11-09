//
// CoopStraddleArrayTests — the tier scan sees fragment arrays.
//
// Why this exists. When a kernel's tiles straddle tiers (a native float32
// accumulator beside software-tier operands), the lowering demotes every tile
// to the software tile, because a native mma cannot consume software operands.
// The scan that decides this matched a declaration's type against
// CooperativeMatrix and did not look inside an array, so a kernel declaring
// `CooperativeMatrix<float32,16,16,2>[2][4] acc;` straddled unseen and the mma
// was refused. Found 2026-11-09 by Proton on RADV: the kernel package's gemm128
// at bfloat16 failed the vulkan kernel gate, where the same tiles declared one
// by one demoted and built.
//
#include <gtest/gtest.h>

#include "KernelLoweringProbe.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <string>

using namespace cajeta::xpu::probe;

namespace {

std::string source(bool arrays) {
    std::string s =
        "package test;\n"
        "import cajeta.xpu.CooperativeMatrix;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void st(KernelBuffer<float32> c, KernelBuffer<bfloat16> a,\n"
        "                          KernelBuffer<bfloat16> b) {\n";
    if (arrays) {
        s += "        CooperativeMatrix<float32,16,16,2>[2] acc;\n"
             "        CooperativeMatrix<bfloat16,16,16,1>[2] mb;\n"
             "        CooperativeMatrix<bfloat16,16,16,0> ma;\n"
             "        for (uint32 j = 0; j < 2; j = j + 1) { acc[j].splat(0.0f); }\n"
             "        ma.load(a, 0, 0, 16);\n"
             "        for (uint32 j = 0; j < 2; j = j + 1) {\n"
             "            mb[j].load(b, j * 16, 0, 32);\n"
             "            acc[j].mma(ma, mb[j]);\n"
             "            acc[j].store(c, j * 16, 0, 32);\n"
             "        }\n";
    } else {
        s += "        CooperativeMatrix<float32,16,16,2> a0; a0.splat(0.0f);\n"
             "        CooperativeMatrix<float32,16,16,2> a1; a1.splat(0.0f);\n"
             "        CooperativeMatrix<bfloat16,16,16,1> m0;\n"
             "        CooperativeMatrix<bfloat16,16,16,1> m1;\n"
             "        CooperativeMatrix<bfloat16,16,16,0> ma;\n"
             "        ma.load(a, 0, 0, 16);\n"
             "        m0.load(b, 0, 0, 32);\n"
             "        m1.load(b, 16, 0, 32);\n"
             "        a0.mma(ma, m0);\n"
             "        a1.mma(ma, m1);\n"
             "        a0.store(c, 0, 0, 32);\n"
             "        a1.store(c, 16, 0, 32);\n";
    }
    s += "    }\n"
         "}\n";
    return s;
}

// Lowers `st` for SPIR-V; empty with the reason in `why` when it refuses.
bool lowerSpirv(bool arrays, std::string* why) {
    using namespace cajeta::xpu::vulkan;
    auto tm = createSpirvTargetMachine("vulkan1.3");
    if (!tm) { *why = "no SPIR-V target machine"; return false; }
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, source(arrays), "test.M", "straddle");
    auto k = findMethod(module->getStructures()["test.M"], "st");
    if (!k) { *why = "kernel st not found"; return false; }
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_straddle", ctx);
    configureDeviceModule(dev, *tm);
    try {
        if (!lowerKernel(k, dev)) { *why = "lowerKernel returned null"; return false; }
    } catch (const std::exception& ex) {
        *why = ex.what();
        return false;
    } catch (...) {
        *why = "the lowering refused the kernel";
        return false;
    }
    return true;
}

} // namespace

// The control: tiles declared one by one straddle, demote and lower.
TEST(CoopStraddleArrayTests, namedFragmentsThatStraddleDemoteAndLower) {
    std::string why;
    EXPECT_TRUE(lowerSpirv(false, &why)) << why;
}

TEST(CoopStraddleArrayTests, fragmentArraysThatStraddleDemoteAndLower) {
    std::string why;
    EXPECT_TRUE(lowerSpirv(true, &why)) << why;
}
