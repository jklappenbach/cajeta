//
// A shared array read at constant indices on the SPIR-V backend
// (xpu-kernel-adaptor 3.2.5). `part[1]` folds to a constant-expression GEP,
// and LLVM's SPIRVLegalizePointerCast pass segfaulted on a load through one,
// taking the compiler down with cajeta-llm's q4kQ8IdDownCombineKernel. The
// SPIR-V pipeline now turns every such address into an instruction first.
//
#include "gtest/gtest.h"

#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <cstdint>
#include <string>
#include <vector>
#include "KernelLoweringProbe.h"

using cajeta::Compiler;
using cajeta::xpu::probe::compileForInspection;
using cajeta::xpu::probe::findMethod;

namespace {

// Eight slots written by lanes 0..7 of each group, then every lane sums them
// by constant index: sum over s of (s + 1) * 10 = 360.
const char* kSource =
    "package test;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n"
    "public class SH {\n"
    "    @Kernel\n"
    "    public static void sum8(KernelBuffer<float32> out, uint32 n) {\n"
    "        Shared<float32> part = shared float32[8];\n"
    "        uint32 tid = KernelThread.x();\n"
    "        if (tid < 8) { part[tid] = (float32) (tid + 1) * 10.0f; }\n"
    "        Barrier.workgroup();\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            out[i] = part[0] + part[1] + part[2] + part[3]\n"
    "                + part[4] + part[5] + part[6] + part[7];\n"
    "        }\n"
    "    }\n"
    "}\n";

}  // namespace

TEST(SpirvConstantAddressTests, aSharedArrayReadAtConstantIndicesCompilesAndSums) {
    using namespace cajeta::xpu::vulkan;
    Compiler compiler;
    auto module = compileForInspection(compiler, kSource, "test.SH", "spvconst");
    auto k = findMethod(module->getStructures()["test.SH"], "sum8");
    ASSERT_NE(k, nullptr);
    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_spv_const_addr", ctx);
    configureDeviceModule(dev, *tm);
    ASSERT_NE(lowerKernel(k, dev), nullptr);
    std::string failure;
    std::vector<uint8_t> spirv = emitSpirv(dev, *tm, &failure);
    ASSERT_FALSE(spirv.empty()) << "SPIR-V codegen failed: " << failure;
    if (!VulkanDriver::available()) GTEST_SKIP() << "no Vulkan compute device available";

    const uint32_t n = 4096;
    std::vector<float> out(n, -1.0f);
    VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    VulkanDriver::Buffer dOut = vk.alloc(n * sizeof(float));
    VulkanDriver::Buffer dN = vk.alloc(sizeof(uint32_t));
    ASSERT_TRUE(vk.upload(dOut, out.data(), n * sizeof(float)));
    ASSERT_TRUE(vk.upload(dN, &n, sizeof(uint32_t)));
    const unsigned block = kVulkanLocalSizeX;
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "sum8", {dOut, dN},
                          (n + block - 1) / block));
    ASSERT_TRUE(vk.download(out.data(), dOut, n * sizeof(float)));
    vk.free(dOut);
    vk.free(dN);
    std::size_t bad = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (out[i] != 360.0f) {
            if (bad < 5) ADD_FAILURE() << "i=" << i << " got " << out[i];
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0u);
}
