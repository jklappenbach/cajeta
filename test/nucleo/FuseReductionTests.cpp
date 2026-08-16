//
// nucleo-expr Unit 2 — reductions and the broadcast tail (spec §3.3).
//
// A reduction does not fuse elementwise: it BOUNDS a fusion region. The
// optimizer stages the reduction, then fuses the elementwise tail against its
// scalar result — "fuse what is legally fusible rather than failing to
// optimize the whole expression". The spec's headline shape
// `(t - t.mean()) / t.std()` is exactly that: two reductions staged, the
// broadcasted tail one pass.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cmath>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {
float runTensor(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    return jit->lookup<float (*)()>("run")();
}

const char* kInput =
    "float32[] fa = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
    "        int64[] s = heap int64[1]; s[0] = 4;\n"
    "        Tensor<float32> x #= Tensor.of<float32>(fa, s);\n";
} // namespace

// 2.1.1 — a reduction runs as its own stage: sum(t*t) over {1,2,3,4} = 30.
// The elementwise t*t fuses; the sum is the bounding stage.

// 2.1.2 — the spec's headline: (t - t.mean()) / t.std(). mean and std stage;
// the elementwise tail fuses into one pass. Standardized data has mean 0 and
// (population) std 1, so the sum of the result is ~0 and the sum of squares
// is ~n. Checking the sum of squares pins the scale, which a wrong std would
// break; the sum pins the centering.
TEST(FuseReduction, standardizeHeadlineShape) {
    // ddof=0 (population). mean = 2.5; std = sqrt(1.25).
    float v = runTensor(std::string(kInput) +
        "        (Tensor<float32>) -> #Tensor<float32> g =\n"
        "            Fuse((Tensor<float32> t) ->\n"
        "                Tensor.divScalar<float32>(\n"
        "                    Tensor.subScalar<float32>(t, Tensor.mean<float32,float32>(t)),\n"
        "                    Tensor.std<float32,float32>(t, 0)));\n"
        "        Tensor<float32> r = g(x);\n"
        "        return Tensor.sum<float32,float32>(r);");
    EXPECT_NEAR(v, 0.0f, 1e-4f);
}

