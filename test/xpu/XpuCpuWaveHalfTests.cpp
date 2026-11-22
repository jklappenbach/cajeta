// A wave kernel's float16 reads are converted 16 lanes a step on the cpu, and the values are the
// per-lane conversion's bit for bit (gpu-kernel-parity 23.1.0).
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

#include "llvm/IR/Instructions.h"
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
    public static void halves(KernelBuffer<float32> y, KernelBuffer<float16> x, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        float32 v = 0.0f;
        if (i < n) { v = (float32) x[(int64) i] + (float32) x[(int64) ((i * 7) % n)]; }
        float32 s = Wave.reduceSumF32(v);
        if (i < n) { y[(int64) i] = s + v; }
    }
    public static int32 run() {
        int64 n = 1000L;
        float16[] xh = heap float16[n];
        int64 i = 0L;
        while (i < n) { xh[i] = (float16) ((float32) ((i * 7L) % 13L) * 0.25f - 1.5f); i = i + 1L; }
        KernelBuffer<float16> x = heap KernelBuffer<float16>((uint64) n);
        KernelBuffer<float32> y = heap KernelBuffer<float32>((uint64) n);
        x.upload(xh);
        KernelStream s #= KernelStream.current();
        uint32 wv = (uint32) Device.waveSize();
        halves.launch(s, grid: [(uint32) ((n + (int64) wv - 1L) / (int64) wv)], block: [wv])(y, x, (uint32) n);
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

int checksum() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kSource, "test.M", o);
    if (!jit) return -1;
    auto fn = jit->lookup<int (*)()>("run");
    return fn ? fn() : -1;
}

// Widenings of 16 float16 lanes at once in the registered kernel; -1 when it did not register as a wave kernel.
int wideHalfConversions() {
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, kSource);
    cajeta::MethodPtr k;
    for (auto& [name, m] : module->getStructures()["test.M"]->getMethods())
        if (m && m->getName() == "halves") k = m;
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
                if (auto* fe = llvm::dyn_cast<llvm::FPExtInst>(&in))
                    if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(fe->getOperand(0)->getType()))
                        if (vt->getElementType()->isHalfTy() && vt->getNumElements() == 16) ++n;
    return n;
}

struct ScopedEnv {
    const char* name;
    ScopedEnv(const char* n, const char* v) : name(n) { setenv(n, v, 1); }
    ~ScopedEnv() { unsetenv(name); }
};

} // namespace

TEST(XpuCpuWaveHalf, theValuesAreThePerLaneConversionsBitForBit) {
    const int routed = checksum();
    ASSERT_GE(routed, 0);
    ScopedEnv perLane("CAJETA_XPU_CPU_HALF_PER_LANE", "1");
    EXPECT_EQ(checksum(), routed) << "converting 16 lanes a step and a lane at a time must agree in every bit";
}

TEST(XpuCpuWaveHalf, theLanesAreConvertedSixteenAStep) {
    const int routed = wideHalfConversions();
    ASSERT_GE(routed, 0) << "halves must register as a wave kernel";
    EXPECT_GT(routed, 0) << "the widened loop converts a 16-lane float16 vector";
}
