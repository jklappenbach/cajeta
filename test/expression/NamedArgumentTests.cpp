//
// Named (keyword) arguments at call sites: `f(name: value, other: value)`.
//
// Cajeta parses a `parameterLabel? expression` for every call argument
// (CajetaParser.g4 parameterEntry), and the resolver matches labeled calls by
// name via a separate labeled method/constructor map (CajetaClass.cpp invokeMethod
// → labeledMethodMap, Method::buildCanonical bakes labels into the signature).
// Two rules: (1) named args are matched by name, so call-site ORDER is free;
// (2) it is all-or-nothing — every argument must be labeled, or the call is
// treated as positional.
//
// These were previously only exercised by the XPU kernel-launch path
// (grid:/block:/sharedBytes:/spec:); this file pins the general behavior.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runBody(const std::string& decls, const std::string& body) {
    std::string src = std::string("package test;\n") + decls +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// A class whose method is order-sensitive (a - b), so a mis-bind would show.
const char* CALC =
    "public class Calc {\n"
    "    public Calc() {}\n"
    "    public int32 sub(int32 a, int32 b) { return a - b; }\n"
    "}\n";

} // namespace

// Named args on a regular method call bind by name.
TEST(NamedArgumentTests, methodCallByName) {
    EXPECT_EQ(runBody(CALC,
        "Calc c = heap Calc();\n"
        "return c.sub(a: 10, b: 3);\n"), 7);
}

// Call-site order is free: swapping the labels still binds a=10, b=3 → 7
// (NOT 3 - 10 = -7).
TEST(NamedArgumentTests, methodCallOrderIndependent) {
    EXPECT_EQ(runBody(CALC,
        "Calc c = heap Calc();\n"
        "return c.sub(b: 3, a: 10);\n"), 7);
}

const char* POINT =
    "public class Point {\n"
    "    public int32 x;\n"
    "    public int32 y;\n"
    "    public Point(int32 x, int32 y) { this.x = x; this.y = y; }\n"
    "}\n";

// Named args on a constructor, order-independent: y first, x second still binds
// x=1, y=2 → 1*10 + 2 = 12.
TEST(NamedArgumentTests, constructorByNameOrderIndependent) {
    EXPECT_EQ(runBody(POINT,
        "Point p = heap Point(y: 2, x: 1);\n"
        "return p.x * 10 + p.y;\n"), 12);
}

// Named args are ALL-OR-NOTHING. A partially-labeled call is NOT matched by name
// and is NOT a hard error — it falls back to FULLY POSITIONAL binding in source
// order, and the stray labels are ignored. (`floatingParams` in
// CajetaClass.cpp::invokeMethod is true only when EVERY arg is labeled.) These
// tests pin that contract so the fallback can't silently change:
//   c.sub(10, b: 3) → positional sub(10, 3) = 7   (label `b:` ignored)
//   c.sub(b: 3, 10) → positional sub(3, 10) = -7  (label `b:` ignored)
// Practical guidance: label ALL arguments or NONE — never mix (see LanguageGuide).
// NB: this fallback is specific to the GENERAL resolver; the XPU `.launch(stream,
// grid: …, block: …)` form deliberately mixes and is handled by a dedicated
// launch lowering (CallExpression.cpp), not this path.
TEST(NamedArgumentTests, partialLabelingFallsBackToPositional) {
    EXPECT_EQ(runBody(CALC,
        "Calc c = heap Calc();\n"
        "return c.sub(10, b: 3);\n"), 7);
    EXPECT_EQ(runBody(CALC,
        "Calc c = heap Calc();\n"
        "return c.sub(b: 3, 10);\n"), -7);
}
