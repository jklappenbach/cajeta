//
// transform-intrinsics Unit 3 — the reverse-mode autodiff LOGIC (spec §5), tested
// as pure C++ over a hand-built forward DAG, decoupled from the AST walk and the
// synthesis plumbing. `reverseModeGrad` composes VjpRegistry rules in reverse into
// a grad SOURCE expression; `emitBackwardSource` assembles the Tier-A helper class.
//
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "cajeta/transform/GradBackward.h"

using cajeta::transform::AdNode;
using cajeta::transform::reverseModeGrad;
using cajeta::transform::emitBackwardSource;

namespace {
// x is node 0 (input param); x*x is node 1 (output).
std::vector<AdNode> squareDag() {
    return {
        AdNode{"x", true, "", {}},
        AdNode{"x * x", false, "mul", {0, 0}},
    };
}
} // namespace

// d/dx (x*x) = g*x + g*x with g seeded to 1.0f (x used twice → both accumulate).
TEST(GradBackward, squareGradAccumulatesReusedInput) {
    std::string missing;
    std::string grad = reverseModeGrad(squareDag(), 0, "", &missing);
    EXPECT_TRUE(missing.empty());
    EXPECT_EQ(grad, "(1.0f) * x + (1.0f) * x");
}

// d/dx (x + x) = g + g (add rule is identity cotangents).
TEST(GradBackward, addGradIsIdentityTwice) {
    std::vector<AdNode> dag = {
        AdNode{"x", true, "", {}},
        AdNode{"x + x", false, "add", {0, 0}},
    };
    std::string missing;
    EXPECT_EQ(reverseModeGrad(dag, 0, "", &missing), "(1.0f) + (1.0f)");
    EXPECT_TRUE(missing.empty());
}

// d/dx (-x) = -(g).
TEST(GradBackward, negateGrad) {
    std::vector<AdNode> dag = {
        AdNode{"x", true, "", {}},
        AdNode{"-(x)", false, "negate", {0}},
    };
    std::string missing;
    EXPECT_EQ(reverseModeGrad(dag, 0, "", &missing), "-((1.0f))");
    EXPECT_TRUE(missing.empty());
}

// Tensor reduction: d/dx sum(x*x). Cotangents are scalar at the output (seed
// 1.0f), tensor below the sum — so the accumulation of x's two contributions is
// spelled `Tensor.add<E>`, and the sum's cotangent broadcasts via onesLike*g.
TEST(GradBackward, tensorSumOfSquaresUsesTensorSurface) {
    std::vector<AdNode> dag = {
        AdNode{"x", true, "", {}, true},                                   // param tensor
        AdNode{"Tensor.mul<float32>(x, x)", false, "mul", {0, 0}, true},   // elementwise
        AdNode{"Tensor.sum<float32,float32>(Tensor.mul<float32>(x, x))",
               false, "sum", {1}, false},                                  // scalar output
    };
    std::string missing;
    std::string grad = reverseModeGrad(dag, 0, "float32", &missing);
    EXPECT_TRUE(missing.empty());
    // sum's cotangent broadcasts the scalar seed back over the operand's shape,
    EXPECT_NE(grad.find(
        "Tensor.mulScalar<float32>(Tensor.onesLike<float32>("), std::string::npos);
    // mul's rule is elementwise, and x's two contributions accumulate over tensors.
    EXPECT_NE(grad.find("Tensor.mul<float32>("), std::string::npos);
    EXPECT_NE(grad.find("Tensor.add<float32>("), std::string::npos);
}

// matmul reverse-composition through the composer (not just the rule in isolation):
// loss = sum(A @ B), so dA = g @ B^T with g the sum's broadcast cotangent. Verifies
// the transposed-product spelling + surface selection compose end-to-end.
TEST(GradBackward, matmulReverseCompositionTensorSurface) {
    std::vector<AdNode> dag = {
        AdNode{"A", true, "", {}, true},                                       // param tensor
        AdNode{"B", false, "", {}, true},                                      // const tensor
        AdNode{"Tensor.matmul<float32>(A, B)", false, "matmul", {0, 1}, true}, // A @ B
        AdNode{"Tensor.sum<float32,float32>(Tensor.matmul<float32>(A, B))",
               false, "sum", {2}, false},                                      // scalar loss
    };
    std::string missing;
    std::string grad = reverseModeGrad(dag, 0, "float32", &missing);
    EXPECT_TRUE(missing.empty());
    EXPECT_NE(grad.find("Tensor.matmul<float32>("), std::string::npos);
    EXPECT_NE(grad.find(".transpose())"), std::string::npos);       // B^T in dA
    EXPECT_NE(grad.find("Tensor.onesLike<float32>"), std::string::npos); // sum broadcast
}

// A primitive with no registered VJP rule → the composer reports it by name
// (the §5.3 missing-rule signal), grad expr empty.
TEST(GradBackward, missingRuleReportedByName) {
    std::vector<AdNode> dag = {
        AdNode{"x", true, "", {}},
        AdNode{"sin(x)", false, "sin", {0}},
    };
    std::string missing;
    EXPECT_EQ(reverseModeGrad(dag, 0, "", &missing), "");
    EXPECT_EQ(missing, "sin");
}

// The assembled Tier-A backward source: a helper class with a static `make()`
// returning the backward as a lambda producing `stack GradResult<V,G>(value, grads)`.
TEST(GradBackward, emitsTierASourceMakeReturningLambda) {
    std::string src = emitBackwardSource(
        "__GradBwd_1", {"x"}, {"float32"}, "float32", "float32",
        "x * x", "(1.0f) * x + (1.0f) * x");
    EXPECT_NE(src.find("import cajeta.nucleo.transform.GradResult;"), std::string::npos);
    EXPECT_NE(src.find("class __GradBwd_1"), std::string::npos);
    EXPECT_NE(src.find(
        "static (float32) -> GradResult<float32,float32> make()"), std::string::npos);
    EXPECT_NE(src.find(
        "return (float32 x) -> stack GradResult<float32,float32>"
        "(x * x, (1.0f) * x + (1.0f) * x);"),
        std::string::npos);
}

// 3.1.4 — the multi-param backward takes ALL of f's params (so the returned
// closure has f's exact arity); the grad is w.r.t. the selected arg only.
TEST(GradBackward, emitsMultiParamBackward) {
    std::string src = emitBackwardSource(
        "__GradBwd_2", {"x", "y"}, {"float32", "float32"}, "float32", "float32",
        "(x * y)", "(1.0f) * y");
    EXPECT_NE(src.find(
        "static (float32,float32) -> GradResult<float32,float32> make()"),
        std::string::npos);
    EXPECT_NE(src.find(
        "return (float32 x, float32 y) -> stack GradResult<float32,float32>"
        "((x * y), (1.0f) * y);"),
        std::string::npos);
}
