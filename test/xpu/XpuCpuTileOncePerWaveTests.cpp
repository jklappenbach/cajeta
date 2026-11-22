// A software cooperative-matrix tile in a `@Wave(width = N)` kernel is held once a wave on the cpu
// and its verbs run on the wave's first lane (gpu-kernel-parity 22.2.3).
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

#include "llvm/IR/GlobalVariable.h"
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
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class M {
    @Kernel @Wave(width = 32) @Occupancy(maxThreads = 64)
    public static void tiles(KernelBuffer<float32> c, KernelBuffer<float16> a, KernelBuffer<float16> b,
            uint32 reps) {
        uint32 g = KernelThread.globalIdX() / 32;
        CooperativeMatrix<float16,16,16,0> ma;
        CooperativeMatrix<float16,16,16,1> mb;
        CooperativeMatrix<float32,16,16,2> acc;
        ma.load(a, g * 256, 0, 16);
        mb.load(b, g * 256, 0, 16);
        acc.splat(0.0f);
        uint32 r = 0;
        while (r < reps) { acc.mma(ma, mb); r = r + 1; }
        uint32 n = acc.elements();
        uint32 e = 0;
        while (e < n) { acc.set(e, acc.get(e) * (float32) (acc.row(e) + 1)); e = e + 1; }
        acc.store(c, g * 256, 0, 16);
    }
    public static int32 run() {
        int64 tiles = 6L;
        int64 n = tiles * 256L;
        float16[] ah = heap float16[n];
        float16[] bh = heap float16[n];
        float32[] af = heap float32[n];
        float32[] bf = heap float32[n];
        int64 i = 0L;
        while (i < n) {
            af[i] = (float32) ((i * 7L + i / 256L) % 9L);
            bf[i] = (float32) ((i * 5L + 3L) % 7L);
            ah[i] = (float16) af[i];
            bh[i] = (float16) bf[i];
            i = i + 1L;
        }
        KernelBuffer<float16> a = heap KernelBuffer<float16>((uint64) n);
        KernelBuffer<float16> b = heap KernelBuffer<float16>((uint64) n);
        KernelBuffer<float32> c = heap KernelBuffer<float32>((uint64) n);
        a.upload(ah);
        b.upload(bh);
        KernelStream s #= KernelStream.current();
        tiles.launch(s, grid: [(uint32) (tiles / 2L)], block: [64])(c, a, b, 3);
        s.sync();
        float32[] got = heap float32[n];
        c.download(got);
        int32 bad = 0;
        int64 t = 0L;
        while (t < tiles) {
            int64 r = 0L;
            while (r < 16L) {
                int64 j = 0L;
                while (j < 16L) {
                    float32 want = 0.0f;
                    int64 k = 0L;
                    while (k < 16L) {
                        want = want + af[t * 256L + r * 16L + k] * bf[t * 256L + k * 16L + j];
                        k = k + 1L;
                    }
                    want = want * 3.0f * (float32) (r + 1L);
                    if (got[t * 256L + r * 16L + j] != want) { bad = bad + 1; }
                    j = j + 1L;
                }
                r = r + 1L;
            }
            t = t + 1L;
        }
        return bad;
    }
}
)CJ";

int mismatches() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kSource, "test.M", o);
    if (!jit) return -1;
    auto fn = jit->lookup<int (*)()>("run");
    return fn ? fn() : -1;
}


// Values in the registered kernel named for a once-a-wave tile store; -1 when it did not register.
int sharedMemoryUses() {
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, kSource);
    cajeta::MethodPtr k;
    for (auto& [name, m] : module->getStructures()["test.M"]->getMethods())
        if (m && m->getName() == "tiles") k = m;
    if (!k) return -1;
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_tile_once", ctx);
    std::vector<cajeta::xpu::KernelManifest> out;
    cajeta::xpu::cpu::emitKernelRegistration({k}, host, "", &out);
    if (out.size() != 1) return -1;
    int n = 0;
    for (llvm::GlobalVariable& gv : host.globals())
        if (gv.getName().contains("_cmtile_")) ++n;
    for (llvm::Function& f : host)
        for (llvm::BasicBlock& bb : f)
            for (llvm::Instruction& in : bb)
                if (in.getName().contains("_cmtile_")) ++n;
    return n;
}

struct ScopedEnv {
    const char* name;
    ScopedEnv(const char* n, const char* v) : name(n) { setenv(n, v, 1); }
    ~ScopedEnv() { unsetenv(name); }
};

} // namespace

TEST(XpuCpuTileOncePerWave, theTilesAreRightWithTwoWavesABlock) {
    EXPECT_EQ(mismatches(), 0) << "elements that differ from the host's product";
}

TEST(XpuCpuTileOncePerWave, theReplicatedBuildAgrees) {
    ScopedEnv replicated("CAJETA_XPU_CPU_TILE_REPLICATED", "1");
    EXPECT_EQ(mismatches(), 0) << "the control: every work-item holding its own tile";
}

TEST(XpuCpuTileOncePerWave, theTileLivesInWorkgroupMemory) {
    const int shared = sharedMemoryUses();
    ASSERT_GE(shared, 0) << "tiles must register";
    int replicated = 0;
    {
        ScopedEnv off("CAJETA_XPU_CPU_TILE_REPLICATED", "1");
        replicated = sharedMemoryUses();
    }
    EXPECT_GT(shared, replicated) << "held once a wave, the tile is workgroup-shared memory; replicated, it is a local";
}
