// A kernel without wave ops widens across work-items on the cpu where its writes are disjoint but
// the vectorizer cannot bound them; a body with a loop of its own stays a work-item at a time
// (gpu-kernel-parity 21.1.1, 21.2.2).
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

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <cstdlib>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

const char* kSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class M {
    @Kernel
    public static void gather(KernelBuffer<float32> y, KernelBuffer<float32> x, KernelBuffer<int32> idx, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) { y[(int64) i] = x[(int64) idx[(int64) i]] * 2.0f; }
    }
    @Kernel
    public static void biasRows(KernelBuffer<float32> y, KernelBuffer<float32> b, uint32 cols, uint32 total) {
        uint32 i = KernelThread.globalIdX();
        if (i < total) { y[(int64) i] = y[(int64) i] + b[(int64) (i % cols)]; }
    }
    @Kernel
    public static void rowSum(KernelBuffer<float32> y, KernelBuffer<float32> x, uint32 rows, uint32 cols) {
        uint32 r = KernelThread.globalIdX();
        if (r < rows) {
            float32 acc = 0.0f;
            uint32 k = 0;
            while (k < cols) {
                acc = acc + x[(int64) r * (int64) cols + (int64) k] * (float32) (k % 3 + 1);
                k = k + 1;
            }
            y[(int64) r] = acc;
        }
    }
    @Kernel(targets = {"cpu"})
    public static void pile(KernelBuffer<float32> y, KernelBuffer<float32> x, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) { y[0L] = y[0L] + x[(int64) i]; }
    }
    @Kernel(targets = {"cpu"})
    public static void pairs(KernelBuffer<float32> y, KernelBuffer<float32> x, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) { int64 o = (int64) (i / 2); y[o] = y[o] + x[(int64) i]; }
    }
    @Kernel
    public static void pileAnywhere(KernelBuffer<float32> y, KernelBuffer<float32> x, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) { y[0L] = y[0L] + x[(int64) i]; }
    }
    public static int32 piled() {
        int64 n = 64L;
        float32[] xh = heap float32[n];
        float32[] zh = heap float32[n];
        int64 i = 0L;
        while (i < n) { xh[i] = (float32) (i + 1L); zh[i] = 0.0f; i = i + 1L; }
        KernelBuffer<float32> x = heap KernelBuffer<float32>((uint64) n);
        KernelBuffer<float32> y = heap KernelBuffer<float32>((uint64) n);
        KernelBuffer<float32> p = heap KernelBuffer<float32>((uint64) n);
        KernelBuffer<float32> q = heap KernelBuffer<float32>((uint64) n);
        x.upload(xh);
        y.upload(zh);
        p.upload(zh);
        q.upload(zh);
        KernelStream s #= KernelStream.current();
        pile.launch(s, grid: [1], block: [64])(y, x, (uint32) n);
        pairs.launch(s, grid: [1], block: [64])(p, x, (uint32) n);
        pileAnywhere.launch(s, grid: [1], block: [64])(q, x, (uint32) n);
        s.sync();
        float32[] yy = heap float32[n];
        float32[] pp = heap float32[n];
        y.download(yy);
        p.download(pp);
        int32 bad = 0;
        float32[] qq = heap float32[n];
        q.download(qq);
        if (yy[0L] != 2080.0f) { bad = bad + 1; }
        if (qq[0L] != 2080.0f) { bad = bad + 1; }
        i = 0L;
        while (i < n / 2L) { if (pp[i] != xh[2L * i] + xh[2L * i + 1L]) { bad = bad + 1; } i = i + 1L; }
        return bad;
    }
    public static int32 run() {
        int64 n = 1000L;
        int64 rows = 37L;
        int64 cols = 29L;
        float32[] xh = heap float32[n];
        int32[] ih = heap int32[n];
        float32[] yh = heap float32[rows * cols];
        float32[] bh = heap float32[cols];
        float32[] mh = heap float32[rows * cols];
        int64 i = 0L;
        while (i < n) { xh[i] = (float32) (i % 97L); ih[i] = (int32) ((i * 7L + 3L) % n); i = i + 1L; }
        i = 0L;
        while (i < rows * cols) { yh[i] = (float32) (i % 11L); i = i + 1L; }
        i = 0L;
        while (i < cols) { bh[i] = (float32) (i + 1L); i = i + 1L; }
        i = 0L;
        while (i < rows * cols) { mh[i] = (float32) ((i * 5L) % 13L); i = i + 1L; }
        KernelBuffer<float32> x = heap KernelBuffer<float32>((uint64) n);
        KernelBuffer<int32> idx = heap KernelBuffer<int32>((uint64) n);
        KernelBuffer<float32> g = heap KernelBuffer<float32>((uint64) n);
        KernelBuffer<float32> y = heap KernelBuffer<float32>((uint64) (rows * cols));
        KernelBuffer<float32> b = heap KernelBuffer<float32>((uint64) cols);
        KernelBuffer<float32> m = heap KernelBuffer<float32>((uint64) (rows * cols));
        KernelBuffer<float32> rs = heap KernelBuffer<float32>((uint64) rows);
        x.upload(xh);
        idx.upload(ih);
        y.upload(yh);
        b.upload(bh);
        m.upload(mh);
        KernelStream s #= KernelStream.current();
        gather.launch(s, grid: [(uint32) ((n + 15L) / 16L)], block: [16])(g, x, idx, (uint32) n);
        biasRows.launch(s, grid: [(uint32) ((rows * cols + 15L) / 16L)], block: [16])
            (y, b, (uint32) cols, (uint32) (rows * cols));
        rowSum.launch(s, grid: [(uint32) ((rows + 15L) / 16L)], block: [16])(rs, m, (uint32) rows, (uint32) cols);
        s.sync();
        float32[] gg = heap float32[n];
        float32[] yy = heap float32[rows * cols];
        float32[] rr = heap float32[rows];
        g.download(gg);
        y.download(yy);
        rs.download(rr);
        int32 bad = 0;
        i = 0L;
        while (i < n) { if (gg[i] != xh[(int64) ih[i]] * 2.0f) { bad = bad + 1; } i = i + 1L; }
        i = 0L;
        while (i < rows * cols) {
            if (yy[i] != yh[i] + bh[i % cols]) { bad = bad + 1; }
            i = i + 1L;
        }
        int64 r = 0L;
        while (r < rows) {
            float32 want = 0.0f;
            int64 k = 0L;
            while (k < cols) { want = want + mh[r * cols + k] * (float32) (k % 3L + 1L); k = k + 1L; }
            if (rr[r] != want) { bad = bad + 1; }
            r = r + 1L;
        }
        return bad;
    }
}
)CJ";

// The width the cpu backend widened `kernel`'s work-item loop to; 0 when it did not register.
unsigned widenedTo(const char* kernel) {
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, kSource);
    cajeta::MethodPtr k;
    for (auto& [name, m] : module->getStructures()["test.M"]->getMethods())
        if (m && m->getName() == kernel) k = m;
    if (!k) return 0;
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_plain_widen", ctx);
    std::vector<cajeta::xpu::KernelManifest> out;
    cajeta::xpu::cpu::emitKernelRegistration({k}, host, "", &out);
    if (out.size() != 1) return 0;
    return out[0].vectorWidth.value_or(0);
}

int mismatches(const char* entry = "run") {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kSource, "test.M", o);
    if (!jit) return -1;
    auto fn = jit->lookup<int (*)()>(entry);
    return fn ? fn() : -1;
}

struct ScopedEnv {
    const char* name;
    ScopedEnv(const char* n, const char* v) : name(n) { setenv(n, v, 1); }
    ~ScopedEnv() { unsetenv(name); }
};

} // namespace

TEST(XpuCpuPlainWiden, aGatherWidens) {
    EXPECT_GE(widenedTo("gather"), 2u) << "an indexed read with a store a work-item is independent across work-items";
}

TEST(XpuCpuPlainWiden, aBiasAddOverRowsWidens) {
    EXPECT_GE(widenedTo("biasRows"), 2u) << "a load by the work-item's column has no bounds the vectorizer can check";
}

TEST(XpuCpuPlainWiden, aBodyWithALoopStaysAWorkItemAtATime) {
    EXPECT_EQ(widenedTo("rowSum"), 1u) << "stepping rows together would turn each row's read into a gather";
}

TEST(XpuCpuPlainWiden, theWidenedKernelsAreRight) {
    EXPECT_EQ(mismatches(), 0) << "values that differ from the host's";
}

TEST(XpuCpuPlainWiden, theScalarBuildAgrees) {
    ScopedEnv scalar("CAJETA_XPU_CPU_NO_VECTORIZE", "1");
    EXPECT_EQ(widenedTo("gather"), 1u) << "the control: the scalar build reports width 1";
    EXPECT_EQ(mismatches(), 0) << "values that differ from the host's";
}

TEST(XpuCpuPlainWiden, workItemsOfOneBlockThatShareALocationStillRunInOrder) {
    EXPECT_EQ(mismatches("piled"), 0)
        << "a cpu-only kernel's sums into shared locations, and any kernel's sum into one fixed location";
}
