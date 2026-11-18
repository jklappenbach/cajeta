//
// SpirvPortableTileTests — a portable cooperative-matrix tile on vulkan is spread
// across the subgroup, not copied whole into each work-item (gpu-kernel-parity 10.1).
//
// RADV advertises no bf16 matrix config, so a bf16 tile takes the portable tile.
// Replicated, every work-item held the whole 16x16 tile in its own frame: the
// bf16 GEMM took 15 minutes to compile and ran slowly (cajeta-ml-gpu 23.2.6).
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

#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <string>

using cajeta_test::CajetaJit;
using cajeta::xpu::probe::compileForInspection;
using cajeta::xpu::probe::findMethod;

namespace {

// C = A * B at bf16 with a float32 accumulator, A[i][k] = (i + 2k) % 7 - 3 and
// B[k][j] = (3k + j) % 5 - 2: small integers, so every product and sum is exact.
const char* kSource = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
public class P {
    @Kernel
    public static void bf(KernelBuffer<float32> c, KernelBuffer<bfloat16> a, KernelBuffer<bfloat16> b) {
        CooperativeMatrix<bfloat16,16,16,0> ma;
        CooperativeMatrix<bfloat16,16,16,1> mb;
        CooperativeMatrix<float32,16,16,2> mc;
        ma.load(a, 0, 0, 16);
        mb.load(b, 0, 0, 16);
        mc.splat(0.0f);
        mc.mma(ma, mb);
        mc.store(c, 0, 0, 16);
    }
    public static int32 run() {
        bfloat16[] ha = heap bfloat16[256];
        bfloat16[] hb = heap bfloat16[256];
        float32[] hc = heap float32[256];
        for (int32 i = 0; i < 16; i = i + 1) {
            for (int32 k = 0; k < 16; k = k + 1) {
                ha[i * 16 + k] = (bfloat16) (float32) ((i + 2 * k) % 7 - 3);
                hb[i * 16 + k] = (bfloat16) (float32) ((3 * i + k) % 5 - 2);
            }
        }
        KernelBuffer<bfloat16> a = heap KernelBuffer<bfloat16>(256);
        KernelBuffer<bfloat16> b = heap KernelBuffer<bfloat16>(256);
        KernelBuffer<float32> c = heap KernelBuffer<float32>(256);
        a.upload(ha);
        b.upload(hb);
        KernelStream s #= KernelStream.current();
        bf.launch(s, grid: [1], block: [32])(c, a, b);
        s.sync();
        c.download(hc);
        for (int32 i = 0; i < 16; i = i + 1) {
            for (int32 j = 0; j < 16; j = j + 1) {
                int32 want = 0;
                for (int32 k = 0; k < 16; k = k + 1) { want = want + ((i + 2 * k) % 7 - 3) * ((3 * k + j) % 5 - 2); }
                if (hc[i * 16 + j] != (float32) want) { return 1000 + i * 16 + j; }
            }
        }
        return 777;
    }
}
)CJ";

} // namespace

// No work-item holds a whole 256-element tile: the tile is spread across the subgroup.
TEST(SpirvPortableTileTests, aBf16TileIsSpreadAcrossTheSubgroupOnVulkan) {
    using namespace cajeta::xpu::vulkan;
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, kSource, "test.P", "spvportable");
    auto k = findMethod(module->getStructures()["test.P"], "bf");
    ASSERT_NE(k, nullptr);
    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_portable", ctx);
    configureDeviceModule(dev, *tm);
    ASSERT_NE(lowerKernel(k, dev), nullptr);
    for (auto& f : dev)
        for (auto& bb : f)
            for (auto& i : bb)
                if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(&i))
                    if (auto* at = llvm::dyn_cast<llvm::ArrayType>(a->getAllocatedType()))
                        EXPECT_LT(at->getNumElements(), 256u)
                            << "a work-item holds a whole tile in " << f.getName().str();
    std::string failure;
    EXPECT_FALSE(emitSpirv(dev, *tm, &failure).empty()) << failure;
}

TEST(SpirvPortableTileTests, aBf16TileMatchesTheHostOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kSource, "test.P", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run(), 777) << "1000 + r*16 + c: the first wrong cell";
}
