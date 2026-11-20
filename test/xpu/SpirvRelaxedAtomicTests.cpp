//
// SpirvRelaxedAtomicTests — a Relaxed kernel atomic reaches Vulkan as memory semantics 0, and as
// AcquireRelease under CAJETA_XPU_VK_RELAXED=0 (gpu-kernel-parity 16.2.4.3.2).
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "../PortableEnv.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <cstdlib>
#include <cstring>
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
import cajeta.xpu.MemoryOrder;
public class R {
    @Kernel
    public static void count(KernelBuffer<int32> c, KernelBuffer<int32> flag) {
        c.atomicAdd(0, 1, MemoryOrder.Relaxed);
        flag.atomicExchange(KernelThread.globalIdX(), 1, MemoryOrder.Release);
    }
    public static int32 run() {
        KernelBuffer<int32> c = heap KernelBuffer<int32>(1);
        KernelBuffer<int32> f = heap KernelBuffer<int32>(256);
        int32[] h = heap int32[1];
        h[0] = 0;
        c.upload(h);
        KernelStream s #= KernelStream.current();
        count.launch(s, grid: [1], block: [256])(c, f);
        s.sync();
        c.download(h);
        return h[0] == 256 ? 777 : h[0];
    }
}
)CJ";

// The default for one scope (`on`), or CAJETA_XPU_VK_RELAXED=0.
struct Relaxed {
    explicit Relaxed(bool on) { if (on) unsetenv("CAJETA_XPU_VK_RELAXED"); else setenv("CAJETA_XPU_VK_RELAXED", "0", 1); }
    ~Relaxed() { unsetenv("CAJETA_XPU_VK_RELAXED"); }
};

// The value of each atomic's memory-semantics constant in kernel `count`'s SPIR-V, in order.
std::vector<uint32_t> atomicSemantics(std::string& failure) {
    using namespace cajeta::xpu::vulkan;
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, kSource, "test.R", "spvrelaxed");
    auto k = findMethod(module->getStructures()["test.R"], "count");
    if (!k) { failure = "no kernel"; return {}; }
    auto tm = createSpirvTargetMachine("vulkan1.3");
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_relaxed", ctx);
    configureDeviceModule(dev, *tm);
    if (!lowerKernel(k, dev)) { failure = "lowering failed"; return {}; }
    std::vector<uint8_t> bytes = emitSpirv(dev, *tm, &failure);
    std::vector<uint32_t> w(bytes.size() / 4);
    if (!bytes.empty()) std::memcpy(w.data(), bytes.data(), bytes.size());
    std::vector<uint32_t> consts(w.size() > 3 ? w[3] : 0, ~0u), sems;
    for (size_t i = 5; i < w.size();) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (wc == 0) break;
        if (op == 43 && wc == 4 && w[i + 2] < consts.size()) consts[w[i + 2]] = w[i + 3];
        if ((op == 229 || op == 234) && wc >= 6) sems.push_back(consts[w[i + 5]]);
        i += wc;
    }
    return sems;
}

} // namespace

TEST(SpirvRelaxedAtomicTests, aRelaxedAtomicIsSemanticsZero) {
    Relaxed on(true);
    std::string failure;
    auto sems = atomicSemantics(failure);
    ASSERT_EQ(sems.size(), 2u) << failure;
    EXPECT_EQ(sems[0], 0u) << "the Relaxed atomicAdd";
    EXPECT_EQ(sems[1] & 0x4u, 0x4u) << "the Release exchange keeps its order";
}

TEST(SpirvRelaxedAtomicTests, aRelaxedAtomicIsAcquireReleaseWhenTheSwitchIsOff) {
    Relaxed off(false);
    std::string failure;
    auto sems = atomicSemantics(failure);
    ASSERT_EQ(sems.size(), 2u) << failure;
    EXPECT_EQ(sems[0], 0x48u) << "UniformMemory | AcquireRelease";
}

TEST(SpirvRelaxedAtomicTests, relaxedAtomicsCountOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    Relaxed on(true);
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kSource, "test.R", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run(), 777) << "the count after 256 relaxed adds";
}
