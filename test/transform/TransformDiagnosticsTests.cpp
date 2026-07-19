//
// transform-intrinsics Unit 8 — the diagnostics acceptance bar (spec §10).
// Every transform failure mode must be a NAMED, LOCATED error that points at
// the offending op/primitive/signature — never a generic failure. This file
// aggregates the per-unit error shapes into one bar (8.1.1) and adds the
// statically-known rank/dtype reporting (8.1.2: shape DIMS are not in the type
// system, so "statically known" v1 = rank-kind tensor/scalar + element dtype).
//
// NO_VJP_RULE is deliberately absent: the registry covers the entire DAG
// primitive set (add/mul/sub/negate/matmul/sum), so that error is unreachable
// from source today — reaching it requires desyncing the registry from the DAG
// builder, which is a compiler bug, not a user program.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

struct Diag {
    std::string id;
    std::string msg;
    bool located = false;
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
        return {e.getErrorId(), e.getMessage(), e.hasLocation()};
    }
    return {};
}

} // namespace

// 8.1.1 — an unsupported op in a differentiated body names the OPERATOR.
TEST(TransformDiagnostics, unsupportedOpNamedAndLocated) {
    Diag d = diagOf(
        "(float32) -> GradResult<float32,float32> g = Grad((float32 x) -> x / x);\n"
        "        return g(2.0f).value;");
    EXPECT_EQ(d.id, "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
    EXPECT_TRUE(d.located) << d.msg;
    EXPECT_NE(d.msg.find("'/'"), std::string::npos)
        << "must name the offending operator, got: " << d.msg;
}

// 8.1.1 — an op with no batching rule under Vmap names the OPERATOR too (the
// generic "this body" text is not pointing at anything).
TEST(TransformDiagnostics, noBatchRuleNamedAndLocated) {
    Diag d = diagOf(
        "float32[] xs = {1.0f, 2.0f};\n"
        "        (float32[]) -> #float32[] g = Vmap((float32 x) -> x / x);\n"
        "        float32[] ys = g(xs);\n"
        "        return ys[0];");
    EXPECT_EQ(d.id, "CAJETA_ERROR_TRANSFORM_NO_BATCH_RULE");
    EXPECT_TRUE(d.located) << d.msg;
    EXPECT_NE(d.msg.find("'/'"), std::string::npos)
        << "must name the offending operator, got: " << d.msg;
}

// 8.1.1 — a non-specializable target is named and located.
TEST(TransformDiagnostics, notSpecializableLocated) {
    std::string src =
        "package test;\n"
        "public final class T {\n"
        "    public static float32 viaParam((float32) -> float32 f) {\n"
        "        (float32) -> float32 g = Jit(f);\n"
        "        return g(3.0f);\n"
        "    }\n"
        "    public static float32 run() { return T.viaParam((float32 x) -> x); }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.T");
        FAIL() << "expected the non-specializable target to fail the compile";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_TRANSFORM_NOT_SPECIALIZABLE");
        EXPECT_TRUE(e.hasLocation()) << e.getMessage();
    }
}

// 8.1.1 — the wrong-return-type shape: Grad of a tensor-valued body is named
// (scalar-valued requirement) and located.
TEST(TransformDiagnostics, wrongReturnTypeLocated) {
    Diag d = diagOf(
        "float32[] fa = { 1.0f, 2.0f };\n"
        "        int64[] s = heap int64[1]; s[0] = 2;\n"
        "        Tensor<float32> x = Tensor.of<float32>(fa, s);\n"
        "        (Tensor<float32>) -> GradResult<float32, Tensor<float32>> g =\n"
        "            Grad((Tensor<float32> xx) -> Tensor.mul<float32>(xx, xx));\n"
        "        GradResult<float32, Tensor<float32>> r = g(x);\n"
        "        return r.value;");
    EXPECT_EQ(d.id, "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
    EXPECT_TRUE(d.located) << d.msg;
    EXPECT_NE(d.msg.find("scalar-valued"), std::string::npos) << d.msg;
}

// 8.1.2 — a rank mismatch the backward would build is reported WITH the
// statically-known ranks and dtype: matmul over a tensor and a scalar.
TEST(TransformDiagnostics, rankMismatchReportsRanksAndDtype) {
    Diag d = diagOf(
        "float32[] fa = { 1.0f, 2.0f };\n"
        "        int64[] s = heap int64[1]; s[0] = 2;\n"
        "        Tensor<float32> x = Tensor.of<float32>(fa, s);\n"
        "        (Tensor<float32>, float32) -> GradResult<float32, Tensor<float32>> g =\n"
        "            Grad((Tensor<float32> xx, float32 k) ->\n"
        "                Tensor.sum<float32,float32>(Tensor.matmul<float32>(xx, k)));\n"
        "        GradResult<float32, Tensor<float32>> r = g(x, 2.0f);\n"
        "        return r.value;");
    EXPECT_EQ(d.id, "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
    EXPECT_TRUE(d.located) << d.msg;
    EXPECT_NE(d.msg.find("matmul"), std::string::npos) << d.msg;
    EXPECT_NE(d.msg.find("tensor"), std::string::npos) << d.msg;
    EXPECT_NE(d.msg.find("scalar"), std::string::npos) << d.msg;
    EXPECT_NE(d.msg.find("float32"), std::string::npos)
        << "must carry the statically-known dtype, got: " << d.msg;
}
