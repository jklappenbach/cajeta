//
// nucleo-nn-optim Unit 1 — GradAll: one backward, K parameters (plan 1.1.x).
//
// `GradAll(f)` differentiates a scalar-valued f w.r.t. ALL of its parameters;
// `GradAll<K>(f)` w.r.t. the leading K (params first, data args after — the
// functional-step convention). Returns GradResult<V, G[]>: the forward value
// and the grads as an array in argument order. v1 requires the differentiated
// args to share one type spelling (all float32, or all Tensor<float32>).
//
// relu rides in this unit too (plan 1.2.3): the op did not exist before, and
// the MLP bar needs its VJP.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// Compile-error probe (the TransformDiagnostics pattern).
struct Diag {
    std::string id;
    std::string msg;
};

Diag diagOf(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "import cajeta.math.Tensor;\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    try { CajetaJit::compile(src, "test.T"); }
    catch (cajeta::Exception& e) {
        return {e.getErrorId(), e.getMessage()};
    }
    return {};
}

} // namespace

// 0 — language probe (no GradAll): an owned array of owned tensors built in a
// static helper survives the return and reads back intact. This is the exact
// carrier shape GradAll's synthesized backward uses for tensor grads; if this
// breaks, GradAll's design premise breaks, so it is pinned independently.
TEST(GradAllTests, ownedTensorArrayRoundTrips) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "public final class T {\n"
        "    static #Tensor<float32>[] make2() {\n"
        "        Tensor<float32>[] gs = heap Tensor<float32>[2];\n"
        "        int64[] shape = {2};\n"
        "        gs[0] = Tensor.full<float32>(shape, 3.0f);\n"
        "        gs[1] = Tensor.full<float32>(shape, 5.0f);\n"
        "        return gs;\n"
        "    }\n"
        "    public static float32 run() {\n"
        "        Tensor<float32>[] gs = make2();\n"
        "        return gs[0].get1(0) + gs[1].get1(1) * 10.0f;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_NEAR(fn(), 53.0f, 1e-5f);
}

// 1.1.1 — scalar smoke: f(x,y) = x*x*y at (3,4) -> value 36, grads {24, 9};
// agreement with Grad<0> and Grad<1> run separately.
TEST(GradAllTests, scalarAllArgsMatchesPerArgGrad) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class T {\n"
        "    public static float32 run(float32 xv, float32 yv) {\n"
        "        (float32, float32) -> GradResult<float32, float32[]> g =\n"
        "            GradAll((float32 x, float32 y) -> x * x * y);\n"
        "        GradResult<float32, float32[]> r = g(xv, yv);\n"
        "        (float32, float32) -> GradResult<float32,float32> g0 =\n"
        "            Grad<0>((float32 x, float32 y) -> x * x * y);\n"
        "        (float32, float32) -> GradResult<float32,float32> g1 =\n"
        "            Grad<1>((float32 x, float32 y) -> x * x * y);\n"
        "        float32 d0 = r.grads[0] - g0(xv, yv).grads;\n"
        "        float32 d1 = r.grads[1] - g1(xv, yv).grads;\n"
        "        return r.value + d0 * 1000.0f + d1 * 1000.0f;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)(float, float)>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_NEAR(fn(3.0f, 4.0f), 36.0f, 1e-4f);   // agreement deltas add 0
    EXPECT_NEAR(fn(-2.0f, 0.5f), 2.0f, 1e-4f);
}

// 1.1.1b — leading-K: f(x,y) = x*y with K=1 grades only x; grads has length 1.
TEST(GradAllTests, leadingKGradesOnlyLeadingArgs) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class T {\n"
        "    public static float32 run(float32 xv, float32 yv) {\n"
        "        (float32, float32) -> GradResult<float32, float32[]> g =\n"
        "            GradAll<1>((float32 x, float32 y) -> x * y);\n"
        "        GradResult<float32, float32[]> r = g(xv, yv);\n"
        "        return r.grads[0] * 100.0f + (float32)(r.grads.count()) + r.value;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)(float, float)>("run");
    ASSERT_NE(fn, nullptr);
    // grads[0] = y = 5 -> 500; count = 1; value = 15  => 516
    EXPECT_NEAR(fn(3.0f, 5.0f), 516.0f, 1e-4f);
}

// 1.1.2 — tensor K-arg, the Linear shape: f(w, b, x) = sum(x·w + b), K=2.
// Grads agree elementwise with per-arg Grad<N> references.
TEST(GradAllTests, tensorLeadingKMatchesPerArgGrad) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        "        int64[] s22 = {2, 2};\n"
        "        Tensor<float32> w = Tensor.full<float32>(s22, 0.5f);\n"
        "        Tensor<float32> b = Tensor.full<float32>(s22, 0.1f);\n"
        "        Tensor<float32> x = Tensor.full<float32>(s22, 2.0f);\n"
        "        (Tensor<float32>, Tensor<float32>, Tensor<float32>)\n"
        "                -> GradResult<float32, Tensor<float32>[]> g =\n"
        "            GradAll<2>((Tensor<float32> wp, Tensor<float32> bp,\n"
        "                        Tensor<float32> xp) ->\n"
        "                Tensor.sum<float32,float32>(\n"
        "                    Tensor.add<float32>(Tensor.matmul<float32>(xp, wp), bp)));\n"
        "        GradResult<float32, Tensor<float32>[]> r = g(w, b, x);\n"
        "        (Tensor<float32>, Tensor<float32>, Tensor<float32>)\n"
        "                -> GradResult<float32, Tensor<float32>> g0 =\n"
        "            Grad<0>((Tensor<float32> wp, Tensor<float32> bp,\n"
        "                     Tensor<float32> xp) ->\n"
        "                Tensor.sum<float32,float32>(\n"
        "                    Tensor.add<float32>(Tensor.matmul<float32>(xp, wp), bp)));\n"
        "        (Tensor<float32>, Tensor<float32>, Tensor<float32>)\n"
        "                -> GradResult<float32, Tensor<float32>> g1 =\n"
        "            Grad<1>((Tensor<float32> wp, Tensor<float32> bp,\n"
        "                     Tensor<float32> xp) ->\n"
        "                Tensor.sum<float32,float32>(\n"
        "                    Tensor.add<float32>(Tensor.matmul<float32>(xp, wp), bp)));\n"
        "        Tensor<float32> rw = g0(w, b, x).grads;\n"
        "        Tensor<float32> rb = g1(w, b, x).grads;\n"
        "        float32 dw = Tensor.sum<float32,float32>(\n"
        "            Tensor.sub<float32>(r.grads[0], rw));\n"
        "        float32 db = Tensor.sum<float32,float32>(\n"
        "            Tensor.sub<float32>(r.grads[1], rb));\n"
        "        return r.value + dw * 1000.0f + db * 1000.0f\n"
        "            + (float32)(r.grads.count()) * 0.125f;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    // value: each of 4 output cells = 2*0.5*2 (matmul row) + 0.1 = 2.1 -> sum 8.4;
    // deltas 0; count 2 -> +0.25.
    EXPECT_NEAR(fn(), 8.65f, 1e-3f);
}

// 1.1.4 — named, located errors: K out of range and mixed leading types.
TEST(GradAllTests, namedErrors) {
    Diag zero = diagOf(
        "(float32, float32) -> GradResult<float32, float32[]> g =\n"
        "            GradAll<0>((float32 x, float32 y) -> x * y);\n"
        "        return g(1.0f, 2.0f).value;");
    EXPECT_EQ(zero.id, "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY") << zero.msg;
    EXPECT_NE(zero.msg.find("GradAll"), std::string::npos) << zero.msg;

    Diag over = diagOf(
        "(float32, float32) -> GradResult<float32, float32[]> g =\n"
        "            GradAll<3>((float32 x, float32 y) -> x * y);\n"
        "        return g(1.0f, 2.0f).value;");
    EXPECT_EQ(over.id, "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY") << over.msg;

    Diag mixed = diagOf(
        "int64[] s = {2};\n"
        "        (Tensor<float32>, float32) -> GradResult<float32, float32[]> g =\n"
        "            GradAll((Tensor<float32> t, float32 y) ->\n"
        "                Tensor.sum<float32,float32>(t) * y);\n"
        "        Tensor<float32> tt = Tensor.full<float32>(s, 1.0f);\n"
        "        return g(tt, 2.0f).value;");
    EXPECT_EQ(mixed.id, "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY") << mixed.msg;
    EXPECT_NE(mixed.msg.find("same type"), std::string::npos) << mixed.msg;
}

// 1.1.3 — the one-call cost pin: a single GradAll<2> call costs no more than
// the per-arg alternative (Grad<0> + Grad<1>) plus the grads ARRAY itself,
// measured by live-set delta (the FuseAllocationTests idiom). Tier-A inlining
// re-emits shared forward subexpressions textually per grad, so strict
// op-sharing is NOT claimed (the SSA-locals emission that would share them is
// recorded backlog, plan 8.2.2); this pins the no-worse-than-naive bound.
TEST(GradAllTests, oneCallAllocatesLessThanPerArgPair) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.lang.Cajeta;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class T {\n"
        "    public static int64 run() {\n"
        "        int64[] s = {2, 2};\n"
        "        Tensor<float32> a = Tensor.full<float32>(s, 0.5f);\n"
        "        Tensor<float32> b = Tensor.full<float32>(s, 2.0f);\n"
        "        (Tensor<float32>, Tensor<float32>)\n"
        "                -> GradResult<float32, Tensor<float32>[]> g =\n"
        "            GradAll((Tensor<float32> u, Tensor<float32> v) ->\n"
        "                Tensor.sum<float32,float32>(Tensor.mul<float32>(u, v)));\n"
        "        (Tensor<float32>, Tensor<float32>)\n"
        "                -> GradResult<float32, Tensor<float32>> g0 =\n"
        "            Grad<0>((Tensor<float32> u, Tensor<float32> v) ->\n"
        "                Tensor.sum<float32,float32>(Tensor.mul<float32>(u, v)));\n"
        "        (Tensor<float32>, Tensor<float32>)\n"
        "                -> GradResult<float32, Tensor<float32>> g1 =\n"
        "            Grad<1>((Tensor<float32> u, Tensor<float32> v) ->\n"
        "                Tensor.sum<float32,float32>(Tensor.mul<float32>(u, v)));\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        GradResult<float32, Tensor<float32>[]> r = g(a, b);\n"
        "        int64 all = Cajeta.liveCount() - base;\n"
        "        base = Cajeta.liveCount();\n"
        "        GradResult<float32, Tensor<float32>> r0 = g0(a, b);\n"
        "        GradResult<float32, Tensor<float32>> r1 = g1(a, b);\n"
        "        int64 pair = Cajeta.liveCount() - base;\n"
        "        if (all <= pair + 1) { return 777; }\n"
        "        return all * 1000 + pair;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 777);
}

// 1.1.5 — composition: Jit(GradAll<K>(f)) fuses and agrees with the unfused
// form; a @NoGrad helper inside f is a constant (its path's grad is zero).
TEST(GradAllTests, jitCompositionAndNoGrad) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class T {\n"
        "    @NoGrad\n"
        "    static float32 scale(float32 v) { return v * 3.0f; }\n"
        "    public static float32 run(float32 xv, float32 yv) {\n"
        "        (float32, float32) -> GradResult<float32, float32[]> g =\n"
        "            Jit(GradAll((float32 x, float32 y) -> x * x + scale(y)));\n"
        "        GradResult<float32, float32[]> r = g(xv, yv);\n"
        "        // value = x^2 + 3y; d/dx = 2x; d/dy = 0 (@NoGrad path)\n"
        "        return r.value + r.grads[0] * 100.0f + r.grads[1] * 10000.0f;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)(float, float)>("run");
    ASSERT_NE(fn, nullptr);
    // x=3, y=2: value 9+6=15, dx=6 -> +600, dy=0 -> 615
    EXPECT_NEAR(fn(3.0f, 2.0f), 615.0f, 1e-3f);
}

// 1.2.3 — Tensor.relu forward + its VJP rule: relu(x) elementwise max(x, 0);
// d/dx sum(relu(x)) = 1 where x > 0 else 0.
TEST(GradAllTests, reluForwardAndVjp) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        "        int64[] s = {4};\n"
        "        Tensor<float32> x = Tensor.zeros<float32>(s);\n"
        "        x.set1(0, -2.0f);\n"
        "        x.set1(1, -0.5f);\n"
        "        x.set1(2, 0.5f);\n"
        "        x.set1(3, 3.0f);\n"
        "        Tensor<float32> y = Tensor.relu<float32>(x);\n"
        "        float32 fwd = y.get1(0) * 1000.0f + y.get1(1) * 100.0f\n"
        "            + y.get1(2) * 10.0f + y.get1(3);\n"
        "        (Tensor<float32>) -> GradResult<float32, Tensor<float32>> g =\n"
        "            Grad((Tensor<float32> t) ->\n"
        "                Tensor.sum<float32,float32>(Tensor.relu<float32>(t)));\n"
        "        GradResult<float32, Tensor<float32>> r = g(x);\n"
        "        float32 gv = r.grads.get1(0) * 1000.0f + r.grads.get1(1) * 100.0f\n"
        "            + r.grads.get1(2) * 10.0f + r.grads.get1(3);\n"
        "        return fwd + gv * 0.001f;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    // fwd: {0, 0, 0.5, 3} -> 0*1000 + 0*100 + 5 + 3 = 8
    // grads: {0, 0, 1, 1} -> (0 + 0 + 10 + 1) * 0.001 = 0.011
    EXPECT_NEAR(fn(), 8.011f, 1e-4f);
}
