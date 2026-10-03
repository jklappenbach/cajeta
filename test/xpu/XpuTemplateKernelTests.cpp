//
// A @Kernel inside a class template binds the class's non-type parameters
// (xpu-tile-shape-selection plan Unit 1, spec §2 and §7.2).
//
// A tile written once over its shape, `class Tile<uint32 TM>`, is lowered once
// per instantiation, and its body reads TM as a compile-time constant. Measured
// 2026-10-02/03 before this unit: the lowering failed with "unbound identifier
// 'TM'", and two instantiations of one kernel were refused at registration
// with CAJETA_ERROR_XPU_KERNEL_NAME_COLLISION, because a kernel registered and
// launched under its bare method name. An instantiation's kernel now registers
// as `Tile_4_fill` (the runtime also uses the registered name as the device
// entry symbol, and PTX takes no '<' or '.'); a kernel of an ordinary class
// keeps its bare name.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "XpuDeviceTestUtil.h"
#include "KernelLoweringProbe.h"

#include "cajeta/compile/CajetaModule.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/core/KernelManifest.h"
#include "cajeta/xpu/nvidia/NvptxRegistration.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <map>
#include <set>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

// Two kernels over TM: `fill` reads it as a value, `staged` sizes a Shared
// tile with it. The instance methods launch them, so each launch resolves in
// its own instantiation.
const char* kSource = R"CJ(
package test;
import cajeta.xpu.Barrier;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Shared;
public class Tile<uint32 TM> {
    public Tile() { return; }

    @Kernel
    public static void fill(KernelBuffer<uint32> out) {
        uint32 i = KernelThread.globalIdX();
        out[i] = i * TM;
    }

    @Kernel
    public static void staged(KernelBuffer<uint32> out) {
        Shared<uint32> s = shared uint32[TM * 8];
        uint32 t = KernelThread.x();
        if (t < TM * 8) { s[t] = t + TM; }
        Barrier.workgroup();
        if (t < TM * 8) { out[t] = s[TM * 8 - 1 - t]; }
    }

    public uint32 fillAt(uint32 k) {
        KernelBuffer<uint32> out = heap KernelBuffer<uint32>(0, 64);
        out.allocate();
        KernelStream s #= KernelStream.current();
        fill.launch(s, grid: [1], block: [64])(out);
        s.sync();
        uint32[] h = heap uint32[64];
        out.download(h);
        out.free();
        return h[k];
    }

    public uint32 stagedAt(uint32 k) {
        KernelBuffer<uint32> out = heap KernelBuffer<uint32>(0, 64);
        out.allocate();
        KernelStream s #= KernelStream.current();
        staged.launch(s, grid: [1], block: [64])(out);
        s.sync();
        uint32[] h = heap uint32[64];
        out.download(h);
        out.free();
        return h[k];
    }
}
public class M {
    public static int32 run() {
        Tile<4> a #= heap Tile<4>();
        Tile<7> b #= heap Tile<7>();
        return (int32) (a.fillAt(3) * 100 + b.fillAt(3));
    }
    public static int32 runStaged() {
        Tile<4> a #= heap Tile<4>();
        Tile<7> b #= heap Tile<7>();
        return (int32) (a.stagedAt(0) * 100 + b.stagedAt(0));
    }
}
)CJ";

// fill: out[3] = 3 * TM, so 12 and 21.
constexpr int kFillWant = 12 * 100 + 21;
// staged: out[0] = s[TM*8 - 1] = TM*8 - 1 + TM, so 35 and 62.
constexpr int kStagedWant = 35 * 100 + 62;

int runEntry(cajeta::xpu::Backend backend, const char* entry) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    CajetaJit::Options o;
    o.xpuBackends = {backend};
    auto jit = CajetaJit::compile(kSource, "test.M", o);
    EXPECT_NE(jit, nullptr);
    if (!jit) return -2;
    auto fn = jit->lookup<int (*)()>(entry);
    EXPECT_NE(fn, nullptr);
    return fn ? fn() : -2;
}

// Every kernel of every instantiation of test.Tile, by its owner's canonical
// name.
std::vector<cajeta::MethodPtr> tileKernels(const cajeta::CajetaModulePtr& module,
                                           const std::string& name) {
    std::vector<cajeta::MethodPtr> out;
    for (auto& [canon, klass] : module->getStructures()) {
        if (canon.rfind("test.Tile<", 0) != 0 || !klass) continue;
        for (auto& [k, m] : klass->getMethods())
            if (m && m->getName() == name) out.push_back(m);
    }
    return out;
}

} // namespace

// 4.1.1.1, cpu.
TEST(XpuTemplateKernel, aKernelReadsItsClassTemplateParameterOnCpu) {
    EXPECT_EQ(runEntry(cajeta::xpu::Backend::Cpu, "run"), kFillWant)
        << "Tile<4>.fill and Tile<7>.fill must each read their own TM";
}

// 4.1.1.1, nvptx.
TEST(XpuTemplateKernel, aKernelReadsItsClassTemplateParameterOnNvptx) {
    CAJETA_SKIP_IF_NO_CUDA();
    EXPECT_EQ(runEntry(cajeta::xpu::Backend::Nvptx, "run"), kFillWant);
}

// 4.1.1.2: a Shared tile sized by TM is static, and sized per instantiation.
TEST(XpuTemplateKernel, aSharedTileSizedByTheParameterRunsOnCpu) {
    EXPECT_EQ(runEntry(cajeta::xpu::Backend::Cpu, "runStaged"), kStagedWant);
}

TEST(XpuTemplateKernel, aSharedTileSizedByTheParameterRunsOnNvptx) {
    CAJETA_SKIP_IF_NO_CUDA();
    EXPECT_EQ(runEntry(cajeta::xpu::Backend::Nvptx, "runStaged"), kStagedWant);
}

// 4.1.1.2 and 4.1.1.3: each instantiation registers under its own name, and
// the manifest carries that instantiation's static shared bytes.
TEST(XpuTemplateKernel, eachInstantiationRegistersByNameWithItsOwnFootprint) {
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, kSource);
    ASSERT_NE(module, nullptr);
    auto kernels = tileKernels(module, "staged");
    ASSERT_EQ(kernels.size(), 2u) << "Tile<4> and Tile<7> each carry 'staged'";

    llvm::LLVMContext ctx;
    llvm::Module host("xpu_template_host", ctx);
    std::vector<cajeta::xpu::KernelManifest> out;
    testing::internal::CaptureStderr();
    cajeta::xpu::nvidia::emitKernelRegistration(kernels, host, "sm_89", &out);
    std::string err = testing::internal::GetCapturedStderr();
    if (out.empty()) {
        GTEST_SKIP() << "no cubin assembled on this box (ptxas absent or too "
                        "old), so there is no manifest to inspect: " << err;
    }
    std::map<std::string, unsigned> lds;
    for (auto& m : out) lds[m.kernel] = m.ldsStaticBytes.value_or(0);
    ASSERT_EQ(lds.size(), 2u) << err;
    ASSERT_TRUE(lds.count("test.Tile<4>.staged")) << err;
    ASSERT_TRUE(lds.count("test.Tile<7>.staged")) << err;
    EXPECT_EQ(lds["test.Tile<4>.staged"], 4u * 8u * 4u);
    EXPECT_EQ(lds["test.Tile<7>.staged"], 7u * 8u * 4u);

    // The runtime registry's key is the string the registration ctor passes.
    std::set<std::string> strings;
    for (auto& g : host.globals()) {
        if (!g.hasInitializer()) continue;
        if (auto* cda = llvm::dyn_cast<llvm::ConstantDataArray>(g.getInitializer()))
            if (cda->isCString()) strings.insert(cda->getAsCString().str());
    }
    EXPECT_TRUE(strings.count("Tile_4_staged"));
    EXPECT_TRUE(strings.count("Tile_7_staged"));
    EXPECT_FALSE(strings.count("staged"))
        << "an instantiation's kernel never registers under the bare name";
}
