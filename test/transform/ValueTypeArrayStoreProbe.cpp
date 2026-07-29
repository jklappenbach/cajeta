//
// Probe for the value-type-record array-element store gap surfaced by
// transform-intrinsics 5.1.3 (Vmap(Grad(f))). The batched form synthesizes
//   GradResult<float32,float32>[] out = heap GradResult<float32,float32>[n];
//   out[i] = stack GradResult<float32,float32>(v, g);
// and reading `out[i].grads` back yields uninitialized memory, so the per-element
// store does not land. Vmap's synthesized source is otherwise correct (verified
// via CAJETA_VMAP_DUMP), which is why this pins the store itself — no transform
// intrinsic involved.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

using cajeta_test::CajetaJit;

namespace {
float runF32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class P {\n"
        "    public static float32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.P");
    return jit->lookup<float (*)()>("run")();
}
} // namespace

// Baseline: a value-type record in a LOCAL round-trips (this is the shape U3
// already relies on, so it is expected green).
TEST(ValueTypeArrayStoreProbe, localRoundTrips) {
    EXPECT_FLOAT_EQ(runF32(
        "GradResult<float32,float32> r = stack GradResult<float32,float32>(9.0f, 6.0f);\n"
        "        return r.grads;"), 6.0f);
}

// The pinned case: the SAME record stored into an array element and read back.
TEST(ValueTypeArrayStoreProbe, arrayElementStoreRoundTrips) {
    EXPECT_FLOAT_EQ(runF32(
        "GradResult<float32,float32>[] a = heap GradResult<float32,float32>[2];\n"
        "        a[0] = stack GradResult<float32,float32>(9.0f, 6.0f);\n"
        "        return a[0].grads;"), 6.0f);
}

// Same store, written inside a loop (the exact Vmap shape).
TEST(ValueTypeArrayStoreProbe, arrayElementStoreInLoopRoundTrips) {
    EXPECT_FLOAT_EQ(runF32(
        "GradResult<float32,float32>[] a = heap GradResult<float32,float32>[2];\n"
        "        for (int32 i = 0; i < 2; i = i + 1) {\n"
        "            a[i] = stack GradResult<float32,float32>(9.0f, 6.0f);\n"
        "        }\n"
        "        return a[1].grads;"), 6.0f);
}
