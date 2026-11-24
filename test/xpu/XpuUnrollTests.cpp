// `for (int64 q = Unroll.from(c); ..)` in a kernel is written out at lowering, the counter a
// constant in each copy (kernel-drivers 7.2.1).
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"

#include "cajeta/compile/CajetaModule.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/error/Exception.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string source(const std::string& bound) {
    return std::string(R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Unroll;
public class U {
    public U() { return; }

    @Kernel
    public static void run(KernelBuffer<int32> y, KernelBuffer<int32> x, uint32 n, int64 m) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) {
            int32 acc = 0;
            for (int64 q = Unroll.from(0L); q < )CJ") + bound + R"CJ(; q = q + 1L) {
                int32 v = x[(int64) i * 4L + q];
                acc = acc + (v >> (int32) (2L * q)) * (int32) (q + 1L);
            }
            y[(int64) i] = acc;
        }
    }

    public int32 wrong() {
        int64 n = 64L;
        int32[] xh = heap int32[n * 4L];
        int64 i = 0L;
        while (i < n * 4L) { xh[i] = (int32) ((i * 2654435L + 77L) % 100000L); i = i + 1L; }
        KernelBuffer<int32> x = heap KernelBuffer<int32>((uint64) (n * 4L));
        KernelBuffer<int32> y = heap KernelBuffer<int32>((uint64) n);
        x.upload(xh);
        KernelStream s #= KernelStream.current();
        run.launch(s, grid: [1], block: [64])(y, x, (uint32) n, 4L);
        s.sync();
        int32[] got = heap int32[n];
        y.download(got);
        int32 bad = 0;
        i = 0L;
        while (i < n) {
            int32 want = 0;
            for (int64 q = Unroll.from(0L); q < 4L; q = q + 1L) {
                want = want + (xh[i * 4L + q] >> (int32) (2L * q)) * (int32) (q + 1L);
            }
            if (got[i] != want) { bad = bad + 1; }
            i = i + 1L;
        }
        return bad;
    }
}
public class M {
    public static int32 run() { U u #= heap U(); return u.wrong(); }
}
)CJ";
}

std::string loweredOrError(const std::string& src) {
    cajeta::Compiler compiler;
    try {
        auto module = cajeta::xpu::probe::compileForInspection(compiler, src, "test.U");
        if (!module) return "did not compile";
        auto k = cajeta::xpu::probe::findMethod(module->getStructures()["test.U"], "run");
        if (!k) return "no kernel";
        llvm::LLVMContext ctx;
        llvm::Module dev("unroll_probe", ctx);
        auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
        if (!tm) return "";
        cajeta::xpu::vulkan::configureDeviceModule(dev, *tm);
        cajeta::xpu::vulkan::lowerKernel(k, dev);
        std::string ir;
        llvm::raw_string_ostream os(ir);
        dev.print(os, nullptr);
        return os.str();
    } catch (cajeta::Exception& e) {
        return "ERROR: " + e.getMessage();
    } catch (const char* e) {
        return std::string("ERROR: ") + e;
    } catch (std::exception& e) {
        return std::string("ERROR: ") + e.what();
    }
}

} // namespace

TEST(XpuUnroll, theLoopComputesWhatTheHostLoopDoesOnCpu) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(source("4L"), "test.M", o);
    ASSERT_TRUE(jit);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}

TEST(XpuUnroll, theLoweredKernelHasNoLoopAndConstantShifts) {
    const std::string ir = loweredOrError(source("4L"));
    if (ir.empty()) GTEST_SKIP() << "no spirv target";
    ASSERT_EQ(ir.find("ERROR"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("for.head"), std::string::npos) << ir;
    EXPECT_NE(ir.find(", 6"), std::string::npos) << "the last copy shifts by the constant 6\n" << ir;
}

TEST(XpuUnroll, aBoundThatIsNotAConstantIsRefusedByName) {
    const std::string why = loweredOrError(source("m"));
    if (why.empty()) GTEST_SKIP() << "no spirv target";
    EXPECT_NE(why.find("Unroll.from"), std::string::npos) << why;
    EXPECT_NE(why.find("compile-time"), std::string::npos) << why;
}
