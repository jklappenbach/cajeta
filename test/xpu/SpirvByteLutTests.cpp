//
// Vector.lut4 on the SPIR-V backend (xpu-kernel-adaptor 3.2.5).
//
// Vulkan has no byte permute and no 16-component vector. The portable lut4
// spilled the table to a private <16 x i8> and rebuilt the result lane by
// lane, and LLVM's SPIR-V legalizer cannot select that 16-lane build, so the
// IQ4_NL coop kernels ended the whole vulkan build with an LLVM fatal error.
// The SPIR-V target now picks each byte out of the table's four words.
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

const int8_t kTable[16] = {-127, -104, -83, -65, -49, -35, -22, -10,
                           1, 13, 25, 38, 53, 69, 89, 113};

// Both nibbles of 16 runtime bytes through the IQ4 table, every lane weighted
// so a lane that reads the wrong byte moves the sum.
std::string lutSource() {
    std::string s =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class LUT {\n"
        "    @Kernel\n"
        "    public static void lut(KernelBuffer<int32> out, KernelBuffer<int32> idx,"
        " uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            Vector<int8,16> kv = heap Vector<int8,16>(-127, -104, -83, -65,"
        " -49, -35, -22, -10, 1, 13, 25, 38, 53, 69, 89, 113);\n"
        "            Vector<int8,16> q = idx.vload<4>((int64) i * 4L).asBytes();\n"
        "            Vector<int8,16> lo = (q & 15).lut4(kv);\n"
        "            Vector<int8,16> hi = ((q >> 4) & 15).lut4(kv);\n"
        "            int32 s = 0;\n";
    for (int k = 0; k < 16; ++k) {
        s += "            s = s + (int32) lo[" + std::to_string(k) + "] * "
           + std::to_string(k + 1) + " + (int32) hi[" + std::to_string(k) + "] * "
           + std::to_string(k + 17) + ";\n";
    }
    s += "            out[(int64) i] = s;\n"
         "        }\n"
         "    }\n"
         "}\n";
    return s;
}

int32_t expectedFor(const uint8_t* bytes) {
    int32_t s = 0;
    for (int k = 0; k < 16; ++k) {
        s += (int32_t) kTable[bytes[k] & 15] * (k + 1)
           + (int32_t) kTable[(bytes[k] >> 4) & 15] * (k + 17);
    }
    return s;
}

struct SpirvLowered {
    std::string ir;
    std::vector<uint8_t> spirv;
    std::string failure;
};

SpirvLowered lowerLut() {
    using namespace cajeta::xpu::vulkan;
    SpirvLowered out;
    Compiler compiler;
    auto module = compileForInspection(compiler, lutSource(), "test.LUT", "spvlut");
    auto k = findMethod(module->getStructures()["test.LUT"], "lut");
    if (k == nullptr) { out.failure = "no kernel"; return out; }
    auto tm = createSpirvTargetMachine("vulkan1.3");
    if (tm == nullptr) { out.failure = "no SPIR-V target"; return out; }
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_lut4_spirv", ctx);
    configureDeviceModule(dev, *tm);
    if (lowerKernel(k, dev) == nullptr) { out.failure = "lowering refused"; return out; }
    { llvm::raw_string_ostream os(out.ir); dev.print(os, nullptr); }
    out.spirv = emitSpirv(dev, *tm, &out.failure);
    return out;
}

}  // namespace

// The lookup emits no private table, and SPIR-V codegen selects it.
TEST(SpirvByteLutTests, lut4LowersWithoutAScratchTable) {
    SpirvLowered l = lowerLut();
    ASSERT_FALSE(l.spirv.empty()) << "SPIR-V codegen failed: " << l.failure;
    EXPECT_EQ(l.ir.find("lut.tbl"), std::string::npos)
        << "lut4 must not spill its table on SPIR-V:\n" << l.ir;
    EXPECT_NE(l.ir.find("lut.bytes"), std::string::npos)
        << "the SPIR-V word-select lookup did not run:\n" << l.ir;
}

// Every lane of both nibble lookups matches the host on a real Vulkan device.
TEST(SpirvByteLutTests, lut4RunsOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) GTEST_SKIP() << "no Vulkan compute device available";
    SpirvLowered l = lowerLut();
    ASSERT_FALSE(l.spirv.empty()) << "SPIR-V codegen failed: " << l.failure;

    const uint32_t n = 4096;
    std::vector<uint8_t> idx(std::size_t(n) * 16);
    std::mt19937 rng(0x1a2b3c);
    for (auto& b : idx) b = (uint8_t) rng();
    std::vector<int32_t> out(n, 0);

    VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    VulkanDriver::Buffer dOut = vk.alloc(out.size() * sizeof(int32_t));
    VulkanDriver::Buffer dIdx = vk.alloc(idx.size());
    VulkanDriver::Buffer dN = vk.alloc(sizeof(uint32_t));
    ASSERT_NE(dOut, 0u);
    ASSERT_NE(dIdx, 0u);
    ASSERT_NE(dN, 0u);
    ASSERT_TRUE(vk.upload(dOut, out.data(), out.size() * sizeof(int32_t)));
    ASSERT_TRUE(vk.upload(dIdx, idx.data(), idx.size()));
    ASSERT_TRUE(vk.upload(dN, &n, sizeof(uint32_t)));
    const unsigned block = kVulkanLocalSizeX;
    ASSERT_TRUE(vk.launch(l.spirv.data(), l.spirv.size(), "lut", {dOut, dIdx, dN},
                          (n + block - 1) / block));
    ASSERT_TRUE(vk.download(out.data(), dOut, out.size() * sizeof(int32_t)));
    vk.free(dOut);
    vk.free(dIdx);
    vk.free(dN);

    std::size_t bad = 0;
    for (uint32_t i = 0; i < n; ++i) {
        int32_t want = expectedFor(&idx[std::size_t(i) * 16]);
        if (out[i] != want) {
            if (bad < 5) ADD_FAILURE() << "i=" << i << " got " << out[i] << " want " << want;
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0u);
}

namespace {

// Two lanes, not a whole word: the lookup pads to four and cuts back.
const char* kTwoLaneSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class LUT {\n"
    "    @Kernel\n"
    "    public static void lut(KernelBuffer<int32> out, KernelBuffer<int32> idx, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<int8,16> kv = heap Vector<int8,16>(-127, -104, -83, -65,"
    " -49, -35, -22, -10, 1, 13, 25, 38, 53, 69, 89, 113);\n"
    "            int32 a = idx[(int64) i];\n"
    "            Vector<int8,2> q = stack Vector<int8,2>((int8) a, (int8) (a >> 8));\n"
    "            Vector<int8,2> lo = (q & 15).lut4(kv);\n"
    "            out[(int64) i] = (int32) lo[0] + 1000 * (int32) lo[1];\n"
    "        }\n"
    "    }\n"
    "}\n";

}  // namespace

TEST(SpirvByteLutTests, aTwoLaneLookupPadsToAWordAndRunsOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    Compiler compiler;
    auto module = compileForInspection(compiler, kTwoLaneSource, "test.LUT", "spvlut2");
    auto k = findMethod(module->getStructures()["test.LUT"], "lut");
    ASSERT_NE(k, nullptr);
    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_lut4_spirv_2", ctx);
    configureDeviceModule(dev, *tm);
    ASSERT_NE(lowerKernel(k, dev), nullptr);
    std::string failure;
    std::vector<uint8_t> spirv = emitSpirv(dev, *tm, &failure);
    ASSERT_FALSE(spirv.empty()) << "SPIR-V codegen failed: " << failure;
    if (!VulkanDriver::available()) GTEST_SKIP() << "no Vulkan compute device available";

    const uint32_t n = 1024;
    std::vector<int32_t> idx(n), out(n, 0);
    for (uint32_t i = 0; i < n; ++i) idx[i] = (int32_t) (i * 37u + 5u);
    VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    VulkanDriver::Buffer dOut = vk.alloc(n * sizeof(int32_t));
    VulkanDriver::Buffer dIdx = vk.alloc(n * sizeof(int32_t));
    VulkanDriver::Buffer dN = vk.alloc(sizeof(uint32_t));
    ASSERT_TRUE(vk.upload(dOut, out.data(), n * sizeof(int32_t)));
    ASSERT_TRUE(vk.upload(dIdx, idx.data(), n * sizeof(int32_t)));
    ASSERT_TRUE(vk.upload(dN, &n, sizeof(uint32_t)));
    const unsigned block = kVulkanLocalSizeX;
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "lut", {dOut, dIdx, dN},
                          (n + block - 1) / block));
    ASSERT_TRUE(vk.download(out.data(), dOut, n * sizeof(int32_t)));
    vk.free(dOut);
    vk.free(dIdx);
    vk.free(dN);
    std::size_t bad = 0;
    for (uint32_t i = 0; i < n; ++i) {
        int32_t want = kTable[idx[i] & 15] + 1000 * kTable[(idx[i] >> 8) & 15];
        if (out[i] != want) {
            if (bad < 5) ADD_FAILURE() << "i=" << i << " got " << out[i] << " want " << want;
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0u);
}
