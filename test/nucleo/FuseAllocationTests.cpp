//
// nucleo-expr Unit 3 — THE headline acceptance criterion (spec §3.1, §3.2, §9):
// "a multi-operator elementwise tensor expression executes as one kernel and
// allocates NO intermediate buffers (only the final result) — demonstrable by
// allocation count."
//
// So it is demonstrated by allocation count, not by inspecting IR. Every test
// diffs Cajeta.liveCount() across the evaluation (the DropGapTests idiom). The
// eager baseline is the control: the SAME arithmetic written with Tensor.*
// calls allocates one tensor per operator, and the delta between the two IS
// the fusion win.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
int64_t liveDelta(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.lang.Cajeta;\n"
        "public final class T {\n"
        "    public static int64 run() {\n"
        "        float32[] fa = { 1.0f, 2.0f, 3.0f, 4.0f };\n"
        "        int64[] s = heap int64[1]; s[0] = 4;\n"
        "        Tensor<float32> x = Tensor.of<float32>(fa, s);\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    return jit->lookup<int64_t (*)()>("run")();
}

// A 4-operator elementwise chain, fused. ((x*x + x) - x) * x
const char* kFused4 =
    "(Tensor<float32>) -> #Tensor<float32> g =\n"
    "            Fuse((Tensor<float32> t) -> Tensor.mul<float32>(\n"
    "                Tensor.sub<float32>(\n"
    "                    Tensor.add<float32>(Tensor.mul<float32>(t, t), t), t), t));\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        Tensor<float32> r = g(x);\n"
    "        return Cajeta.liveCount() - base;";

// The SAME arithmetic, eager — the control.
const char* kEager4 =
    "int64 base = Cajeta.liveCount();\n"
    "        Tensor<float32> a = Tensor.mul<float32>(x, x);\n"
    "        Tensor<float32> b = Tensor.add<float32>(a, x);\n"
    "        Tensor<float32> c = Tensor.sub<float32>(b, x);\n"
    "        Tensor<float32> r = Tensor.mul<float32>(c, x);\n"
    "        return Cajeta.liveCount() - base;";

// An 8-operator chain, fused — same shape, twice the operators.
const char* kFused8 =
    "(Tensor<float32>) -> #Tensor<float32> g =\n"
    "            Fuse((Tensor<float32> t) -> Tensor.mul<float32>(Tensor.sub<float32>(\n"
    "                Tensor.add<float32>(Tensor.mul<float32>(Tensor.sub<float32>(\n"
    "                    Tensor.add<float32>(Tensor.mul<float32>(t, t), t), t), t), t),\n"
    "                t), t));\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        Tensor<float32> r = g(x);\n"
    "        return Cajeta.liveCount() - base;";
} // namespace

// 3.1.1 — the eager control: one live object PER OPERATOR survives the chain.
// This is the problem statement, measured.
TEST(FuseAllocation, eagerChainAllocatesPerOperator) {
    EXPECT_GE(liveDelta(kEager4), 4);
}

// 3.1.1 — the claim: the fused 4-op chain allocates strictly FEWER objects than
// the eager form. The delta is the fusion win.
TEST(FuseAllocation, fusedChainAllocatesFewerThanEager) {
    int64_t fused = liveDelta(kFused4);
    int64_t eager = liveDelta(kEager4);
    EXPECT_LT(fused, eager) << "fused=" << fused << " eager=" << eager;
}

// 3.1.2 — INDEPENDENCE OF CHAIN LENGTH: doubling the operator count does not
// change the allocation count. Eager would double; fused is flat. This is the
// property that makes it fusion rather than a constant-factor saving.
TEST(FuseAllocation, allocationIsIndependentOfChainLength) {
    EXPECT_EQ(liveDelta(kFused4), liveDelta(kFused8));
}

// 3.1.3 — a shared sub-expression is computed once, not twice. `c = t*t` feeds
// both operands of the outer multiply; the DAG's leaf/CSE dedup means the
// fused body contains it once and allocates nothing extra for the sharing.
TEST(FuseAllocation, sharedSubExpressionCostsNothingExtra) {
    int64_t shared = liveDelta(
        "(Tensor<float32>) -> #Tensor<float32> g =\n"
        "            Fuse((Tensor<float32> t) -> Tensor.mul<float32>(\n"
        "                Tensor.mul<float32>(t, t), Tensor.mul<float32>(t, t)));\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        Tensor<float32> r = g(x);\n"
        "        return Cajeta.liveCount() - base;");
    EXPECT_EQ(shared, liveDelta(kFused4));
}

// 3.1.1 (reduction root) — a reduction-rooted fused expression allocates NO
// tensor at all: the elementwise body fuses into the accumulation and the
// result is a scalar.
TEST(FuseAllocation, reductionRootAllocatesNoTensor) {
    EXPECT_EQ(liveDelta(
        "(Tensor<float32>) -> float32 g =\n"
        "            Fuse((Tensor<float32> t) ->\n"
        "                Tensor.sum<float32,float32>(Tensor.mul<float32>(t, t)));\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        float32 v = g(x);\n"
        "        return Cajeta.liveCount() - base;"), 0);
}
