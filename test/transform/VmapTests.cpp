//
// transform-intrinsics Unit 5 — Vmap (spec §6.1, §6.2). `Vmap(f)` lifts `f` over a
// leading batch axis: `f` is written for a SINGLE example and runs over a batch
// with no hand-written loop, so `Vmap(f)(xs)` equals mapping `f` over `xs`. The
// batching is a compile-time transform over f's specialized body (the Grad U3
// Tier-A precedent), not a runtime wrapper. An op with no batching rule is a
// named, located compile error (the @Kernel/XPU-N01 bounded-IR precedent) — never
// a silently wrong batch.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
std::string prog(const std::string& body) {
    return
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class G {\n"
        "    public static float32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
}

float runF32(const std::string& body) {
    auto jit = CajetaJit::compile(prog(body), "test.G");
    return jit->lookup<float (*)()>("run")();
}

std::string compileErrorId(const std::string& body) {
    try { CajetaJit::compile(prog(body), "test.G"); }
    catch (cajeta::Exception& e) { return e.getErrorId(); }
    return "";
}
} // namespace

// 5.1.1 — Vmap(f) over a leading batch axis equals mapping f over the batch, with
// no hand-written loop. f = x*x over {1,2,3} -> {1,4,9}; positionally encoded as
// 1 + 4*10 + 9*100 = 941 (so a mis-ordered batch cannot pass).
TEST(Vmap, batchEqualsMap) {
    EXPECT_FLOAT_EQ(runF32(
        "float32[] xs = {1.0f, 2.0f, 3.0f};\n"
        "        (float32[]) -> #float32[] g = Vmap((float32 x) -> x * x);\n"
        "        float32[] ys = g(xs);\n"
        "        return ys[0] + ys[1] * 10.0f + ys[2] * 100.0f;"), 941.0f);
}

// 5.1.1 (batch-size independence) — the same transformed function runs over a
// different batch length; the axis is the argument's, not baked into the closure.
// f = x+x over {2,5} -> {4,10}; 4 + 10*10 = 104.
TEST(Vmap, batchLengthIsTheArgumentAxis) {
    EXPECT_FLOAT_EQ(runF32(
        "float32[] xs = {2.0f, 5.0f};\n"
        "        (float32[]) -> #float32[] g = Vmap((float32 x) -> x + x);\n"
        "        float32[] ys = g(xs);\n"
        "        return ys[0] + ys[1] * 10.0f;"), 104.0f);
}

// 5.1.2 — an op with NO batching rule is a named, located compile error, never a
// silently wrong batch. Modulo has no batching rule (div joined the set in nucleo-autograd U1).
TEST(Vmap, unbatchableOpNamedError) {
    EXPECT_EQ(compileErrorId(
        "float32[] xs = {1.0f, 2.0f};\n"
        "        (float32[]) -> #float32[] g = Vmap((float32 x) -> x % x);\n"
        "        float32[] ys = g(xs);\n"
        "        return ys[0];"), "CAJETA_ERROR_TRANSFORM_NO_BATCH_RULE");
}

// 5.1.3 — Vmap(Grad(f)) batches the DIFFERENTIATED form: per-example gradients
// over the batch (composition with U3). f = x*x, so grad = 2x; over {1,2,3} the
// grads are {2,4,6} -> 2 + 4*10 + 6*100 = 642.
TEST(Vmap, vmapOfGradGivesPerExampleGradients) {
    EXPECT_FLOAT_EQ(runF32(
        "float32[] xs = {1.0f, 2.0f, 3.0f};\n"
        "        (float32[]) -> #GradResult<float32,float32>[] g =\n"
        "            Vmap(Grad((float32 x) -> x * x));\n"
        "        GradResult<float32,float32>[] rs = g(xs);\n"
        "        return rs[0].grads + rs[1].grads * 10.0f + rs[2].grads * 100.0f;"),
        642.0f);
}
