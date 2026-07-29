//
// nucleo-expr Unit 5 (5.1.4) — the column seam pin (plan X7-seam).
//
// `elementExpr` is the ONE place a DAG node becomes element-level source, and
// the ONLY place allowed to assume a dense, always-valid input buffer (the
// `t.get1(i)` leaf). Nullable columns (nucleo-column, [X7]) will need validity
// propagation exactly there and nowhere else. These tests are the CONTRACT a
// future null-aware variant must match: they drive `elementExpr` directly with
// a hand-built DAG (no codegen) and check the emitted element source, and they
// prove the surrounding emitters carry no dense-input knowledge of their own.
//
#include "gtest/gtest.h"
#include "cajeta/transform/FuseExpr.h"
#include "cajeta/transform/GradBackward.h"

#include <string>
#include <vector>

using cajeta::transform::AdNode;
using cajeta::transform::Hoist;
using cajeta::transform::elementExpr;
using cajeta::transform::emitFusedReductionSource;
using cajeta::transform::emitFusedSource;

namespace {
AdNode leaf(const std::string& name) {
    AdNode n;
    n.valueExpr = name;
    n.isInputParam = true;
    n.isTensor = true;
    return n;
}
AdNode prim(const std::string& p, std::vector<size_t> ops,
            const std::string& valueExpr = "", bool tensor = true) {
    AdNode n;
    n.primitive = p;
    n.operands = std::move(ops);
    n.valueExpr = valueExpr;
    n.isTensor = tensor;
    return n;
}
size_t count(const std::string& hay, const std::string& needle) {
    size_t c = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + 1)) ++c;
    return c;
}
} // namespace

// The elementwise contract: every dense read is `<param>.get1(<indexVar>)`,
// one per input-leaf REFERENCE, and the operators collapse to parenthesized
// scalar arithmetic around them. (t*t)+t over leaf `t`.
TEST(FuseSeam, elementExprReadsLeavesThroughGet1Only) {
    std::vector<AdNode> nodes;
    nodes.push_back(leaf("t"));            // 0: t
    nodes.push_back(prim("mul", {0, 0}));  // 1: t*t
    nodes.push_back(prim("add", {1, 0}));  // 2: (t*t)+t
    std::vector<Hoist> hoists;
    std::string err;
    std::string src = elementExpr(nodes, 2, "__i", &hoists, &err);
    EXPECT_EQ(src, "((t.get1(__i) * t.get1(__i)) + t.get1(__i))");
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_TRUE(hoists.empty());
    EXPECT_EQ(count(src, "get1("), 3u);    // one per leaf reference, no more
}

// A reduction operand does NOT read the buffer inside the loop: it stages to a
// hoisted local (loop-invariant), and the hoist's initializer is the
// TENSOR-level source verbatim — the dense assumption never spreads to it.
TEST(FuseSeam, reductionOperandStagesWithoutDenseRead) {
    std::vector<AdNode> nodes;
    nodes.push_back(leaf("t"));                                        // 0
    nodes.push_back(prim("mean", {0}, "Tensor.mean<float32>(t)",
                         /*tensor=*/false));                           // 1
    nodes.push_back(prim("sub", {0, 1}));                              // 2: t - mean(t)
    std::vector<Hoist> hoists;
    std::string err;
    std::string src = elementExpr(nodes, 2, "__i", &hoists, &err);
    EXPECT_EQ(src, "(t.get1(__i) - __r0)");
    ASSERT_EQ(hoists.size(), 1u);
    EXPECT_EQ(hoists[0].local, "__r0");
    EXPECT_EQ(hoists[0].init, "Tensor.mean<float32>(t)");
    EXPECT_EQ(hoists[0].init.find("get1("), std::string::npos);
}

// A non-elementwise primitive refuses to lower — named in the error — instead
// of guessing a dense form for something that has none.
TEST(FuseSeam, nonElementwisePrimitiveIsARefusalNotAGuess) {
    std::vector<AdNode> nodes;
    nodes.push_back(leaf("t"));
    nodes.push_back(prim("matmul", {0, 0}));
    std::vector<Hoist> hoists;
    std::string err;
    EXPECT_EQ(elementExpr(nodes, 1, "__i", &hoists, &err), "");
    EXPECT_NE(err.find("matmul"), std::string::npos) << err;
}

// The EMITTERS carry no dense-input knowledge: with a placeholder element
// expression, neither fused-source shape contains a `get1` of its own — every
// dense read in real output comes from elementExpr. (The output write,
// `set1` on the freshly allocated result, is the emitter's own output
// contract, not an input-validity assumption.)
TEST(FuseSeam, emittersHaveNoDenseInputAssumption) {
    std::string loop = emitFusedSource("__C", "t", "float32", "__ELEM__", {});
    std::string red = emitFusedReductionSource("__C", "t", "float32", "sum",
                                               "__ELEM__", {});
    EXPECT_EQ(loop.find("get1("), std::string::npos) << loop;
    EXPECT_EQ(red.find("get1("), std::string::npos) << red;
    EXPECT_NE(loop.find("__ELEM__"), std::string::npos);
    EXPECT_NE(red.find("__ELEM__"), std::string::npos);
}
