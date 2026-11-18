//
// SpirvWideVectorCastTests — a cast of more than four lanes reaches SPIR-V
// codegen as four-lane casts (spirv-kernel-parity 1.1).
//
// Vulkan SPIR-V carries vectors of at most four components. LLVM's legalizer
// splits an eight-lane sext into two halves and then cannot select the
// G_CONCAT_VECTORS that joins them, so cajeta-llm's f16 column kernels
// (Vector<int8,16>.widenLo().toF32().toF16()) had no vulkan device code on
// 2026-11-18. emitSpirv now splits such a cast before codegen.
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
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <random>
#include <string>
#include <vector>
#include "KernelLoweringProbe.h"

using cajeta::Compiler;
using cajeta::xpu::probe::compileForInspection;
using cajeta::xpu::probe::findMethod;

namespace {

// Sixteen bytes a thread, widened to two eight-lane int16 halves, converted to
// f32 (low half) and through f32 to f16 (high half), every lane weighted.
std::string castSource() {
    std::string s =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class WC {\n"
        "    @Kernel\n"
        "    public static void wc(KernelBuffer<float32> out, KernelBuffer<int32> idx, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            Vector<int8,16> q = idx.vload<4>((int64) i * 4L).asBytes();\n"
        "            Vector<float32,8> lo = q.widenLo().toF32();\n"
        "            Vector<float16,8> hi = q.widenHi().toF32().toF16();\n"
        "            float32 s = 0.0f;\n";
    for (int k = 0; k < 8; ++k) {
        s += "            s = s + lo[" + std::to_string(k) + "] * " + std::to_string(k + 1)
           + ".0f + (float32) hi[" + std::to_string(k) + "] * " + std::to_string(k + 17)
           + ".0f;\n";
    }
    s += "            out[(int64) i] = s;\n"
         "        }\n"
         "    }\n"
         "}\n";
    return s;
}

float expectedFor(const uint8_t* bytes) {
    float s = 0.0f;
    for (int k = 0; k < 8; ++k) s = s + (float) (int8_t) bytes[k] * (float) (k + 1);
    for (int k = 0; k < 8; ++k) s = s + (float) (int8_t) bytes[8 + k] * (float) (k + 17);
    return s;
}

struct SpirvLowered {
    std::string ir;
    std::vector<uint8_t> spirv;
    std::string failure;
};

SpirvLowered lowerCasts() {
    using namespace cajeta::xpu::vulkan;
    SpirvLowered out;
    Compiler compiler;
    auto module = compileForInspection(compiler, castSource(), "test.WC", "spvwidecast");
    auto k = findMethod(module->getStructures()["test.WC"], "wc");
    if (k == nullptr) { out.failure = "no kernel"; return out; }
    auto tm = createSpirvTargetMachine("vulkan1.3");
    if (tm == nullptr) { out.failure = "no SPIR-V target"; return out; }
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_widecast_spirv", ctx);
    configureDeviceModule(dev, *tm);
    if (lowerKernel(k, dev) == nullptr) { out.failure = "lowering refused"; return out; }
    out.spirv = emitSpirv(dev, *tm, &out.failure);
    { llvm::raw_string_ostream os(out.ir); dev.print(os, nullptr); }
    return out;
}

}  // namespace

// SPIR-V codegen selects the kernel, and no cast wider than four lanes reaches it.
TEST(SpirvWideVectorCastTests, eightLaneWidensAndConversionsReachCodegenAsFourLaneCasts) {
    SpirvLowered l = lowerCasts();
    ASSERT_FALSE(l.spirv.empty()) << "SPIR-V codegen failed: " << l.failure;
    for (const char* op : {"sext <8", "sitofp <8", "fptrunc <8"})
        EXPECT_EQ(l.ir.find(op), std::string::npos) << op << " reached codegen:\n" << l.ir;
}

// Every lane of both halves matches the host on a real Vulkan device.
TEST(SpirvWideVectorCastTests, eightLaneWidensAndConversionsRunOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) GTEST_SKIP() << "no Vulkan compute device available";
    SpirvLowered l = lowerCasts();
    ASSERT_FALSE(l.spirv.empty()) << "SPIR-V codegen failed: " << l.failure;

    const uint32_t n = 4096;
    std::vector<uint8_t> idx(std::size_t(n) * 16);
    std::mt19937 rng(0x5eed16);
    for (auto& b : idx) b = (uint8_t) rng();
    std::vector<float> out(n, 0.0f);

    VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    VulkanDriver::Buffer dOut = vk.alloc(out.size() * sizeof(float));
    VulkanDriver::Buffer dIdx = vk.alloc(idx.size());
    VulkanDriver::Buffer dN = vk.alloc(sizeof(uint32_t));
    ASSERT_NE(dOut, 0u);
    ASSERT_NE(dIdx, 0u);
    ASSERT_NE(dN, 0u);
    ASSERT_TRUE(vk.upload(dOut, out.data(), out.size() * sizeof(float)));
    ASSERT_TRUE(vk.upload(dIdx, idx.data(), idx.size()));
    ASSERT_TRUE(vk.upload(dN, &n, sizeof(uint32_t)));
    const unsigned block = kVulkanLocalSizeX;
    ASSERT_TRUE(vk.launch(l.spirv.data(), l.spirv.size(), "wc", {dOut, dIdx, dN},
                          (n + block - 1) / block));
    ASSERT_TRUE(vk.download(out.data(), dOut, out.size() * sizeof(float)));
    vk.free(dOut);
    vk.free(dIdx);
    vk.free(dN);

    std::size_t bad = 0;
    for (uint32_t i = 0; i < n; ++i) {
        float want = expectedFor(&idx[std::size_t(i) * 16]);
        if (out[i] != want) {
            if (bad < 5) ADD_FAILURE() << "i=" << i << " got " << out[i] << " want " << want;
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0u);
}
