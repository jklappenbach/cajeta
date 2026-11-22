// A wave kernel whose lanes run explicit Vector code keeps that code whole on the cpu: the region
// runs lane by lane, and only regions with a wave op are widened (gpu-kernel-parity 20.2.2).
#include "gtest/gtest.h"

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
    public static void dots(KernelBuffer<float32> y, KernelBuffer<int8> w, KernelBuffer<int8> x,
            uint32 rows, uint32 chunks) {
        uint32 wv = (uint32) Group.width();
        uint32 lane = KernelThread.x() % wv;
        uint32 row = KernelThread.globalIdX() / wv;
        float32 acc = 0.0f;
        if (row < rows) {
            Vector<int32,8> zv = heap Vector<int32,8>(0, 0, 0, 0, 0, 0, 0, 0);
            uint32 c = lane;
            while (c < chunks) {
                Vector<uint8,32> a = w.vload<32>((int64) (row * chunks + c) * 32L).asUnsigned();
                Vector<int8,32> b = x.vload<32>((int64) c * 32L);
                Vector<int32,8> d = a.dotAccum(b, zv);
                acc = acc + (float32) (d[0] + d[1] + d[2] + d[3] + d[4] + d[5] + d[6] + d[7]);
                c = c + wv;
            }
        }
        float32 t = Wave.reduceSumF32(acc);
        if (lane == 0 && row < rows) { y[(int64) row] = t; }
    }
    public static int32 run() {
        int64 rows = 37L;
        int64 chunks = 40L;
        int8[] wh = heap int8[rows * chunks * 32L];
        int8[] xh = heap int8[chunks * 32L];
        int64 i = 0L;
        while (i < rows * chunks * 32L) { wh[i] = (int8) ((i * 7L + i / 1280L) % 16L); i = i + 1L; }
        i = 0L;
        while (i < chunks * 32L) { xh[i] = (int8) ((i % 23L) - 11L); i = i + 1L; }
        KernelBuffer<int8> w = heap KernelBuffer<int8>((uint64) (rows * chunks * 32L));
        KernelBuffer<int8> x = heap KernelBuffer<int8>((uint64) (chunks * 32L));
        KernelBuffer<float32> y = heap KernelBuffer<float32>((uint64) rows);
        w.upload(wh);
        x.upload(xh);
        KernelStream s #= KernelStream.current();
        dots.launch(s, grid: [(uint32) rows], block: [(uint32) Device.waveSize()])(y, w, x, (uint32) rows, (uint32) chunks);
        s.sync();
        float32[] got = heap float32[rows];
        y.download(got);
        int32 bad = 0;
        int64 r = 0L;
        while (r < rows) {
            int64 want = 0L;
            int64 k = 0L;
            while (k < chunks * 32L) { want = want + (int64) wh[r * chunks * 32L + k] * (int64) xh[k]; k = k + 1L; }
            if (got[r] != (float32) want) { bad = bad + 1; }
            r = r + 1L;
        }
        return bad;
    }
}
)CJ";

// Byte vectors in the registered kernel that are not a lane's own 32-byte vector: a lane's vector
// widened across the wave becomes one of these an element.
int laneWideByteVectors() {
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, kSource);
    cajeta::MethodPtr k;
    for (auto& [name, m] : module->getStructures()["test.M"]->getMethods())
        if (m && m->getName() == "dots") k = m;
    if (!k) return -1;
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_wave_vector_body", ctx);
    std::vector<cajeta::xpu::KernelManifest> out;
    cajeta::xpu::cpu::emitKernelRegistration({k}, host, "", &out);
    if (out.size() != 1 || out[0].waveWidth.value_or(1) < 2) return -1;
    int n = 0;
    for (llvm::Function& f : host)
        for (llvm::BasicBlock& bb : f)
            for (llvm::Instruction& in : bb)
                if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(in.getType()))
                    if (vt->getElementType()->isIntegerTy(8) && vt->getNumElements() != 32) ++n;
    return n;
}

struct ScopedEnv {
    const char* name;
    ScopedEnv(const char* n, const char* v) : name(n) { setenv(n, v, 1); }
    ~ScopedEnv() { unsetenv(name); }
};

} // namespace

TEST(XpuCpuWaveVectorBody, aLanesVectorIsNotWidenedAcrossTheWave) {
    const int n = laneWideByteVectors();
    ASSERT_GE(n, 0) << "dots must register as a wave kernel";
    EXPECT_LT(n, 64) << "the region holding each lane's 32-byte vectors must run lane by lane";
}

TEST(XpuCpuWaveVectorBody, wideningAcrossTheWaveBreaksALanesVectorUp) {
    ScopedEnv widen("CAJETA_XPU_CPU_WIDEN_VECTOR_BODY", "1");
    EXPECT_GT(laneWideByteVectors(), 500)
        << "the control: widened across lanes, every element of a lane's vector is its own vector";
}

TEST(XpuCpuWaveVectorBody, theDotsAreRight) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kSource, "test.M", o);
    ASSERT_TRUE(jit);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_TRUE(fn);
    EXPECT_EQ(fn(), 0) << "rows whose dot differs from the host's";
}
