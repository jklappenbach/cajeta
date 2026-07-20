//
// nucleo-expr Unit 5 — the autograd seam (spec §8.1/§8.2; plan X8).
//
// `Fuse` and `Grad` are two consumers of the SAME forward DAG (`buildDag`):
// Fuse lowers it to one element loop, Grad reverse-composes the VJP rules over
// it. `Grad(Fuse(f))` therefore differentiates the fused expression by handing
// Grad the same lambda body the fuser walked — no graph translation layer, no
// second recognizer path. The tests pin the three spec claims: the gradient
// matches the unfused form (8.1), the backward is ordinary IR that Jit can
// fuse (8.2), and an undifferentiated fused expression pays nothing for
// autograd's existence (8.4).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
// The body text of the first `define` whose signature contains `fnNeedle`
// (the JitFuseTests idiom). "" when absent.
std::string bodyOf(const std::string& ir, const std::string& fnNeedle) {
    size_t d = ir.find("define ");
    while (d != std::string::npos) {
        size_t nl = ir.find('\n', d);
        size_t end = ir.find("\n}", d);
        if (nl == std::string::npos || end == std::string::npos) break;
        if (ir.substr(d, nl - d).find(fnNeedle) != std::string::npos) {
            return ir.substr(d, end - d);
        }
        d = ir.find("define ", end);
    }
    return "";
}

// x = {1,2,3}; loss(x) = sum(x*x) = 14; grad = 2x -> sum = 12; value+sum = 26.
const char* kPrelude =
    "        float32[] fa = { 1.0f, 2.0f, 3.0f };\n"
    "        int64[] s = heap int64[1]; s[0] = 3;\n"
    "        Tensor<float32> x = Tensor.of<float32>(fa, s);\n";

std::string wrap(const std::string& members, const std::string& body) {
    return
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class T {\n"
        + members +
        "    public static float32 run() {\n"
        + kPrelude
        + "        " + body + "\n"
        "    }\n"
        "}\n";
}
} // namespace

// 5.1.1 — Grad over a fused expression differentiates it: same DAG, two
// consumers. loss(x) = sum(x*x): value 14, grad 2x, 14 + 12 = 26.
TEST(FuseGrad, gradOverFusedSumOfSquares) {
    auto jit = CajetaJit::compile(wrap("",
        "(Tensor<float32>) -> GradResult<float32, Tensor<float32>> g =\n"
        "            Grad(Fuse((Tensor<float32> t) ->\n"
        "                Tensor.sum<float32,float32>(Tensor.mul<float32>(t, t))));\n"
        "        GradResult<float32, Tensor<float32>> r = g(x);\n"
        "        return r.value + Tensor.sum<float32,float32>(r.grads);"), "test.T");
    EXPECT_FLOAT_EQ(jit->lookup<float (*)()>("run")(), 26.0f);
}

// 5.1.1 — the fused form's gradient MATCHES the unfused form's gradient, both
// computed in one program over the same input (the equivalence, not just the
// absolute value).
TEST(FuseGrad, gradOverFusedMatchesUnfusedForm) {
    auto jit = CajetaJit::compile(wrap("",
        "(Tensor<float32>) -> GradResult<float32, Tensor<float32>> gf =\n"
        "            Grad(Fuse((Tensor<float32> t) ->\n"
        "                Tensor.sum<float32,float32>(Tensor.mul<float32>(t, t))));\n"
        "        (Tensor<float32>) -> GradResult<float32, Tensor<float32>> gu =\n"
        "            Grad((Tensor<float32> t) ->\n"
        "                Tensor.sum<float32,float32>(Tensor.mul<float32>(t, t)));\n"
        "        GradResult<float32, Tensor<float32>> a = gf(x);\n"
        "        GradResult<float32, Tensor<float32>> b = gu(x);\n"
        "        return (a.value - b.value)\n"
        "             + Tensor.sum<float32,float32>(Tensor.sub<float32>(a.grads, b.grads));"),
        "test.T");
    EXPECT_FLOAT_EQ(jit->lookup<float (*)()>("run")(), 0.0f);
}

// 5.1.2 — the backward of a fused expression is itself ordinary IR: Jit fuses
// it through the standard pipeline (the U6 __JitFused_ probe shape) and the
// gradient survives unchanged.
TEST(FuseGrad, backwardOfFusedIsOrdinaryIr) {
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(wrap("",
        "(Tensor<float32>) -> GradResult<float32, Tensor<float32>> g =\n"
        "            Jit(Grad(Fuse((Tensor<float32> t) ->\n"
        "                Tensor.sum<float32,float32>(Tensor.mul<float32>(t, t)))));\n"
        "        GradResult<float32, Tensor<float32>> r = g(x);\n"
        "        return r.value + Tensor.sum<float32,float32>(r.grads);"), "test.T", opts);
    EXPECT_FLOAT_EQ(jit->lookup<float (*)()>("run")(), 26.0f);
    EXPECT_NE(jit->getModuleIr().find("__JitFused_"), std::string::npos)
        << "the fused expression's backward did not go through the ordinary "
           "optimization pipeline";
}

// 5.1.3 — a NON-differentiated fused expression carries no autograd overhead
// (spec 8.4): nothing autograd-shaped — no backward class, no GradResult — in
// its emitted IR. (This program deliberately does not import GradResult: an
// undifferentiated program has no reason to name it, and importing it would
// put the very symbol we probe for into the module.)
TEST(FuseGrad, fusedWithoutGradHasNoAutogradShape) {
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        + std::string(kPrelude) +
        "        (Tensor<float32>) -> #Tensor<float32> g =\n"
        "            Fuse((Tensor<float32> t) ->\n"
        "                Tensor.add<float32>(Tensor.mul<float32>(t, t), t));\n"
        "        Tensor<float32> r = g(x);\n"
        "        return Tensor.sum<float32,float32>(r);\n"
        "    }\n"
        "}\n", "test.T", opts);
    EXPECT_FLOAT_EQ(jit->lookup<float (*)()>("run")(), 20.0f);
    // The captured IR is the WHOLE module, embedded stdlib included — and the
    // stdlib legitimately contains GradResult. The probe is therefore scoped
    // to what THIS program emitted: no backward class was synthesized at all,
    // and neither the user function nor the fused synthesis mentions anything
    // autograd-shaped.
    const std::string& ir = jit->getModuleIr();
    EXPECT_EQ(ir.find("__GradBwd_"), std::string::npos)
        << "a backward class was synthesized for an undifferentiated Fuse";
    std::string runBody = bodyOf(ir, "test.T::run");
    ASSERT_FALSE(runBody.empty());
    EXPECT_EQ(runBody.find("GradResult"), std::string::npos)
        << "the undifferentiated program's own code references GradResult";
    std::string makeBody = bodyOf(ir, "__Fused_");
    ASSERT_FALSE(makeBody.empty()) << "no __Fused_ synthesis in the IR";
    EXPECT_EQ(makeBody.find("GradResult"), std::string::npos)
        << "the fused synthesis references GradResult";
}

// 4.1.4 (deferred from U4) — `@Grad @Fuse` composition through the annotation
// sugar: nearest-the-declaration-first, so the body fuses and Grad
// differentiates the fused form. Same 26 as the explicit spelling.
TEST(FuseGrad, gradFuseAnnotationComposition) {
    const char* members =
        "    @Grad @Fuse\n"
        "    public static float32 loss(Tensor<float32> t) {\n"
        "        return Tensor.sum<float32,float32>(Tensor.mul<float32>(t, t));\n"
        "    }\n";
    auto jit = CajetaJit::compile(wrap(members,
        "GradResult<float32, Tensor<float32>> r = T.loss(x);\n"
        "        return r.value + Tensor.sum<float32,float32>(r.grads);"), "test.T");
    EXPECT_FLOAT_EQ(jit->lookup<float (*)()>("run")(), 26.0f);
}
