//
// SpirvWideVectorPhiTests — a vector of more than four lanes carried across a
// loop reaches SPIR-V codegen as four-lane phis.
//
// Why this exists. Vulkan SPIR-V carries vectors of at most four components.
// The backend legalizes a wider vector at a load, a store or arithmetic, but at
// a phi it emitted `OpPhi %v4half` whose incoming value had another type, and
// spirv-val refused the module. RADV did worse: it segfaulted compiling the
// shader (Proton, Mesa 26.2.6, 2026-11-09, cajeta-ml-gpu 23.2.7), on the kernel
// package's gemm128_float16, whose 8-wide prefetch registers cross the K loop's
// phi. emitSpirv now splits such a phi into four-lane phis before codegen.
//
// CAJETA_TEST_SPV_OUT=<file> writes the module's SPIR-V there, so it can be run
// through spirv-val by hand.
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
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace cajeta::xpu::probe;

namespace {

// The gemm128 prefetch, reduced: an 8-wide load before the loop, the next one
// under `if (more)` inside it, and the whole vector stored on every pass.
const char* kSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void carry(KernelBuffer<float16> out, KernelBuffer<float16> a, uint32 n) {\n"
    "        Vector<float16,8> p = a.vload<8>(0);\n"
    "        uint32 i = 0;\n"
    "        while (i < n) {\n"
    "            out.vstore(i * 8, p);\n"
    "            boolean more = i + 1 < n;\n"
    "            if (more) { p = a.vload<8>((i + 1) * 8); }\n"
    "            i = i + 1;\n"
    "        }\n"
    "    }\n"
    "}\n";

} // namespace

TEST(SpirvWideVectorPhiTests, anEightLaneVectorAcrossALoopReachesCodegenAsFourLanePhis) {
    using namespace cajeta::xpu::vulkan;
    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, kSource, "test.M", "widephi");
    auto k = findMethod(module->getStructures()["test.M"], "carry");
    ASSERT_NE(k, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_widephi", ctx);
    configureDeviceModule(dev, *tm);
    ASSERT_NE(lowerKernel(k, dev), nullptr);
    std::string failure;
    std::vector<uint8_t> spirv = emitSpirv(dev, *tm, &failure);
    ASSERT_FALSE(spirv.empty()) << failure;
    if (const char* out = std::getenv("CAJETA_TEST_SPV_OUT")) {
        std::ofstream f(out, std::ios::binary);
        f.write(reinterpret_cast<const char*>(spirv.data()), (std::streamsize) spirv.size());
    }
    std::string ir;
    llvm::raw_string_ostream os(ir);
    dev.print(os, nullptr);
    os.flush();
    EXPECT_EQ(ir.find("phi <8 x"), std::string::npos)
        << "an 8-lane phi reached SPIR-V codegen:\n" << ir;
}
