//
// nucleo-expr Units 3+4, folded (test-battery-restructure 2.1).
//
// Unit 3 — THE headline acceptance criterion (spec §3.1, §3.2, §9): "a
// multi-operator elementwise tensor expression executes as one kernel and
// allocates NO intermediate buffers (only the final result) — demonstrable by
// allocation count." So it is demonstrated by allocation count, not by
// inspecting IR: every measurement diffs Cajeta.liveCount() across the
// evaluation (the DropGapTests idiom), with the eager form of the SAME
// arithmetic as the control — the delta between the two IS the fusion win.
//
// Unit 4 — `@Fuse` on a method. Operators on Tensor are BLOCKED, recorded with
// evidence: `Tensor<T>` is deliberately unbounded (boolean masks), operators
// cannot carry method-level type parameters (grammar), and `mul<T>` resolves
// to the Numeric-BOUNDED overload — so `a * b` cannot be declared without
// either bounding Tensor<T> (forbidding Tensor<boolean>) or a type-system
// change, both decisions above this unit. The explicit spelling fuses
// identically; the sugar question is deferred, not silently dropped.
//
// Fold shape: the nine original single-claim tests compiled thirteen separate
// programs whose coverage sets were measured byte-identical or strict subsets
// of each other (.coverage/index, 2026-08-10). One program now carries every
// measurement as its own entry point — each builds its own tensor and returns
// one delta, so measurements cannot interfere — and one C++ test asserts the
// relations between them. The unfusable-body diagnostic keeps its own test:
// it needs a compile that FAILS.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
// The sugar-vs-eager byte measurements use allocatedBytes(), not liveCount():
// inside a method the eager intermediates are DROPPED at scope exit, so a
// live-population delta cannot see them at all. allocatedBytes is cumulative
// and monotonic, so it counts allocation EVENTS regardless of when they die —
// the right instrument for "did fusion remove the temporaries". Both bytes
// entry points warm their path once before taking the baseline, so one-time
// transform costs land outside the measurement symmetrically.
const char* kFuseMatrixSrc =
    "package test;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.lang.Cajeta;\n"
    "public final class T {\n"
    "    @Fuse\n"
    "    public static Tensor<float32> sugared(Tensor<float32> t) {\n"
    "        return Tensor.add<float32>(Tensor.mul<float32>(t, t), t);\n"
    "    }\n"
    "    public static Tensor<float32> unsugared(Tensor<float32> t) {\n"
    "        return Tensor.add<float32>(Tensor.mul<float32>(t, t), t);\n"
    "    }\n"
    // 3.1.1 control — the eager 4-op chain ((x*x + x) - x) * x: one live
    // object PER OPERATOR survives. The problem statement, measured.
    "    public static int64 eagerChainDelta() {\n"
    "        float32[] fa = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
    "        int64[] s = heap int64[1]; s[0] = 4;\n"
    "        Tensor<float32> x #= Tensor.of<float32>(fa, s);\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        Tensor<float32> a #= Tensor.mul<float32>(x, x);\n"
    "        Tensor<float32> b #= Tensor.add<float32>(a, x);\n"
    "        Tensor<float32> c #= Tensor.sub<float32>(b, x);\n"
    "        Tensor<float32> r #= Tensor.mul<float32>(c, x);\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"
    // 3.1.1 claim — the SAME arithmetic fused.
    "    public static int64 fusedChain4Delta() {\n"
    "        float32[] fa = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
    "        int64[] s = heap int64[1]; s[0] = 4;\n"
    "        Tensor<float32> x #= Tensor.of<float32>(fa, s);\n"
    "        (Tensor<float32>) -> #Tensor<float32> g =\n"
    "            Fuse((Tensor<float32> t) -> Tensor.mul<float32>(\n"
    "                Tensor.sub<float32>(\n"
    "                    Tensor.add<float32>(Tensor.mul<float32>(t, t), t), t), t));\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        Tensor<float32> r #= g(x);\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"
    // 3.1.2 — the same shape at twice the operators.
    "    public static int64 fusedChain8Delta() {\n"
    "        float32[] fa = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
    "        int64[] s = heap int64[1]; s[0] = 4;\n"
    "        Tensor<float32> x #= Tensor.of<float32>(fa, s);\n"
    "        (Tensor<float32>) -> #Tensor<float32> g =\n"
    "            Fuse((Tensor<float32> t) -> Tensor.mul<float32>(Tensor.sub<float32>(\n"
    "                Tensor.add<float32>(Tensor.mul<float32>(Tensor.sub<float32>(\n"
    "                    Tensor.add<float32>(Tensor.mul<float32>(t, t), t), t), t), t),\n"
    "                t), t));\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        Tensor<float32> r #= g(x);\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"
    // 3.1.3 — `t*t` feeds both operands of the outer multiply; DAG leaf/CSE
    // dedup means the fused body contains it once.
    "    public static int64 sharedSubExprDelta() {\n"
    "        float32[] fa = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
    "        int64[] s = heap int64[1]; s[0] = 4;\n"
    "        Tensor<float32> x #= Tensor.of<float32>(fa, s);\n"
    "        (Tensor<float32>) -> #Tensor<float32> g =\n"
    "            Fuse((Tensor<float32> t) -> Tensor.mul<float32>(\n"
    "                Tensor.mul<float32>(t, t), Tensor.mul<float32>(t, t)));\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        Tensor<float32> r #= g(x);\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"
    // 3.1.1 (reduction root) — the elementwise body fuses into the
    // accumulation; the result is a scalar, no tensor at all.
    "    public static int64 reductionRootDelta() {\n"
    "        float32[] fa = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
    "        int64[] s = heap int64[1]; s[0] = 4;\n"
    "        Tensor<float32> x #= Tensor.of<float32>(fa, s);\n"
    "        (Tensor<float32>) -> float32 g =\n"
    "            Fuse((Tensor<float32> t) ->\n"
    "                Tensor.sum<float32,float32>(Tensor.mul<float32>(t, t)));\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        float32 v = g(x);\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"
    // 4.2.2 — the explicit spelling is the writable form (and computes right).
    "    public static float32 explicitSum() {\n"
    "        float32[] fa = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
    "        int64[] s = heap int64[1]; s[0] = 4;\n"
    "        Tensor<float32> x #= Tensor.of<float32>(fa, s);\n"
    "        Tensor<float32> r #= Tensor.add<float32>(Tensor.mul<float32>(x, x), x);\n"
    "        return Tensor.sum<float32,float32>(r);\n"
    "    }\n"
    // 4.1.1 — the annotated method produces the same values as the plain one.
    "    public static float32 sugarValueDiff() {\n"
    "        float32[] fa = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
    "        int64[] s = heap int64[1]; s[0] = 4;\n"
    "        Tensor<float32> x #= Tensor.of<float32>(fa, s);\n"
    "        Tensor<float32> a #= T.sugared(x);\n"
    "        Tensor<float32> b #= T.unsugared(x);\n"
    "        return Tensor.sum<float32,float32>(a) - Tensor.sum<float32,float32>(b);\n"
    "    }\n"
    // 4.1.2 — allocation events on the sugared path (warmed).
    "    public static int64 sugarFusedBytes() {\n"
    "        float32[] fa = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
    "        int64[] s = heap int64[1]; s[0] = 4;\n"
    "        Tensor<float32> x #= Tensor.of<float32>(fa, s);\n"
    "        Tensor<float32> w = T.sugared(x);\n"
    "        int64 base = Cajeta.allocatedBytes();\n"
    "        Tensor<float32> a #= T.sugared(x);\n"
    "        return Cajeta.allocatedBytes() - base;\n"
    "    }\n"
    // 4.1.2 — allocation events on the eager path (warmed identically).
    "    public static int64 sugarEagerBytes() {\n"
    "        float32[] fa = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
    "        int64[] s = heap int64[1]; s[0] = 4;\n"
    "        Tensor<float32> x #= Tensor.of<float32>(fa, s);\n"
    "        Tensor<float32> w = T.unsugared(x);\n"
    "        int64 base = Cajeta.allocatedBytes();\n"
    "        Tensor<float32> b #= T.unsugared(x);\n"
    "        return Cajeta.allocatedBytes() - base;\n"
    "    }\n"
    "}\n";
} // namespace

// The Fuse use-case matrix, one compiled program (was: FuseAllocation.{eager
// ChainAllocatesPerOperator, fusedChainAllocatesFewerThanEager, allocationIs
// IndependentOfChainLength, sharedSubExpressionCostsNothingExtra, reduction
// RootAllocatesNoTensor} + FuseSugar.{explicitSpellingIsTheWritableForm,
// annotatedMethodFusesAndMatches, annotatedAllocatesLikeExplicitFuseNotLike
// Eager} — thirteen compiles' worth of claims, every assertion preserved):
//   eager allocates per operator; fused < eager; allocation independent of
//   chain length; shared sub-expressions free; reduction root allocates no
//   tensor; explicit spelling computes; @Fuse matches in value; @Fuse
//   allocates like explicit Fuse, not like eager.
TEST(Fuse, allocationAndSugarMatrix) {
    auto jit = CajetaJit::compile(kFuseMatrixSrc, "test.T");
    ASSERT_NE(jit, nullptr);
    auto eagerChainDelta   = jit->lookup<int64_t (*)()>("eagerChainDelta");
    auto fusedChain4Delta  = jit->lookup<int64_t (*)()>("fusedChain4Delta");
    auto fusedChain8Delta  = jit->lookup<int64_t (*)()>("fusedChain8Delta");
    auto sharedSubExprDelta = jit->lookup<int64_t (*)()>("sharedSubExprDelta");
    auto reductionRootDelta = jit->lookup<int64_t (*)()>("reductionRootDelta");
    auto explicitSum       = jit->lookup<float (*)()>("explicitSum");
    auto sugarValueDiff    = jit->lookup<float (*)()>("sugarValueDiff");
    auto sugarFusedBytes   = jit->lookup<int64_t (*)()>("sugarFusedBytes");
    auto sugarEagerBytes   = jit->lookup<int64_t (*)()>("sugarEagerBytes");
    ASSERT_NE(eagerChainDelta, nullptr);
    ASSERT_NE(fusedChain4Delta, nullptr);
    ASSERT_NE(fusedChain8Delta, nullptr);
    ASSERT_NE(sharedSubExprDelta, nullptr);
    ASSERT_NE(reductionRootDelta, nullptr);
    ASSERT_NE(explicitSum, nullptr);
    ASSERT_NE(sugarValueDiff, nullptr);
    ASSERT_NE(sugarFusedBytes, nullptr);
    ASSERT_NE(sugarEagerBytes, nullptr);

    // 3.1.1 — control, then claim.
    int64_t eager = eagerChainDelta();
    int64_t fused4 = fusedChain4Delta();
    EXPECT_GE(eager, 4);
    EXPECT_LT(fused4, eager) << "fused=" << fused4 << " eager=" << eager;

    // 3.1.2 — doubling the operator count does not change the allocation
    // count. Eager would double; fused is flat. This is the property that
    // makes it fusion rather than a constant-factor saving.
    EXPECT_EQ(fused4, fusedChain8Delta());

    // 3.1.3 — sharing costs nothing extra.
    EXPECT_EQ(sharedSubExprDelta(), fused4);

    // 3.1.1 (reduction root) — no tensor at all.
    EXPECT_EQ(reductionRootDelta(), 0);

    // 4.2.2 — the explicit spelling computes: sum((x*x)+x) over [1..4] = 40.
    EXPECT_FLOAT_EQ(explicitSum(), 40.0f);

    // 4.1.1 — sugar matches in value.
    EXPECT_FLOAT_EQ(sugarValueDiff(), 0.0f);

    // 4.1.2 — sugar is only sugar: strictly fewer allocation events.
    int64_t sugarBytes = sugarFusedBytes();
    int64_t eagerBytes = sugarEagerBytes();
    EXPECT_LT(sugarBytes, eagerBytes)
        << "annotated=" << sugarBytes << " eager=" << eagerBytes;
}

// 4.1.3 — a body that cannot fuse raises the SAME named error the explicit
// form raises (one recognizer, one diagnostic). NOTE the sugar desugars at the
// CALL SITE (as @Grad/@Vmap/@Jit do), so the method must be CALLED for the
// transform — and therefore its diagnostic — to fire. An annotated-but-unused
// method is not transformed at all. Its own test because the compile FAILS.
TEST(Fuse, unfusableAnnotatedBodyIsTheSameNamedError) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "public final class T {\n"
        "    @Fuse\n"
        "    public static Tensor<float32> bad(Tensor<float32> t) {\n"
        "        return Tensor.matmul<float32>(t, t);\n"
        "    }\n"
        "    public static float32 run() {\n"
        "        float32[] fa = [ 1.0f, 2.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 2;\n"
        "        Tensor<float32> x #= Tensor.of<float32>(fa, s);\n"
        "        Tensor<float32> r #= T.bad(x);\n"
        "        return 0.0f;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.T");
        FAIL() << "expected an unfusable @Fuse body to fail the compile";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_TRANSFORM_NOT_FUSIBLE")
            << e.getMessage();
    }
}
