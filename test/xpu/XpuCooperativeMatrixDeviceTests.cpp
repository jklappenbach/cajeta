//
// CooperativeMatrix<T,Rows,Cols,Use> (cajeta-gpu CM5) — the SPV_KHR_cooperative_
// matrix tile-matmul primitive, end to end on a REAL device. The on-device
// counterpart of the CM4/CM5b emit tests: the SAME cajeta @Kernel that loads an
// A tile (use 0) and a B tile (use 1), zeroes an accumulator C (use 2), issues
// one OpCooperativeMatrixMulAddKHR, and stores C — compiles through the Vulkan/
// SPIR-V backend and DISPATCHES on the RADV STRIX_HALO WMMA cores.
//
// MIXED PRECISION (the only float config the hardware exposes, and the SPELA/
// Prism regime): A and B are float16 (IEEE half), the accumulator/result is
// float32. The device lowerer keys each tile off its declared element type, so
// f16-in / f32-accumulate flows through unchanged (CM5a made cajeta's float16 an
// IEEE half so it matches VK_COMPONENT_TYPE_FLOAT16_KHR).
//
// EXACT check: the inputs are small integers that are exactly representable in
// half, so every product and the 16-term f32 accumulation are exact — the device
// tile must equal the CPU reference to the bit. (No fp tolerance; like the wave
// test's exact out[i]==50.)
//
// Gated on VulkanDriver::coopMatrixAvailable() — which checks the device actually
// lists the 16x16x16 Subgroup f16/f16->f32 config — so it SKIPs cleanly on a box
// without WMMA (or without the cooperative-matrix extension at all).
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
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::xpu::vulkan::VulkanDriver;

namespace {

// One 16x16 tile per operand: A (16x16 half), B (16x16 half), C (16x16 float).
constexpr unsigned N = 16;
constexpr unsigned TILE = N * N;  // 256 elements

// A single-tile matmul C = A * B through the cooperative-matrix verbs. No
// per-thread indexing: load/mma/store are subgroup-collective, so the whole
// subgroup cooperates on one tile (the dispatch's 64 threads = full subgroups).
const char* kMatmulSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void matmul(Buffer<float16> a, Buffer<float16> b,\n"
    "                              Buffer<float32> c) {\n"
    "        CooperativeMatrix<float16,16,16,0> ma;\n"
    "        ma.load(a, 0, 16);\n"           // layout 0 = row-major, stride 16
    "        CooperativeMatrix<float16,16,16,1> mb;\n"
    "        mb.load(b, 0, 16);\n"
    "        CooperativeMatrix<float32,16,16,2> mc;\n"
    "        mc.splat(0.0f);\n"              // zero accumulator
    "        mc.mma(ma, mb);\n"             // C = A*B + C  (one tile FMA)
    "        mc.store(c, 0, 16);\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_coopmatdev_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_coopmatdev_arch_" + std::to_string(rng()));
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

} // namespace

TEST(XpuCooperativeMatrixDeviceTests, mixedPrecisionMatmulOnDevice) {
    if (!VulkanDriver::coopMatrixAvailable()) {
        GTEST_SKIP() << "no Vulkan device with a 16x16x16 f16/f16->f32 "
                        "cooperative-matrix config";
    }

    Compiler compiler;
    auto module = compileForInspection(compiler, kMatmulSource);
    auto k = findMethod(module->getStructures()["test.M"], "matmul");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_coopmat_vkdevice", ctx);
    cajeta::xpu::vulkan::configureDeviceModule(dev, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, dev);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(dev, *tm);
    ASSERT_FALSE(spirv.empty());

    // Row-major tiles. Inputs are small integers exactly representable in half
    // (A in 0..4, B in 0..3): products <= 12, the 16-term sum <= 192 — all exact
    // in both half and float, so the device tile equals the integer reference.
    std::vector<_Float16> hostA(TILE), hostB(TILE);
    std::vector<float> ref(TILE, 0.0f);
    for (unsigned i = 0; i < N; ++i)
        for (unsigned kk = 0; kk < N; ++kk)
            hostA[i * N + kk] = (_Float16) ((i + 2 * kk) % 5);
    for (unsigned kk = 0; kk < N; ++kk)
        for (unsigned j = 0; j < N; ++j)
            hostB[kk * N + j] = (_Float16) ((3 * kk + j) % 4);
    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < N; ++j) {
            float acc = 0.0f;
            for (unsigned kk = 0; kk < N; ++kk)
                acc += (float) hostA[i * N + kk] * (float) hostB[kk * N + j];
            ref[i * N + j] = acc;
        }

    VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    auto dA = vk.alloc(TILE * sizeof(_Float16));
    auto dB = vk.alloc(TILE * sizeof(_Float16));
    auto dC = vk.alloc(TILE * sizeof(float));
    ASSERT_NE(dA, 0u);
    ASSERT_NE(dB, 0u);
    ASSERT_NE(dC, 0u);
    ASSERT_TRUE(vk.upload(dA, hostA.data(), TILE * sizeof(_Float16)));
    ASSERT_TRUE(vk.upload(dB, hostB.data(), TILE * sizeof(_Float16)));
    std::vector<float> zero(TILE, -1.0f);
    ASSERT_TRUE(vk.upload(dC, zero.data(), TILE * sizeof(float)));

    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "matmul", {dA, dB, dC},
                          /*groupCountX=*/1));

    std::vector<float> out(TILE, -2.0f);
    ASSERT_TRUE(vk.download(out.data(), dC, TILE * sizeof(float)));
    vk.free(dA);
    vk.free(dB);
    vk.free(dC);

    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < N; ++j)
            EXPECT_EQ(out[i * N + j], ref[i * N + j])
                << "tile mismatch at (" << i << "," << j << ")";
}
