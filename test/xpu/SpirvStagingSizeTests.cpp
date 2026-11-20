//
// SpirvStagingSizeTests — a kernel that declares its block as @Occupancy(maxWaves = N) gets N
// per-subgroup staging tiles, not one per subgroup of the default 1024-thread limit
// (gpu-kernel-parity 16.2.3: llm's MwTile and IdTile kernels carried 80 KB of staging on Vulkan).
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

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <string>

using cajeta_test::CajetaJit;
using cajeta::xpu::probe::compileForInspection;
using cajeta::xpu::probe::findMethod;

namespace {

// Four subgroups each edit their own f16 tile element by element (staged), multiply by I and
// store: y[w*256 + r*16 + c] = a[...] + (r*16 + c)/8 + w.
const char* kSource = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.Group;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class ST {
    @Kernel
    @Occupancy(maxWaves = 4)
    public static void st(KernelBuffer<float32> y, KernelBuffer<float16> a, KernelBuffer<float16> eye) {
        uint32 w = KernelThread.x() / (uint32) Group.width();
        CooperativeMatrix<float16,16,16,0> am;
        CooperativeMatrix<float16,16,16,1> bm;
        CooperativeMatrix<float32,16,16,2> acc;
        am.load(a, w * 256, 0, 16);
        bm.load(eye, 0, 0, 16);
        acc.splat(0.0f);
        uint32 n = am.elements();
        for (uint32 i = 0; i < n; i = i + 1) {
            uint32 r = am.row(i);
            uint32 c = am.col(i);
            am.set(i, (float16) ((float32) am.get(i) + (float32) (r * 16 + c) * 0.125f + (float32) w));
        }
        acc.mma(am, bm);
        acc.store(y, w * 256, 0, 16);
    }
    public static int32 run() {
        float16[] ha = heap float16[1024];
        float16[] he = heap float16[256];
        float32[] hy = heap float32[1024];
        for (int32 i = 0; i < 1024; i = i + 1) { ha[i] = (float16) (float32) (i % 7); hy[i] = -1.0f; }
        for (int32 i = 0; i < 256; i = i + 1) { he[i] = (i / 16 == i % 16) ? (float16) 1.0f : (float16) 0.0f; }
        KernelBuffer<float16> a = heap KernelBuffer<float16>(1024);
        KernelBuffer<float16> e = heap KernelBuffer<float16>(256);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(1024);
        a.upload(ha);
        e.upload(he);
        y.upload(hy);
        int32 wave = Group.laneBlockOf("st");
        uint32 block = (uint32) (4 * (wave > 0 ? wave : 32));
        KernelStream s #= KernelStream.current();
        st.launch(s, grid: [1], block: [block])(y, a, e);
        s.sync();
        y.download(hy);
        for (int32 i = 0; i < 1024; i = i + 1) {
            float32 want = (float32) (i % 7) + (float32) (i % 256) * 0.125f + (float32) (i / 256);
            if (hy[i] != want) { return 1000 + i; }
        }
        return 777;
    }
}
)CJ";

uint64_t stagingCells(llvm::Module& m, const std::string& name) {
    auto* gv = m.getGlobalVariable(name, true);
    if (!gv) return 0;
    return llvm::cast<llvm::ArrayType>(gv->getValueType())->getNumElements();
}

} // namespace

TEST(SpirvStagingSizeTests, aKernelOfFourWavesStagesFourTiles) {
    using namespace cajeta::xpu::vulkan;
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, kSource, "test.ST", "spvstagesize");
    auto k = findMethod(module->getStructures()["test.ST"], "st");
    ASSERT_NE(k, nullptr);
    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_stagesize", ctx);
    configureDeviceModule(dev, *tm);
    ASSERT_NE(lowerKernel(k, dev), nullptr);
    EXPECT_EQ(stagingCells(dev, "cajeta_cmstage_half"), 4u * 256u)
        << "one 16 x 16 staging tile per declared wave";
}

// 777: every subgroup's tile came back edited by its own subgroup index.
TEST(SpirvStagingSizeTests, fourWavesEachStageTheirOwnTileOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kSource, "test.ST", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run(), 777) << "1000 + i: the first wrong cell";
}
