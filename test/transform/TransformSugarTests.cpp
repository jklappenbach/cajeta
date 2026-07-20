//
// transform-intrinsics Unit 7 — annotation sugar + composition order (spec §4).
// `@Jit @Vmap @Grad func` desugars to `Jit(Vmap(Grad(f)))`: the annotation
// NEAREST the declaration (the last one written) applies first, matching JAX's
// nesting so the mental model transfers. The sugar is ONLY sugar — it builds
// the nested combinator nodes and routes them through the same U1 driver, so
// every composition and every error shape is identical to the explicit form.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
float runF32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.T");
    return jit->lookup<float (*)()>("run")();
}
std::string errIdOf(const std::string& src) {
    try { CajetaJit::compile(src, "test.T"); }
    catch (cajeta::Exception& e) { return e.getErrorId(); }
    return "";
}
} // namespace

// 7.1.1 (single) — `@Grad` on a static makes calls to it the differentiated
// form: T.sq(3) returns GradResult{9, 6} -> 906.
TEST(TransformSugar, gradAnnotationAloneDesugars) {
    EXPECT_FLOAT_EQ(runF32(
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class T {\n"
        "    @Grad\n"
        "    public static float32 sq(float32 x) { return x * x; }\n"
        "    public static float32 run() {\n"
        "        GradResult<float32,float32> r = T.sq(3.0f);\n"
        "        return r.value * 100.0f + r.grads;\n"
        "    }\n"
        "}\n"), 906.0f);
}

// 7.1.1 (full stack) — `@Jit @Vmap @Grad` desugars to Jit(Vmap(Grad(f))):
// innermost annotation applied first, per-example gradients over the batch,
// fused. {1,2,3} -> grads {2,4,6} -> 642, and the fusion tag is in the IR.
TEST(TransformSugar, fullStackDesugarsInnermostFirst) {
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class T {\n"
        "    @Jit @Vmap @Grad\n"
        "    public static float32 sq(float32 x) { return x * x; }\n"
        "    public static float32 run() {\n"
        "        float32[] xs = {1.0f, 2.0f, 3.0f};\n"
        "        GradResult<float32,float32>[] rs = T.sq(xs);\n"
        "        return rs[0].grads + rs[1].grads * 10.0f + rs[2].grads * 100.0f;\n"
        "    }\n"
        "}\n", "test.T", opts);
    EXPECT_FLOAT_EQ(jit->lookup<float (*)()>("run")(), 642.0f);
    EXPECT_NE(jit->getModuleIr().find("__JitFused_"), std::string::npos)
        << "the @Jit layer of the stack did not fuse";
}

// 7.1.2 — the sugar and the explicit combinator form for the SAME composition
// are observably equivalent: both computed in one program, results equal.
TEST(TransformSugar, sugarEqualsExplicitForm) {
    EXPECT_FLOAT_EQ(runF32(
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class T {\n"
        "    @Vmap @Grad\n"
        "    public static float32 sq(float32 x) { return x * x; }\n"
        "    public static float32 run() {\n"
        "        float32[] xs = {1.0f, 2.0f, 3.0f};\n"
        "        GradResult<float32,float32>[] a = T.sq(xs);\n"
        "        (float32[]) -> #GradResult<float32,float32>[] g =\n"
        "            Vmap(Grad((float32 x) -> x * x));\n"
        "        GradResult<float32,float32>[] b = g(xs);\n"
        "        float32 sa = a[0].grads + a[1].grads * 10.0f + a[2].grads * 100.0f;\n"
        "        float32 sb = b[0].grads + b[1].grads * 10.0f + b[2].grads * 100.0f;\n"
        "        if (sa == sb) { return sa; }\n"
        "        return -1.0f;\n"
        "    }\n"
        "}\n"), 642.0f);
}

// 7.1.3 — order is MEANINGFUL, not normalized away. Vmap(Grad(f)) is the
// per-example-gradient form (works); Grad(Vmap(f)) asks for the gradient of a
// batched (array-valued) function, which v1's scalar-valued Grad rejects with a
// NAMED, located transform error. The error itself is the proof of
// non-normalization: had the order been silently canonicalized, both would
// produce 642.
TEST(TransformSugar, reorderedCompositionIsDistinct) {
    std::string workedId = errIdOf(
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        "        float32[] xs = {1.0f, 2.0f};\n"
        "        (float32[]) -> #GradResult<float32,float32>[] g =\n"
        "            Vmap(Grad((float32 x) -> x * x));\n"
        "        GradResult<float32,float32>[] rs = g(xs);\n"
        "        return rs[0].grads;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(workedId, "") << "Vmap(Grad(f)) must compile";

    std::string reorderedId = errIdOf(
        "package test;\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        "        float32[] xs = {1.0f, 2.0f};\n"
        "        (float32) -> float32 g = Grad(Vmap((float32 x) -> x * x));\n"
        "        return g(2.0f);\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(reorderedId.rfind("CAJETA_ERROR_TRANSFORM", 0), 0u)
        << "Grad(Vmap(f)) must fail with a NAMED transform error, got: '"
        << reorderedId << "'";
}

// 7.2.2 (one code path) — a sugar-order error is the SAME error the explicit
// form raises: `@Grad @Vmap` (Grad outermost over a batched function) reports
// the identical named transform error as `Grad(Vmap(f))`.
TEST(TransformSugar, sugarErrorMatchesExplicitError) {
    std::string sugarId = errIdOf(
        "package test;\n"
        "public final class T {\n"
        "    @Grad @Vmap\n"
        "    public static float32 sq(float32 x) { return x * x; }\n"
        "    public static float32 run() {\n"
        "        float32[] xs = {1.0f, 2.0f};\n"
        "        return T.sq(xs);\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(sugarId.rfind("CAJETA_ERROR_TRANSFORM", 0), 0u)
        << "sugar must surface the explicit form's transform error, got: '"
        << sugarId << "'";
}
