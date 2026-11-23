// A wave kernel's lane that reads four float16 elements in a row reads them in one step with its
// neighbours on the cpu, and the values are the per-element reads bit for bit (gpu-kernel-parity 23.1.0).
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
    public static void quads(KernelBuffer<float32> y, KernelBuffer<float16> x, uint32 n, uint32 stride, uint32 skip) {
        uint32 i = KernelThread.globalIdX();
        float32 v = 0.0f;
        if (i < n && i % skip != 1) {
            uint32 at = i * stride;
            v = (float32) x[at] + 2.0f * (float32) x[at + 1] + 3.0f * (float32) x[at + 2] + 5.0f * (float32) x[at + 3];
        }
        float32 s = Wave.reduceSumF32(v);
        if (i < n) { y[(int64) i] = s + v; }
    }
    @Kernel
    public static void triples(KernelBuffer<float32> y, KernelBuffer<float16> x, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        float32 v = 0.0f;
        if (i < n) {
            uint32 at = i * 4;
            v = (float32) x[at] + 2.0f * (float32) x[at + 1] + 3.0f * (float32) x[at + 2];
        }
        float32 s = Wave.reduceSumF32(v);
        if (i < n) { y[(int64) i] = s + v; }
    }
    static int32 go(uint32 stride, uint32 skip) {
        int64 n = 1000L;
        int64 len = n * 8L + 8L;
        float16[] xh = heap float16[len];
        int64 i = 0L;
        while (i < len) { xh[i] = (float16) ((float32) ((i * 7L) % 13L) * 0.25f - 1.5f); i = i + 1L; }
        KernelBuffer<float16> x = heap KernelBuffer<float16>((uint64) len);
        KernelBuffer<float32> y = heap KernelBuffer<float32>((uint64) n);
        x.upload(xh);
        KernelStream s #= KernelStream.current();
        uint32 wv = (uint32) Device.waveSize();
        quads.launch(s, grid: [(uint32) ((n + (int64) wv - 1L) / (int64) wv)], block: [wv])(y, x, (uint32) n, stride, skip);
        s.sync();
        float32[] got = heap float32[n];
        y.download(got);
        int64 sum = 0L;
        i = 0L;
        while (i < n) { sum = (sum * 31L + (int64) (Cajeta.f32ToBits(got[i]) & 16777215)) % 1000003L; i = i + 1L; }
        return (int32) sum;
    }
    public static int32 inARow() { return M.go(4, 1001); }
    public static int32 eightApart() { return M.go(8, 1001); }
    public static int32 sixApart() { return M.go(6, 1001); }
    public static int32 someLanesOff() { return M.go(4, 3); }
}
)CJ";

int checksum(const char* entry) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kSource, "test.M", o);
    if (!jit) return -1;
    auto fn = jit->lookup<int (*)()>(entry);
    return fn ? fn() : -1;
}

// Calls of the widened four-element read in the registered kernel; -1 when it did not register as a wave kernel.
int wideQuadReads(const char* kernel) {
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, kSource);
    cajeta::MethodPtr k;
    for (auto& [name, m] : module->getStructures()["test.M"]->getMethods())
        if (m && m->getName() == kernel) k = m;
    if (!k) return -1;
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_wave_quad", ctx);
    std::vector<cajeta::xpu::KernelManifest> out;
    cajeta::xpu::cpu::emitKernelRegistration({k}, host, "", &out);
    if (out.size() != 1 || out[0].waveWidth.value_or(1) < 2) return -1;
    int n = 0;
    for (llvm::Function& f : host) {
        if (f.getName().starts_with("__cajeta_xpu_cpu_ld4h")) continue;
        for (llvm::BasicBlock& bb : f)
            for (llvm::Instruction& in : bb)
                if (auto* c = llvm::dyn_cast<llvm::CallInst>(&in))
                    if (llvm::Function* callee = c->getCalledFunction())
                        if (callee->getName().starts_with("__cajeta_xpu_cpu_ld4h_") && c->getType()->isVectorTy()) ++n;
    }
    return n;
}

struct ScopedEnv {
    const char* name;
    ScopedEnv(const char* n, const char* v) : name(n) { setenv(n, v, 1); }
    ~ScopedEnv() { unsetenv(name); }
};

void expectSameAsPerElement(const char* entry) {
    const int grouped = checksum(entry);
    ASSERT_GE(grouped, 0);
    ScopedEnv perLane("CAJETA_XPU_CPU_HALF_QUAD_PER_LANE", "1");
    EXPECT_EQ(checksum(entry), grouped) << entry << ": the grouped read and the per-element reads must agree in every bit";
}

} // namespace

TEST(XpuCpuWaveQuadRead, lanesInARowReadTheSameValues) { expectSameAsPerElement("inARow"); }

TEST(XpuCpuWaveQuadRead, lanesEightApartReadTheSameValues) { expectSameAsPerElement("eightApart"); }

TEST(XpuCpuWaveQuadRead, lanesSixApartReadTheSameValues) { expectSameAsPerElement("sixApart"); }

TEST(XpuCpuWaveQuadRead, aWaveWithLanesOffReadsTheSameValues) { expectSameAsPerElement("someLanesOff"); }

TEST(XpuCpuWaveQuadRead, fourInARowAreReadAsOne) {
    const int grouped = wideQuadReads("quads");
    ASSERT_GE(grouped, 0) << "quads must register as a wave kernel";
    EXPECT_GT(grouped, 0) << "the widened loop reads the wave's elements in one step";
    ScopedEnv perLane("CAJETA_XPU_CPU_HALF_QUAD_PER_LANE", "1");
    EXPECT_EQ(wideQuadReads("quads"), 0);
}

TEST(XpuCpuWaveQuadRead, threeInARowAreLeftAlone) {
    EXPECT_EQ(wideQuadReads("triples"), 0);
}
