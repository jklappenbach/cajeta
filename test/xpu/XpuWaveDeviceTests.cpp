//
// CajetaXPU wave ops (@Wave) — on a real GPU, end to end, across AMD and Vulkan.
//
// The on-device counterpart of XpuWaveEmitTests: the SAME Cajeta wave kernel
// (Wave.shuffleSync + Wave.ballotSync) compiles through each backend and runs.
//   out[t] = readlane(t*10+5, lane 3) + (u32) ballot(t < 4)
// Within the first wave (lanes 0..31, present at any wave width 32 or 64):
//   - lane 3 holds 3*10+5 = 35, so readlane broadcasts 35 to every lane, and
//   - ballot(t<4) sets the low 4 bits → 0xF = 15,
// so out[t] == 50 for t in [0,32). We verify exactly that first-wave window,
// which is wave-size-agnostic (it holds whether the hardware wave is 32 or 64).
//
// Skips cleanly when the respective device is absent.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/amd/HipDriver.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"

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
#include <vector>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

const char* kWaveSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Thread;\n"
    "import cajeta.xpu.core.Wave;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wavetest(Buffer<uint32> out) {\n"
    "        uint32 t = Thread.x();\n"
    "        uint32 r = Wave.shuffleSync(t * 10 + 5, 3);\n"
    "        uint64 b = Wave.ballotSync(t < 4);\n"
    "        out[t] = r + (uint32) b;\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_wavedev_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_wavedev_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = base / "test" / "M.cajeta";
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

constexpr uint32_t kExpected = 50;  // readlane(lane3)=35 + ballot(t<4)=0xF
constexpr unsigned kVerify = 32;    // verify the first wave window

// Wave reduce, width-agnostic. The input is a *divergent* buffer (host fills it
// with 1s) rather than a constant: a uniform-constant operand to AMDGPU's
// wave.reduce.add is folded back to the operand (returns 1) instead of being
// summed, so a real per-lane operand is needed to exercise the reduction. Over
// a fully-occupied wave, sum(1) == the wave width, so every lane sees the same
// value W ∈ {32, 64}. Dispatch block=64 (a multiple of both widths) so every
// wave/subgroup is fully occupied. We avoid Wave.width() here because LLVM 22's
// SPIR-V backend cannot select llvm.spv.wave.get.lane.count — a separate latent
// gap unrelated to reduce — so the check is "all lanes agree on a valid wave
// size" instead of comparing against width().
const char* kReduceSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Thread;\n"
    "import cajeta.xpu.core.Wave;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wavereduce(Buffer<uint32> in, Buffer<uint32> out) {\n"
    "        uint32 t = Thread.x();\n"
    "        out[t] = Wave.reduceSum(in[t]);\n"
    "    }\n"
    "}\n";

constexpr unsigned kReduceBlock = 64;  // multiple of 32 and 64 ⇒ full occupancy

// out[t] = Wave.laneId() (Inc 5C). Within the first wave (lanes 0..31, present
// at any wave width 32 or 64) lane t simply has index t, so out[t] == t — a
// wave-size-agnostic check.
const char* kLaneSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Thread;\n"
    "import cajeta.xpu.core.Wave;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wavelane(Buffer<uint32> out) {\n"
    "        uint32 t = Thread.x();\n"
    "        out[t] = Wave.laneId();\n"
    "    }\n"
    "}\n";

// reduceSum(1) over a full wave == wave width; the only real wave sizes are 32
// and 64, and every lane must agree (block is a multiple of both).
void expectUniformWaveWidth(const std::vector<uint32_t>& out) {
    ASSERT_FALSE(out.empty());
    uint32_t w = out[0];
    EXPECT_TRUE(w == 32u || w == 64u) << "reduceSum(1) = " << w
        << " is not a valid wave width (expected a genuine cross-lane sum)";
    for (size_t i = 0; i < out.size(); ++i)
        EXPECT_EQ(out[i], w) << "lane " << i << " disagrees on the wave sum";
}

} // namespace

TEST(XpuWaveDeviceTests, amdgpuShuffleBallotRunsOnDevice) {
    if (!cajeta::xpu::amd::HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kWaveSource);
    auto k = findMethod(module->getStructures()["test.M"], "wavetest");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::amd::createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_wave_amddevice", ctx);
    cajeta::xpu::amd::configureDeviceModule(dev, *tm);
    cajeta::xpu::amd::lowerKernel(k, dev);
    std::vector<uint8_t> hsaco = cajeta::xpu::amd::assembleHsaco(dev, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed (ld.lld present?)";

    cajeta::xpu::amd::HipDriver hip;
    ASSERT_TRUE(hip.init());
    auto mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    auto fn = hip.getFunction(mod, "wavetest");
    ASSERT_NE(fn, nullptr);

    const unsigned block = 32;  // one wave window (<= wavefront on gfx1151)
    auto dOut = hip.alloc(block * sizeof(uint32_t));
    ASSERT_NE(dOut, nullptr);
    void* params[] = { &dOut };
    ASSERT_TRUE(hip.launch(fn, /*grid=*/1, block, params));
    ASSERT_TRUE(hip.synchronize());

    std::vector<uint32_t> out(block, 0);
    ASSERT_TRUE(hip.memcpyDtoH(out.data(), dOut, block * sizeof(uint32_t)));
    hip.free(dOut);
    for (unsigned i = 0; i < kVerify; ++i)
        EXPECT_EQ(out[i], kExpected) << "lane " << i;
}

TEST(XpuWaveDeviceTests, vulkanShuffleBallotRunsOnDevice) {
    if (!cajeta::xpu::vulkan::VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kWaveSource);
    auto k = findMethod(module->getStructures()["test.M"], "wavetest");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_wave_vkdevice", ctx);
    cajeta::xpu::vulkan::configureDeviceModule(dev, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, dev);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(dev, *tm);
    ASSERT_FALSE(spirv.empty());

    // The workgroup size is baked (kVulkanLocalSizeX); size the buffer to it.
    const unsigned threads = cajeta::xpu::vulkan::kVulkanLocalSizeX;
    cajeta::xpu::vulkan::VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    auto dOut = vk.alloc(threads * sizeof(uint32_t));
    ASSERT_NE(dOut, 0u);
    std::vector<uint32_t> zero(threads, 0);
    ASSERT_TRUE(vk.upload(dOut, zero.data(), threads * sizeof(uint32_t)));
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "wavetest", {dOut},
                          /*groupCountX=*/1));

    std::vector<uint32_t> out(threads, 0);
    ASSERT_TRUE(vk.download(out.data(), dOut, threads * sizeof(uint32_t)));
    vk.free(dOut);
    for (unsigned i = 0; i < kVerify; ++i)
        EXPECT_EQ(out[i], kExpected) << "lane " << i;
}

TEST(XpuWaveDeviceTests, amdgpuReduceSumRunsOnDevice) {
    if (!cajeta::xpu::amd::HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kReduceSource);
    auto k = findMethod(module->getStructures()["test.M"], "wavereduce");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::amd::createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_reduce_amddevice", ctx);
    cajeta::xpu::amd::configureDeviceModule(dev, *tm);
    cajeta::xpu::amd::lowerKernel(k, dev);
    std::vector<uint8_t> hsaco = cajeta::xpu::amd::assembleHsaco(dev, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed (ld.lld present?)";

    cajeta::xpu::amd::HipDriver hip;
    ASSERT_TRUE(hip.init());
    auto mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    auto fn = hip.getFunction(mod, "wavereduce");
    ASSERT_NE(fn, nullptr);

    const unsigned block = kReduceBlock;
    const std::size_t bytes = block * sizeof(uint32_t);
    auto dIn = hip.alloc(bytes);
    auto dOut = hip.alloc(bytes);
    ASSERT_NE(dIn, nullptr);
    ASSERT_NE(dOut, nullptr);
    std::vector<uint32_t> ones(block, 1);
    ASSERT_TRUE(hip.memcpyHtoD(dIn, ones.data(), bytes));
    void* params[] = { &dIn, &dOut };
    ASSERT_TRUE(hip.launch(fn, /*grid=*/1, block, params));
    ASSERT_TRUE(hip.synchronize());

    std::vector<uint32_t> out(block, 0);
    ASSERT_TRUE(hip.memcpyDtoH(out.data(), dOut, bytes));
    hip.free(dIn);
    hip.free(dOut);
    expectUniformWaveWidth(out);
}

TEST(XpuWaveDeviceTests, vulkanReduceSumRunsOnDevice) {
    if (!cajeta::xpu::vulkan::VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kReduceSource);
    auto k = findMethod(module->getStructures()["test.M"], "wavereduce");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_reduce_vkdevice", ctx);
    cajeta::xpu::vulkan::configureDeviceModule(dev, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, dev);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(dev, *tm);
    ASSERT_FALSE(spirv.empty());

    // The baked workgroup size (kVulkanLocalSizeX) is a multiple of both wave
    // widths, so every subgroup is fully occupied and reduceSum(1) == width.
    const unsigned threads = cajeta::xpu::vulkan::kVulkanLocalSizeX;
    const std::size_t bytes = threads * sizeof(uint32_t);
    cajeta::xpu::vulkan::VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    auto dIn = vk.alloc(bytes);
    auto dOut = vk.alloc(bytes);
    ASSERT_NE(dIn, 0u);
    ASSERT_NE(dOut, 0u);
    std::vector<uint32_t> ones(threads, 1), zero(threads, 0);
    ASSERT_TRUE(vk.upload(dIn, ones.data(), bytes));
    ASSERT_TRUE(vk.upload(dOut, zero.data(), bytes));
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "wavereduce", {dIn, dOut},
                          /*groupCountX=*/1));

    std::vector<uint32_t> out(threads, 0);
    ASSERT_TRUE(vk.download(out.data(), dOut, bytes));
    vk.free(dIn);
    vk.free(dOut);
    expectUniformWaveWidth(out);
}

TEST(XpuWaveDeviceTests, amdgpuLaneIdRunsOnDevice) {
    if (!cajeta::xpu::amd::HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kLaneSource);
    auto k = findMethod(module->getStructures()["test.M"], "wavelane");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::amd::createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_lane_amddevice", ctx);
    cajeta::xpu::amd::configureDeviceModule(dev, *tm);
    cajeta::xpu::amd::lowerKernel(k, dev);
    std::vector<uint8_t> hsaco = cajeta::xpu::amd::assembleHsaco(dev, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed (ld.lld present?)";

    cajeta::xpu::amd::HipDriver hip;
    ASSERT_TRUE(hip.init());
    auto mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    auto fn = hip.getFunction(mod, "wavelane");
    ASSERT_NE(fn, nullptr);

    const unsigned block = 32;  // one wave window
    auto dOut = hip.alloc(block * sizeof(uint32_t));
    ASSERT_NE(dOut, nullptr);
    void* params[] = { &dOut };
    ASSERT_TRUE(hip.launch(fn, /*grid=*/1, block, params));
    ASSERT_TRUE(hip.synchronize());

    std::vector<uint32_t> out(block, 0);
    ASSERT_TRUE(hip.memcpyDtoH(out.data(), dOut, block * sizeof(uint32_t)));
    hip.free(dOut);
    for (unsigned i = 0; i < kVerify; ++i)
        EXPECT_EQ(out[i], i) << "lane " << i;
}

TEST(XpuWaveDeviceTests, vulkanLaneIdRunsOnDevice) {
    if (!cajeta::xpu::vulkan::VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kLaneSource);
    auto k = findMethod(module->getStructures()["test.M"], "wavelane");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_lane_vkdevice", ctx);
    cajeta::xpu::vulkan::configureDeviceModule(dev, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, dev);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(dev, *tm);
    ASSERT_FALSE(spirv.empty());

    const unsigned threads = cajeta::xpu::vulkan::kVulkanLocalSizeX;
    cajeta::xpu::vulkan::VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    auto dOut = vk.alloc(threads * sizeof(uint32_t));
    ASSERT_NE(dOut, 0u);
    std::vector<uint32_t> zero(threads, 0);
    ASSERT_TRUE(vk.upload(dOut, zero.data(), threads * sizeof(uint32_t)));
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "wavelane", {dOut},
                          /*groupCountX=*/1));

    std::vector<uint32_t> out(threads, 0);
    ASSERT_TRUE(vk.download(out.data(), dOut, threads * sizeof(uint32_t)));
    vk.free(dOut);
    for (unsigned i = 0; i < kVerify; ++i)
        EXPECT_EQ(out[i], i) << "lane " << i;
}
