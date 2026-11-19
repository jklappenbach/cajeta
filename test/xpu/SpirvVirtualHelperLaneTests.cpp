//
// SpirvVirtualHelperLaneTests — a @Device helper that reads the wave lane, called from a kernel
// that takes slot fission on a virtual wave, must not keep the erased slot function's argument
// (gpu-kernel-parity 18.3.3: the v0.54.0 heap corruption in llm's Vulkan build).
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
#include "cajeta/error/Exception.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

using cajeta_test::CajetaJit;
using cajeta::xpu::probe::compileForInspection;
using cajeta::xpu::probe::findMethod;

namespace {

// out[g] = lane * 2 + in[g] after a workgroup barrier; the lane comes from a helper.
const char* kSource = R"CJ(
package test;
import cajeta.xpu.Barrier;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Wave;
public class H {
    @Device
    static uint32 twiceLane(uint32 bias) {
        return Wave.laneId() * 2 + bias;
    }
    @Kernel
    @Wave(width = 32)
    public static void hl(KernelBuffer<uint32> out, KernelBuffer<uint32> in) {
        uint32 g = KernelThread.globalIdX();
        uint32 v = in[g];
        Barrier.workgroup();
        out[g] = H.twiceLane(v);
    }
    public static int32 run() {
        uint32[] h = heap uint32[64];
        uint32[] r = heap uint32[64];
        for (int32 i = 0; i < 64; i = i + 1) { h[i] = (uint32) (i * 5); r[i] = 0; }
        KernelBuffer<uint32> x = heap KernelBuffer<uint32>(64);
        KernelBuffer<uint32> y = heap KernelBuffer<uint32>(64);
        x.upload(h);
        y.upload(r);
        KernelStream s #= KernelStream.current();
        hl.launch(s, grid: [2], block: [32])(y, x);
        s.sync();
        y.download(r);
        for (int32 i = 0; i < 64; i = i + 1) {
            if (r[i] != (uint32) ((i % 32) * 2 + i * 5)) { return 1000 + i; }
        }
        return 777;
    }
}
)CJ";

} // namespace

// The verifier reports a value used outside its own function; the module must verify whether the
// virtual lowering succeeds or refuses.
TEST(SpirvVirtualHelperLaneTests, aVirtualWaveKeepsNoReferenceIntoTheErasedSlotFunction) {
    using namespace cajeta::xpu::vulkan;
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, kSource, "test.H", "spvvhelper");
    auto k = findMethod(module->getStructures()["test.H"], "hl");
    ASSERT_NE(k, nullptr);
    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_vhelper", ctx);
    configureDeviceModule(dev, *tm);
    std::string refused;
    try {
        lowerVirtualKernel(k, dev, *tm, "hl$v8", 32, 8);
    } catch (cajeta::Exception& e) {
        refused = e.getMessage();
    }
    std::string errs;
    llvm::raw_string_ostream os(errs);
    EXPECT_FALSE(llvm::verifyModule(dev, &os)) << errs << "\n(refused: " << refused << ")";
}

// 777: the kernel ran (natively, or on its virtual variant on a narrow device) and matched.
TEST(SpirvVirtualHelperLaneTests, aHelperReadingTheLaneMatchesTheHostOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kSource, "test.H", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run(), 777) << "1000 + i: the first wrong cell";
}
