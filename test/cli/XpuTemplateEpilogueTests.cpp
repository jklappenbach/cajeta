// cajeta-ml-gpu Unit 9 (9.1.2, 9.2.1; spec §5.4, §11.1): a consumer supplies work fused
// into a library kernel by instantiating a class template over the epilogue TYPE; the
// kernel calls the type's @Device helper, which the lowering resolves through the
// instantiation's argument. The AOT path; the JIT path is test/xpu/XpuTemplateEpilogueTests.cpp.
#include <gtest/gtest.h>
#include <string>
#include "AotProgram.h"

using namespace cajeta_test_aot;

namespace {
const char* kProgram = R"CAJ(package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
/** A consumer's epilogue: bias, then relu. */
public class BiasRelu {
    public BiasRelu() { return; }
    @Device
    public static float32 apply(float32 acc, float32 bias) {
        float32 v = acc + bias;
        if (v < 0.0f) { v = 0.0f; }
        return v;
    }
}
/** A library kernel skeleton whose epilogue is the type parameter. */
public class Fused<E> {
    public Fused() { return; }
    @Kernel
    public static void scaleThen(KernelBuffer<float32> y, KernelBuffer<float32> x,
                                 KernelBuffer<float32> b, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) { y[i] = E.apply(x[i] * 2.0f, b[i]); }
    }
    public void run(KernelBuffer<float32> y, KernelBuffer<float32> x, KernelBuffer<float32> b, uint32 n) {
        KernelStream s #= KernelStream.current();
        scaleThen.launch(s, grid: [(n + 63) / 64], block: [64])(y, x, b, n);
        s.sync();
    }
}
public class D {
    /** Mismatches against the unfused pair over 100 elements. */
    public static int32 main(String[] args) {
        int32 bad = D.run();
        System.stdout.println("bad " + bad);
        return bad == 0 ? 0 : 1;
    }
    public static int32 run() {
        uint32 n = 100;
        float32[] hx = heap float32[100];
        float32[] hb = heap float32[100];
        int32 i = 0;
        while (i < 100) { hx[i] = (float32) i - 50.0f; hb[i] = 3.0f; i = i + 1; }
        KernelBuffer<float32> x = heap KernelBuffer<float32>(100);
        KernelBuffer<float32> b = heap KernelBuffer<float32>(100);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(100);
        x.upload(hx);
        b.upload(hb);
        Fused<BiasRelu> f = heap Fused<BiasRelu>();
        f.run(y, x, b, n);
        float32[] g = heap float32[100];
        y.download(g);
        int32 bad = 0;
        i = 0;
        while (i < 100) {
            float32 want = hx[i] * 2.0f + hb[i];
            if (want < 0.0f) { want = 0.0f; }
            if (g[i] != want) { bad = bad + 1; }
            i = i + 1;
        }
        return bad;
    }
}
)CAJ";
}  // namespace

// 9.1.2: bias then relu from the consumer's class, inside the library kernel, matches the
// unfused pair on every element.
TEST(XpuTemplateEpilogue, aTypeParameterEpilogueRunsInsideTheKernel) {
    Workspace w;
    w.write("src/test/D.cajeta", kProgram);
    Built b = compile(w, "cpu", "--emit=exe --xpu-backend=cpu", "test.D.main", "src");
    ASSERT_TRUE(b.ok) << b.log;
    Ran r = run(b, cajeta_env_prefix({{"CAJETA_XPU_BACKEND", "cpu"}}));
    EXPECT_EQ(r.rc, 0) << r.err;
    EXPECT_NE(r.out.find("bad 0"), std::string::npos) << r.out;
}
