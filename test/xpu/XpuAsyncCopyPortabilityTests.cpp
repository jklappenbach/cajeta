//
// CajetaXPU — AsyncCopy portability: NVPTX + SPIR-V lowering
// (xpu-pipelined-gemm-primitives Unit 5).
//
// GPU-free emit tests (PTX / SPIR-V text). The AMDGPU native path
// (global_load_lds, arch-gated) is covered by XpuAsyncCopyAmdDeviceTests; this
// file pins the OTHER backends:
//   - NVPTX (sm_89): AsyncCopy.copy/commit/wait lower to the native cp.async
//     family (cp.async + commit_group + wait_group) — the CUDA analog of CDNA's
//     global_load_lds (a direct global->shared async copy, no register round
//     trip). Drives U5.2's NVPTX seam; until then the default synchronous seam
//     emits ld.global/st.shared and this FAILS for the right reason.
//   - SPIR-V (vulkan1.3 compute): Vulkan compute has no global->shared async DMA
//     primitive, so AsyncCopy is the DOCUMENTED synchronous fallback — it must
//     still lower to valid SPIR-V and stay byte-portable.
//
// No GPU required: PTX and SPIR-V text are emitted GPU-free.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuKernelLowering.h"

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
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// Same async-staged kernel as XpuAsyncCopyAmdDeviceTests: stage in[0..256) into
// an LDS tile via AsyncCopy, commit + wait(0) to drain, barrier, cross-lane read.
const char* kAsyncStageSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Shared;\n"
    "import cajeta.xpu.AsyncCopy;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void asyncstage(KernelBuffer<uint32> out, KernelBuffer<uint32> in) {\n"
    "        Shared<uint32> tile = shared uint32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        AsyncCopy.copy(tile, 0, in, 0, 256);\n"
    "        AsyncCopy.commit();\n"
    "        AsyncCopy.wait(0);\n"
    "        Barrier.workgroup();\n"
    "        out[t] = tile[255 - t];\n"
    "    }\n"
    "}\n";

// U5.3 acceptance: a 16x16 f16 WMMA GEMM that stages A/B into Swizzled<float16,16>
// LDS tiles via AsyncCopy, then WMMA-loads the fragments from those swizzled tiles.
// Exercises all three primitives at once (async + swizzle + coop-matrix). It must
// COMPILE on every backend: AMD applies the native XOR swizzle + sync-fallback async;
// NVPTX gets native cp.async with the swizzle degraded to identity on the WMMA load
// (note: tier); SPIR-V/CPU use the sync/identity fallbacks.
const char* kSwizzledAsyncWmmaSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Swizzled;\n"
    "import cajeta.xpu.AsyncCopy;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wmma(KernelBuffer<float16> a, KernelBuffer<float16> b,\n"
    "                            KernelBuffer<float32> c) {\n"
    "        Swizzled<float16, 16> sa = shared float16[256];\n"
    "        Swizzled<float16, 16> sb = shared float16[256];\n"
    "        AsyncCopy.copy(sa, 0, a, 0, 256);\n"
    "        AsyncCopy.copy(sb, 0, b, 0, 256);\n"
    "        AsyncCopy.commit();\n"
    "        AsyncCopy.wait(0);\n"
    "        Barrier.workgroup();\n"
    "        CooperativeMatrix<float16, 16, 16, 0> ma;\n"
    "        ma.load(sa, 0, 0, 16);\n"
    "        CooperativeMatrix<float16, 16, 16, 1> mb;\n"
    "        mb.load(sb, 0, 0, 16);\n"
    "        CooperativeMatrix<float32, 16, 16, 2> mc;\n"
    "        mc.splat(0.0f);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler, const char* source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_asyncport_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_asyncport_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule((base / "test" / "M.cajeta").string(),
                                   base.string(), archive.string());
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

// U5.1 (NVPTX, GPU-free): AsyncCopy.copy/commit/wait lower to the native
// cp.async family on sm_89 — the CUDA analog of CDNA's global_load_lds. Until
// U5.2 adds the NVPTX seam, the default synchronous seam emits ld.global +
// st.shared and this FAILS for the right reason.
TEST(XpuAsyncCopyPortabilityTests, nvptxLowersAsyncCopyToCpAsync) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kAsyncStageSrc);
    auto method = findMethod(module->getStructures()["test.M"], "asyncstage");
    ASSERT_NE(method, nullptr);

    auto tm = cajeta::xpu::nvidia::createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_asyncport_nvptx", deviceCtx);
    cajeta::xpu::nvidia::configureDeviceModule(deviceModule, *tm);
    ASSERT_NE(cajeta::xpu::nvidia::lowerKernel(method, deviceModule), nullptr);

    std::string ptx = cajeta::xpu::nvidia::emitPtx(deviceModule, *tm);
    ASSERT_FALSE(ptx.empty()) << "NVPTX emit failed";

    EXPECT_NE(ptx.find("cp.async"), std::string::npos)
        << "expected the native cp.async copy on sm_89\n" << ptx;
    EXPECT_NE(ptx.find("cp.async.commit_group"), std::string::npos)
        << "expected cp.async.commit_group for AsyncCopy.commit()\n" << ptx;
    EXPECT_NE(ptx.find("cp.async.wait_group"), std::string::npos)
        << "expected cp.async.wait_group for AsyncCopy.wait(0)\n" << ptx;
}

// U5.1 (SPIR-V, GPU-free): Vulkan compute has no global->shared async DMA, so
// AsyncCopy is the DOCUMENTED synchronous fallback — it must still lower to
// valid SPIR-V and stay portable (no hard error, byte-identical staging).
TEST(XpuAsyncCopyPortabilityTests, spirvCompilesAsyncCopyViaSyncFallback) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kAsyncStageSrc);
    auto method = findMethod(module->getStructures()["test.M"], "asyncstage");
    ASSERT_NE(method, nullptr);

    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_asyncport_spirv", deviceCtx);
    cajeta::xpu::vulkan::configureDeviceModule(deviceModule, *tm);
    ASSERT_NE(cajeta::xpu::vulkan::lowerKernel(method, deviceModule), nullptr);

    std::string spv = cajeta::xpu::vulkan::emitSpirvText(deviceModule, *tm);
    EXPECT_FALSE(spv.empty())
        << "AsyncCopy must compile to valid SPIR-V via the sync fallback";
}

// U5.3 (acceptance, GPU-free): the swizzled + async f16 WMMA kernel COMPILES on
// every backend. AMD applies the native XOR swizzle + WMMA; NVPTX emits cp.async
// and degrades the WMMA-load swizzle to identity (note: tier — staging is also
// identity there, so it stays consistent); SPIR-V + CPU use the sync/identity
// fallbacks. Before U5.3 the NVPTX/SPIR-V coop-matrix load THREW on a swizzled
// tile; this pins that they now lower instead of rejecting.
TEST(XpuAsyncCopyPortabilityTests, swizzledAsyncWmmaCompilesOnAllBackends) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kSwizzledAsyncWmmaSrc);
    auto method = findMethod(module->getStructures()["test.M"], "wmma");
    ASSERT_NE(method, nullptr);

    // CPU (software tier): identity swizzle + synchronous async + software tile.
    {
        llvm::LLVMContext ctx;
        llvm::Module host("xpu_swzasync_cpu", ctx);
        EXPECT_NE(cajeta::xpu::cpu::lowerKernel(method, host), nullptr)
            << "swizzled+async WMMA must lower on CPU";
    }
    // AMDGPU (native XOR swizzle + WMMA; gfx1151 async = sync fallback).
    {
        auto tm = cajeta::xpu::amd::createAmdgpuTargetMachine("gfx1151");
        ASSERT_NE(tm, nullptr);
        llvm::LLVMContext ctx;
        llvm::Module dev("xpu_swzasync_amd", ctx);
        cajeta::xpu::amd::configureDeviceModule(dev, *tm);
        ASSERT_NE(cajeta::xpu::amd::lowerKernel(method, dev), nullptr);
        EXPECT_FALSE(cajeta::xpu::amd::emitIsa(dev, *tm).empty())
            << "swizzled+async WMMA must emit AMD ISA";
    }
    // NVPTX (cp.async + swizzle degraded to identity on the WMMA load).
    {
        auto tm = cajeta::xpu::nvidia::createNvptxTargetMachine("sm_89");
        ASSERT_NE(tm, nullptr);
        llvm::LLVMContext ctx;
        llvm::Module dev("xpu_swzasync_nvptx", ctx);
        cajeta::xpu::nvidia::configureDeviceModule(dev, *tm);
        ASSERT_NE(cajeta::xpu::nvidia::lowerKernel(method, dev), nullptr);
        std::string ptx = cajeta::xpu::nvidia::emitPtx(dev, *tm);
        EXPECT_FALSE(ptx.empty()) << "swizzled+async WMMA must emit PTX";
        EXPECT_NE(ptx.find("cp.async"), std::string::npos) << ptx;
    }
    // SPIR-V (sync async fallback + identity swizzle on the coop-matrix load).
    {
        auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
        ASSERT_NE(tm, nullptr);
        llvm::LLVMContext ctx;
        llvm::Module dev("xpu_swzasync_spirv", ctx);
        cajeta::xpu::vulkan::configureDeviceModule(dev, *tm);
        ASSERT_NE(cajeta::xpu::vulkan::lowerKernel(method, dev), nullptr);
        EXPECT_FALSE(cajeta::xpu::vulkan::emitSpirvText(dev, *tm).empty())
            << "swizzled+async WMMA must emit SPIR-V";
    }
}
