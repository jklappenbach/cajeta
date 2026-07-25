//
// nucleo-expr Unit 4 — `@Fuse` on a method, and the operators that make the
// natural spelling writable (plan decision (b)).
//
// Without operators on Tensor, `(t - t.mean()) / t.std()` does not compile at
// all, so a fusion engine would have nothing NumPy-shaped to fuse. With them,
// the expression is eager by default (a fresh tensor per operator, exactly as
// numpy behaves) and `@Fuse` is precisely what collapses it into one loop —
// the spec's model, and the difference between the mechanism and the
// experience.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
float runF32(const std::string& members, const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.lang.Cajeta;\n"
        "public final class T {\n"
        + members +
        "    public static float32 run() {\n"
        "        float32[] fa = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 4;\n"
        "        Tensor<float32> x = Tensor.of<float32>(fa, s);\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    return jit->lookup<float (*)()>("run")();
}

int64_t runI64(const std::string& members, const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.lang.Cajeta;\n"
        "public final class T {\n"
        + members +
        "    public static int64 run() {\n"
        "        float32[] fa = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 4;\n"
        "        Tensor<float32> x = Tensor.of<float32>(fa, s);\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    return jit->lookup<int64_t (*)()>("run")();
}
} // namespace

// 4.2.2 — OPERATORS ARE BLOCKED, recorded with evidence. `Tensor<T>` is
// deliberately unbounded (boolean masks), operators cannot carry method-level
// type parameters (grammar), and `mul<T>` resolves to the Numeric-BOUNDED
// overload — so `a * b` cannot be declared without either bounding Tensor<T>
// (forbidding Tensor<boolean>) or a type-system change. Both are decisions
// above this unit. The explicit spelling below fuses identically; the sugar
// question is deferred, not silently dropped.
TEST(FuseSugar, explicitSpellingIsTheWritableForm) {
    EXPECT_FLOAT_EQ(runF32("",
        "Tensor<float32> r = Tensor.add<float32>(Tensor.mul<float32>(x, x), x);\n"
        "        return Tensor.sum<float32,float32>(r);"), 40.0f);
}

// 4.1.1 — `@Fuse` on a static single-return method fuses its body: calls to it
// produce the same values as the unannotated form.
TEST(FuseSugar, annotatedMethodFusesAndMatches) {
    const char* members =
        "    @Fuse\n"
        "    public static Tensor<float32> fused(Tensor<float32> t) {\n"
        "        return Tensor.add<float32>(Tensor.mul<float32>(t, t), t);\n"
        "    }\n"
        "    public static Tensor<float32> plain(Tensor<float32> t) {\n"
        "        return Tensor.add<float32>(Tensor.mul<float32>(t, t), t);\n"
        "    }\n";
    EXPECT_FLOAT_EQ(runF32(members,
        "Tensor<float32> a = T.fused(x);\n"
        "        Tensor<float32> b = T.plain(x);\n"
        "        return Tensor.sum<float32,float32>(a) - Tensor.sum<float32,float32>(b);"),
        0.0f);
}

// 4.1.2 — the sugar is only sugar: strictly less allocation than the
// unannotated eager method. MEASURED WITH allocatedBytes(), not liveCount():
// inside a method the eager intermediates are DROPPED at scope exit, so a
// live-population delta cannot see them at all. allocatedBytes is cumulative
// and monotonic, so it counts allocation EVENTS regardless of when they die —
// the right instrument for "did fusion remove the temporaries".
TEST(FuseSugar, annotatedAllocatesLikeExplicitFuseNotLikeEager) {
    const char* members =
        "    @Fuse\n"
        "    public static Tensor<float32> fused(Tensor<float32> t) {\n"
        "        return Tensor.add<float32>(Tensor.mul<float32>(t, t), t);\n"
        "    }\n"
        "    public static Tensor<float32> plain(Tensor<float32> t) {\n"
        "        return Tensor.add<float32>(Tensor.mul<float32>(t, t), t);\n"
        "    }\n";
    int64_t annotated = runI64(members,
        "int64 base = Cajeta.allocatedBytes();\n"
        "        Tensor<float32> a = T.fused(x);\n"
        "        return Cajeta.allocatedBytes() - base;");
    int64_t eager = runI64(members,
        "int64 base = Cajeta.allocatedBytes();\n"
        "        Tensor<float32> b = T.plain(x);\n"
        "        return Cajeta.allocatedBytes() - base;");
    EXPECT_LT(annotated, eager) << "annotated=" << annotated << " eager=" << eager;
}

// 4.1.3 — a body that cannot fuse raises the SAME named error the explicit
// form raises (one recognizer, one diagnostic). NOTE the sugar desugars at the
// CALL SITE (as @Grad/@Vmap/@Jit do), so the method must be CALLED for the
// transform — and therefore its diagnostic — to fire. An annotated-but-unused
// method is not transformed at all.
TEST(FuseSugar, unfusableAnnotatedBodyIsTheSameNamedError) {
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
        "        Tensor<float32> x = Tensor.of<float32>(fa, s);\n"
        "        Tensor<float32> r = T.bad(x);\n"
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
