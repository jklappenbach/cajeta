// A wave kernel's exp of a value every lane shares is one scalar call a wave on the cpu, and the
// result is the per-lane call's bit for bit (gpu-kernel-parity 22.2.4).
#include "gtest/gtest.h"

#include "../PortableEnv.h"   // setenv/unsetenv on MinGW
#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/core/KernelManifest.h"
#include "cajeta/xpu/cpu/CpuRegistration.h"

#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <cstdlib>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

const char* kSource = R"CJ(
package test;
import cajeta.xpu.Device;
import cajeta.xpu.Group;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Wave;
public class M {
    @Kernel
    public static void expo(KernelBuffer<float32> y, KernelBuffer<float32> x, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        float32 v = 0.0f;
        if (i < n) { v = x[(int64) i]; }
        float32 s = Wave.reduceSumF32(v);
        float32 shared = Math.exp(s * 0.03125f);
        float32 own = Math.exp(v * 0.25f);
        if (i < n) { y[(int64) i] = shared + own; }
    }
    @Kernel
    public static void arms(KernelBuffer<float32> y, KernelBuffer<float32> x, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        float32 v = 0.0f;
        if (i < n) { v = x[(int64) i]; }
        float32 s = Wave.reduceSumF32(v) * 0.03125f;
        float32 r = 0.0f;
        if (s > v) { float32 c = Math.exp(v - s); r = v * c + 1.0f; }
        else { float32 w = Math.exp(s - v); r = v + w; }
        if (i < n) { y[(int64) i] = r; }
    }
    public static int32 runArms() {
        int64 n = 1000L;
        float32[] xh = heap float32[n];
        int64 i = 0L;
        while (i < n) { xh[i] = (float32) ((i * 7L) % 13L) - 6.0f; i = i + 1L; }
        KernelBuffer<float32> x = heap KernelBuffer<float32>((uint64) n);
        KernelBuffer<float32> y = heap KernelBuffer<float32>((uint64) n);
        x.upload(xh);
        KernelStream s #= KernelStream.current();
        uint32 wv = (uint32) Device.waveSize();
        arms.launch(s, grid: [(uint32) ((n + (int64) wv - 1L) / (int64) wv)], block: [wv])(y, x, (uint32) n);
        s.sync();
        float32[] got = heap float32[n];
        y.download(got);
        int64 sum = 0L;
        i = 0L;
        while (i < n) { sum = (sum * 31L + (int64) (Cajeta.f32ToBits(got[i]) & 16777215)) % 1000003L; i = i + 1L; }
        return (int32) sum;
    }
    public static int32 run() {
        int64 n = 1000L;
        float32[] xh = heap float32[n];
        int64 i = 0L;
        while (i < n) { xh[i] = (float32) ((i * 7L) % 13L) - 6.0f; i = i + 1L; }
        KernelBuffer<float32> x = heap KernelBuffer<float32>((uint64) n);
        KernelBuffer<float32> y = heap KernelBuffer<float32>((uint64) n);
        x.upload(xh);
        KernelStream s #= KernelStream.current();
        uint32 wv = (uint32) Device.waveSize();
        expo.launch(s, grid: [(uint32) ((n + (int64) wv - 1L) / (int64) wv)], block: [wv])(y, x, (uint32) n);
        s.sync();
        float32[] got = heap float32[n];
        y.download(got);
        int64 sum = 0L;
        i = 0L;
        while (i < n) { sum = (sum * 31L + (int64) (Cajeta.f32ToBits(got[i]) & 16777215)) % 1000003L; i = i + 1L; }
        return (int32) sum;
    }
}
)CJ";

int checksum(const char* entry = "run") {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kSource, "test.M", o);
    if (!jit) return -1;
    auto fn = jit->lookup<int (*)()>(entry);
    return fn ? fn() : -1;
}

// Scalar float32 exp calls in the registered kernel, and its widened two-arm exp calls; -1 when it did
// not register as a wave kernel.
int scalarExpCalls(const char* kernel = "expo", int* pickCalls = nullptr) {
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, kSource);
    cajeta::MethodPtr k;
    for (auto& [name, m] : module->getStructures()["test.M"]->getMethods())
        if (m && m->getName() == kernel) k = m;
    if (!k) return -1;
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_wave_exp", ctx);
    std::vector<cajeta::xpu::KernelManifest> out;
    cajeta::xpu::cpu::emitKernelRegistration({k}, host, "", &out);
    if (out.size() != 1 || out[0].waveWidth.value_or(1) < 2) return -1;
    int n = 0;
    for (llvm::Function& f : host)
        for (llvm::BasicBlock& bb : f)
            for (llvm::Instruction& in : bb)
                if (auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(&in))
                    if (ii->getIntrinsicID() == llvm::Intrinsic::exp && ii->getType()->isFloatTy()) ++n;
    if (pickCalls) {
        *pickCalls = 0;
        for (llvm::Function& f : host)
            for (llvm::BasicBlock& bb : f)
                for (llvm::Instruction& in : bb)
                    if (auto* c = llvm::dyn_cast<llvm::CallInst>(&in))
                        if (llvm::Function* callee = c->getCalledFunction())
                            if (callee->getName().starts_with("__cajeta_xpu_cpu_exp_pick_f32_") && c->getType()->isVectorTy())
                                ++*pickCalls;
    }
    return n;
}

struct ScopedEnv {
    const char* name;
    ScopedEnv(const char* n, const char* v) : name(n) { setenv(n, v, 1); }
    ~ScopedEnv() { unsetenv(name); }
};

} // namespace

TEST(XpuCpuWaveExp, theResultIsThePerLaneCallsBitForBit) {
    const int routed = checksum();
    ASSERT_GE(routed, 0);
    ScopedEnv perLane("CAJETA_XPU_CPU_EXP_PER_LANE", "1");
    EXPECT_EQ(checksum(), routed) << "one call a wave and a call a lane must agree in every bit";
}

TEST(XpuCpuWaveExp, aSharedValueTakesTheSingleCallPath) {
    const int routed = scalarExpCalls();
    ASSERT_GE(routed, 0) << "expo must register as a wave kernel";
    ScopedEnv perLane("CAJETA_XPU_CPU_EXP_PER_LANE", "1");
    EXPECT_GT(routed, scalarExpCalls()) << "the widened loop holds the scalar call it makes when the lanes agree";
}

TEST(XpuCpuWaveExp, twoArmsOfOneBranchShareOneCallWithTheSameValues) {
    const int merged = checksum("runArms");
    ASSERT_GE(merged, 0);
    int picks = 0;
    ASSERT_GT(scalarExpCalls("arms", &picks), 0) << "arms must register as a wave kernel";
    EXPECT_EQ(picks, 1) << "the two arms' exp calls become one widened call";
    ScopedEnv perArm("CAJETA_XPU_CPU_EXP_PER_ARM", "1");
    EXPECT_EQ(checksum("runArms"), merged) << "one exp for both arms and an exp an arm must agree in every bit";
    scalarExpCalls("arms", &picks);
    EXPECT_EQ(picks, 0);
}
