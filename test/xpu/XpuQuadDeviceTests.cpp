//
// CajetaXPU quad (2x2) ops — on a real GPU, end to end, across AMD and Vulkan.
//
// The on-device counterpart of XpuQuadEmitTests: the SAME Cajeta quad kernel
// runs on each backend. Vulkan lowers to the native quad opcodes
// (OpGroupNonUniformQuadBroadcast / QuadSwap, and OpGroupNonUniformQuad{All,Any}
// KHR from SPV_KHR_quad_control); AMD takes the portable default built on the
// wave shuffle/ballot seams. Because a quad is four consecutive lanes and both
// backends use the same `laneId & ~3` base, the two paths must agree bit-for-bit
// — this test is the cross-check (the rotate / prefix-scan lesson).
//
// All results are quad-local and wave-width-agnostic (they depend only on
// `t & 3`), so the SAME expectation holds whether the hardware wave is 32 or 64
// and for any block that is a multiple of 4 with full occupancy:
//   broadcast(t,0) = t & ~3      broadcast(t,2) = (t & ~3) + 2
//   swapHorizontal = t ^ 1       swapVertical   = t ^ 2     swapDiagonal = t ^ 3
//   all((t&3)<2)   = false (mixed quad)   any((t&3)<2) = true
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

// Seven regions of n threads each, exercising every quad op. The predicate
// (t & 3) < 2 is true for quad lanes 0,1 and false for 2,3 — a mixed quad, so
// all=false / any=true. (`&` binds looser than `<`, hence the parentheses.)
const char* kQuadSource =
    "package test;\n"
    "import cajeta.gpu.KernelBuffer;\n"
    "import cajeta.gpu.KernelThread;\n"
    "import cajeta.gpu.Quad;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void quadops(KernelBuffer<uint32> out, uint32 n) {\n"
    "        uint32 t = KernelThread.x();\n"
    "        out[t] = Quad.broadcast(t, 0);\n"
    "        out[n + t] = Quad.broadcast(t, 2);\n"
    "        out[2 * n + t] = Quad.swapHorizontal(t);\n"
    "        out[3 * n + t] = Quad.swapVertical(t);\n"
    "        out[4 * n + t] = Quad.swapDiagonal(t);\n"
    "        boolean p = (t & 3) < 2;\n"
    "        uint32 av = 0;\n"
    "        if (Quad.all(p)) {\n"
    "            av = 1;\n"
    "        }\n"
    "        out[5 * n + t] = av;\n"
    "        uint32 yv = 0;\n"
    "        if (Quad.any(p)) {\n"
    "            yv = 1;\n"
    "        }\n"
    "        out[6 * n + t] = yv;\n"
    "    }\n"
    "}\n";

constexpr unsigned kQuadRegions = 7;
constexpr unsigned kQuadBlock = 64;  // multiple of 4, full occupancy at 32 or 64

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_quaddev_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_quaddev_arch_" + std::to_string(rng()));
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

void expectQuad(const std::vector<uint32_t>& out, unsigned n) {
    for (unsigned t = 0; t < n; ++t) {
        EXPECT_EQ(out[0 * n + t], t & ~3u)        << "broadcast0 lane " << t;
        EXPECT_EQ(out[1 * n + t], (t & ~3u) + 2u) << "broadcast2 lane " << t;
        EXPECT_EQ(out[2 * n + t], t ^ 1u)         << "swapHorizontal lane " << t;
        EXPECT_EQ(out[3 * n + t], t ^ 2u)         << "swapVertical lane " << t;
        EXPECT_EQ(out[4 * n + t], t ^ 3u)         << "swapDiagonal lane " << t;
        EXPECT_EQ(out[5 * n + t], 0u)             << "quadAll(mixed) lane " << t;
        EXPECT_EQ(out[6 * n + t], 1u)             << "quadAny(mixed) lane " << t;
    }
}

} // namespace

TEST(XpuQuadDeviceTests, amdgpuQuadOpsRunOnDevice) {
    if (!cajeta::xpu::amd::HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kQuadSource);
    auto k = findMethod(module->getStructures()["test.M"], "quadops");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::amd::createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_quad_amddevice", ctx);
    cajeta::xpu::amd::configureDeviceModule(dev, *tm);
    cajeta::xpu::amd::lowerKernel(k, dev);
    std::vector<uint8_t> hsaco = cajeta::xpu::amd::assembleHsaco(dev, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed (ld.lld present?)";

    cajeta::xpu::amd::HipDriver hip;
    ASSERT_TRUE(hip.init());
    auto mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    auto fn = hip.getFunction(mod, "quadops");
    ASSERT_NE(fn, nullptr);

    const unsigned n = kQuadBlock;  // threads per region
    auto dOut = hip.alloc(kQuadRegions * n * sizeof(uint32_t));
    ASSERT_NE(dOut, nullptr);
    void* params[] = { &dOut, (void*) &n };
    ASSERT_TRUE(hip.launch(fn, /*grid=*/1, kQuadBlock, params));
    ASSERT_TRUE(hip.synchronize());

    std::vector<uint32_t> out(kQuadRegions * n, 0);
    ASSERT_TRUE(hip.memcpyDtoH(out.data(), dOut,
                               kQuadRegions * n * sizeof(uint32_t)));
    hip.free(dOut);
    expectQuad(out, n);
}

TEST(XpuQuadDeviceTests, vulkanQuadOpsRunOnDevice) {
    if (!cajeta::xpu::vulkan::VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kQuadSource);
    auto k = findMethod(module->getStructures()["test.M"], "quadops");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_quad_vkdevice", ctx);
    cajeta::xpu::vulkan::configureDeviceModule(dev, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, dev);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(dev, *tm);
    ASSERT_FALSE(spirv.empty());

    const unsigned n = cajeta::xpu::vulkan::kVulkanLocalSizeX;  // threads/region
    cajeta::xpu::vulkan::VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    auto dOut = vk.alloc(kQuadRegions * n * sizeof(uint32_t));
    auto dN = vk.alloc(sizeof(uint32_t));
    ASSERT_NE(dOut, 0u); ASSERT_NE(dN, 0u);
    std::vector<uint32_t> zero(kQuadRegions * n, 0);
    ASSERT_TRUE(vk.upload(dOut, zero.data(), kQuadRegions * n * sizeof(uint32_t)));
    ASSERT_TRUE(vk.upload(dN, &n, sizeof(uint32_t)));
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "quadops",
                          {dOut, dN}, /*groupCountX=*/1));

    std::vector<uint32_t> out(kQuadRegions * n, 0);
    ASSERT_TRUE(vk.download(out.data(), dOut,
                            kQuadRegions * n * sizeof(uint32_t)));
    vk.free(dOut); vk.free(dN);
    expectQuad(out, n);
}
