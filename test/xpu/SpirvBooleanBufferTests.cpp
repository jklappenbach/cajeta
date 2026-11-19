//
// SpirvBooleanBufferTests — a kernel that stores booleans to a buffer emits valid
// SPIR-V and matches the host (gpu-kernel-parity 11.1.1).
//
// SPIR-V forbids OpTypeBool in an externally visible variable, so a
// KernelBuffer<boolean> must reach the module as bytes. dev.cajeta.ml.kernels'
// `arith` at boolean (arith_boolean) failed spirv-val on 2026-11-18.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"

#include <fstream>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;
using cajeta::xpu::probe::compileForInspection;
using cajeta::xpu::probe::findMethod;

namespace {

const char* kSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class B {
    @Kernel
    public static void lt(KernelBuffer<boolean> out, KernelBuffer<boolean> inv, KernelBuffer<int32> a, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) {
            boolean v = a[i] < 7;
            out[i] = v;
            inv[i] = !inv[i];
        }
    }
    public static int32 run() {
        uint32 n = 64;
        int32[] ha = heap int32[n];
        boolean[] ho = heap boolean[n];
        boolean[] hi = heap boolean[n];
        for (uint32 i = 0; i < n; i = i + 1) { ha[i] = (int32) (i % 13); ho[i] = false; hi[i] = (i % 3) == 0; }
        KernelBuffer<int32> a = heap KernelBuffer<int32>(n);
        KernelBuffer<boolean> out = heap KernelBuffer<boolean>(n);
        KernelBuffer<boolean> inv = heap KernelBuffer<boolean>(n);
        a.upload(ha);
        out.upload(ho);
        inv.upload(hi);
        KernelStream s #= KernelStream.current();
        lt.launch(s, grid: [1], block: [64])(out, inv, a, n);
        s.sync();
        out.download(ho);
        inv.download(hi);
        for (uint32 i = 0; i < n; i = i + 1) {
            if (ho[i] != ((int32) (i % 13) < 7)) { return (int32) (100 + i); }
            if (hi[i] != !((i % 3) == 0)) { return (int32) (200 + i); }
        }
        return 777;
    }
}
)CJ";

std::optional<bool> spirvVal(const std::vector<uint8_t>& spirv) {
    auto tool = llvm::sys::findProgramByName("spirv-val");
    if (!tool) return std::nullopt;
    llvm::SmallString<128> path;
    if (llvm::sys::fs::createTemporaryFile("cajeta-bool", "spv", path)) return std::nullopt;
    { std::ofstream f(path.c_str(), std::ios::binary);
      f.write(reinterpret_cast<const char*>(spirv.data()), (std::streamsize) spirv.size()); }
    std::string p = path.str().str();
    llvm::StringRef args[] = {*tool, "--target-env", "vulkan1.3", p};
    int rc = llvm::sys::ExecuteAndWait(*tool, args);
    llvm::sys::fs::remove(path);
    return rc == 0;
}

} // namespace

TEST(SpirvBooleanBufferTests, aBooleanBufferStoreIsValidSpirv) {
    using namespace cajeta::xpu::vulkan;
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, kSource, "test.B", "spvbool");
    auto k = findMethod(module->getStructures()["test.B"], "lt");
    ASSERT_NE(k, nullptr);
    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_bool", ctx);
    configureDeviceModule(dev, *tm);
    ASSERT_NE(lowerKernel(k, dev), nullptr);
    std::string failure;
    std::vector<uint8_t> spirv = emitSpirv(dev, *tm, &failure);
    ASSERT_FALSE(spirv.empty()) << failure;
    auto valid = spirvVal(spirv);
    if (!valid) GTEST_SKIP() << "spirv-val not installed";
    EXPECT_TRUE(*valid) << "spirv-val rejected a boolean buffer store";
}

TEST(SpirvBooleanBufferTests, booleanBuffersMatchTheHostOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kSource, "test.B", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run(), 777) << "100+i: a comparison stored wrong; 200+i: a read-modify-write";
}
