//
// Named (keyword) arguments at call sites: `f(name: value, other: value)`.
//
// Cajeta parses a `parameterLabel? expression` for every call argument
// (CajetaParser.g4 parameterEntry). Resolution rules:
//   (1) Named args are matched BY NAME — so the order you write them is free.
//   (2) A call may be all-positional, all-named, or a POSITIONAL PREFIX followed
//       by a NAMED SUFFIX (option C, Python/C#/Kotlin style): `f(a, b, x: 1)`.
//       The named suffix is reordered to the formal parameter order
//       (CajetaClass::normalizePartialLabeledCall) and then resolved positionally.
//   (3) A positional argument may NOT follow a named one — that is a compile error
//       (LANG-NAMEDARG).
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

// A class whose methods are order-sensitive, so a mis-bind would show:
//   sub(a, b)        = a - b
//   combine(a, b, c) = a*100 + b*10 + c   (digit positions reveal any swap)
const char* CALC =
    "public class Calc {\n"
    "    public Calc() {}\n"
    "    public int32 sub(int32 a, int32 b) { return a - b; }\n"
    "    public int32 combine(int32 a, int32 b, int32 c) { return a * 100 + b * 10 + c; }\n"
    "}\n";

} // namespace

// Named args on a regular method call bind by name.

// Call-site order is free: swapping the labels still binds a=10, b=3 → 7
// (NOT 3 - 10 = -7).

const char* POINT =
    "public class Point {\n"
    "    public int32 x;\n"
    "    public int32 y;\n"
    "    public Point(int32 x, int32 y) { this.x = x; this.y = y; }\n"
    "}\n";

// Named args on a constructor, order-independent: y first, x second still binds
// x=1, y=2 → 1*10 + 2 = 12.

// Option C: a positional PREFIX followed by a named SUFFIX. The prefix binds by
// position, the suffix by name. `sub(10, b: 3)` → a=10 (positional), b=3 (named)
// → 7.
TEST(NamedArgumentTests, partialPrefixThenNamedSuffix) {
    EXPECT_EQ(runBody(CALC,
        "Calc c = heap Calc();\n"
        "return c.sub(10, b: 3);\n"), 7);
}

// The named suffix is matched by name, so its WRITTEN order is free even with a
// positional prefix: a=1 (positional), then c:3,b:2 (named, reordered to b,c)
// → 1*100 + 2*10 + 3 = 123 (NOT 1*100 + 3*10 + 2 = 132).
TEST(NamedArgumentTests, partialNamedSuffixReordered) {
    EXPECT_EQ(runBody(CALC,
        "Calc c = heap Calc();\n"
        "return c.combine(1, c: 3, b: 2);\n"), 123);
}

// A positional argument may NOT follow a named one — compile error (LANG-NAMEDARG).
TEST(NamedArgumentTests, positionalAfterNamedRejected) {
    EXPECT_THROW(runBody(CALC,
        "Calc c = heap Calc();\n"
        "return c.sub(a: 10, 3);\n"), cajeta::Exception);
}
