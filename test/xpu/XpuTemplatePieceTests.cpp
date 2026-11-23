// A kernel in a class template calls its class parameter's @Device methods as its pieces: each
// instantiation computes with its own piece, inlined (kernel-drivers 1.1.1 to 1.1.3).
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "XpuDeviceTestUtil.h"
#include "KernelLoweringProbe.h"

#include "cajeta/compile/CajetaModule.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/error/Exception.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/core/KernelManifest.h"
#include "cajeta/xpu/cpu/CpuRegistration.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

const char* kSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public final class Twice {
    @Device
    public static float32 fold(KernelBuffer<float32> x, int64 at, float32 acc) { return acc + x[at] * 2.0f; }
    @Device
    public static float32 finish(float32 acc) { return acc + 1.0f; }
}
public final class Thrice {
    @Device
    public static float32 fold(KernelBuffer<float32> x, int64 at, float32 acc) { return acc + x[at] * 3.0f; }
    @Device
    public static float32 finish(float32 acc) { return acc - 1.0f; }
}
@Instantiate({Driver<Twice, 4>, Driver<Thrice, 8>})
public final class Driver<P, uint32 SPAN> {
    public Driver() { return; }

    @Kernel
    public static void run(KernelBuffer<float32> y, KernelBuffer<float32> x, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) {
            float32 acc = 0.0f;
            uint32 k = 0;
            while (k < SPAN) { acc = P.fold(x, (int64) (i * SPAN + k), acc); k = k + 1; }
            y[(int64) i] = P.finish(acc);
        }
    }

    public int32 at(int32 k) {
        int64 n = 64L;
        float32[] xh = heap float32[n * 8L];
        int64 i = 0L;
        while (i < n * 8L) { xh[i] = (float32) (i % 5L); i = i + 1L; }
        KernelBuffer<float32> x = heap KernelBuffer<float32>((uint64) (n * 8L));
        KernelBuffer<float32> y = heap KernelBuffer<float32>((uint64) n);
        x.upload(xh);
        KernelStream s #= KernelStream.current();
        run.launch(s, grid: [1], block: [64])(y, x, (uint32) n);
        s.sync();
        float32[] got = heap float32[n];
        y.download(got);
        return (int32) got[k];
    }
}
public class M {
    public static int32 run() {
        Driver<Twice, 4> a #= heap Driver<Twice, 4>();
        Driver<Thrice, 8> b #= heap Driver<Thrice, 8>();
        return a.at(3) * 1000 + b.at(3);
    }
}
)CJ";

// Twice over 4: x[12..15] = 2,3,4,0, sum 9, doubled 18, plus 1 = 19.
// Thrice over 8: x[24..31] = 4,0,1,2,3,4,0,1, sum 15, tripled 45, minus 1 = 44.
constexpr int kWant = 19 * 1000 + 44;

// A kernel in a class template calling its own class's @Device helper by the template's name.
const char* kOwnHelperSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public final class Two {
    @Device
    public static int32 k() { return 2; }
}
public final class Own<P> {
    public Own() { return; }

    @Device
    static int32 plus(int32 a, int32 b) { return a + b; }

    @Kernel
    public static void run(KernelBuffer<int32> y, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) { y[(int64) i] = Own.plus((int32) i, P.k()); }
    }

    public int32 at(int32 k) {
        KernelBuffer<int32> y = heap KernelBuffer<int32>((uint64) 64L);
        KernelStream s #= KernelStream.current();
        run.launch(s, grid: [1], block: [64])(y, 64);
        s.sync();
        int32[] got = heap int32[64L];
        y.download(got);
        return got[k];
    }
}
public class M {
    public static int32 run() { Own<Two> a #= heap Own<Two>(); return a.at(40); }
}
)CJ";

int runOn(cajeta::xpu::Backend backend) {
    CajetaJit::Options o;
    o.xpuBackends = {backend};
    auto jit = CajetaJit::compile(kSource, "test.M", o);
    if (!jit) return -2;
    auto fn = jit->lookup<int (*)()>("run");
    return fn ? fn() : -2;
}

std::string compileError(const std::string& source) {
    cajeta::Compiler compiler;
    try {
        auto module = cajeta::xpu::probe::compileForInspection(compiler, source);
        if (!module) return "did not compile";
        std::vector<cajeta::MethodPtr> kernels;
        for (auto& [canon, klass] : module->getStructures()) {
            if (canon.rfind("test.Driver<", 0) != 0 || !klass) continue;
            for (auto& [k, m] : klass->getMethods())
                if (m && m->getName() == "run") kernels.push_back(m);
        }
        llvm::LLVMContext ctx;
        llvm::Module host("xpu_piece_host", ctx);
        std::vector<cajeta::xpu::KernelManifest> out;
        testing::internal::CaptureStderr();
        cajeta::xpu::cpu::emitKernelRegistration(kernels, host, "", &out);
        return testing::internal::GetCapturedStderr();
    } catch (cajeta::Exception& e) {
        return e.getMessage();
    } catch (const char* e) {
        return e;
    } catch (std::exception& e) {
        return e.what();
    }
}

} // namespace

TEST(XpuTemplatePiece, eachInstantiationComputesWithItsOwnPieceOnCpu) {
    EXPECT_EQ(runOn(cajeta::xpu::Backend::Cpu), kWant);
}

TEST(XpuTemplatePiece, aKernelCallsItsOwnClassHelperByTheTemplatesNameOnCpu) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kOwnHelperSource, "test.M", o);
    ASSERT_TRUE(jit);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 42);
}

TEST(XpuTemplatePiece, eachInstantiationComputesWithItsOwnPieceOnNvptx) {
    CAJETA_SKIP_IF_NO_CUDA();
    EXPECT_EQ(runOn(cajeta::xpu::Backend::Nvptx), kWant);
}

TEST(XpuTemplatePiece, eachInstantiationComputesWithItsOwnPieceOnAmdgpu) {
    CAJETA_SKIP_IF_NO_HIP();
    EXPECT_EQ(runOn(cajeta::xpu::Backend::Amdgpu), kWant);
}

TEST(XpuTemplatePiece, eachInstantiationComputesWithItsOwnPieceOnVulkan) {
    CAJETA_SKIP_IF_NO_VULKAN();
    EXPECT_EQ(runOn(cajeta::xpu::Backend::Spirv), kWant);
}

TEST(XpuTemplatePiece, thePiecesAreInlinedIntoTheKernel) {
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, kSource);
    ASSERT_NE(module, nullptr);
    std::vector<cajeta::MethodPtr> kernels;
    for (auto& [canon, klass] : module->getStructures()) {
        if (canon.rfind("test.Driver<", 0) != 0 || !klass) continue;
        for (auto& [k, m] : klass->getMethods())
            if (m && m->getName() == "run") kernels.push_back(m);
    }
    ASSERT_EQ(kernels.size(), 2u);
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_piece_host", ctx);
    std::vector<cajeta::xpu::KernelManifest> out;
    cajeta::xpu::cpu::emitKernelRegistration(kernels, host, "", &out);
    ASSERT_EQ(out.size(), 2u);
    int blocks = 0;
    for (llvm::Function& f : host) {
        if (!f.getName().starts_with("__cajeta_xpu_cpu_block.")) continue;
        ++blocks;
        for (llvm::BasicBlock& bb : f)
            for (llvm::Instruction& in : bb)
                if (auto* c = llvm::dyn_cast<llvm::CallInst>(&in))
                    if (llvm::Function* callee = c->getCalledFunction())
                        EXPECT_FALSE(callee->getName().contains("fold") || callee->getName().contains("finish"))
                            << f.getName().str() << " calls " << callee->getName().str();
    }
    EXPECT_EQ(blocks, 2);
}

TEST(XpuTemplatePiece, aPieceWithoutAMethodTheDriverCallsIsRefusedByName) {
    std::string source = kSource;
    const std::string from = "public static float32 finish(float32 acc) { return acc - 1.0f; }";
    source.replace(source.find(from), from.size(),
                   "public static float32 done(float32 acc) { return acc - 1.0f; }");
    const std::string why = compileError(source);
    EXPECT_NE(why.find("test.Thrice"), std::string::npos) << why;
    EXPECT_NE(why.find("finish"), std::string::npos) << why;
    EXPECT_NE(why.find("no @Device method"), std::string::npos) << why;
}
