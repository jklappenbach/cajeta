//
// Probe for silent-resolution 1.2.3 — where does a VOID-valued expression in a
// value position go? A `void` call has no value, so every one of these is an
// error; the question is which are diagnosed, which miscompile, and which HANG
// the compiler. `x = D.nothing()` was observed to hang (timeout, not crash).
//
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
std::string prog(const std::string& body) {
    return
        "package test;\n"
        "public final class D {\n"
        "    public static void nothing() { return; }\n"
        "    public static int32 takes(int32 a) { return a; }\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
}
std::string errIdOf(const std::string& body) {
    try { CajetaJit::compile(prog(body), "test.D"); }
    catch (cajeta::Exception& e) { return e.getErrorId(); }
    return "<compiled, no error>";
}
} // namespace

// Statement position — LEGAL, the control case.
TEST(VoidValuePositionProbe, voidCallInStatementPositionIsLegal) {
    EXPECT_EQ(errIdOf("D.nothing();\n        return 1;"), "<compiled, no error>");
}

// Local INITIALIZER — Unit 3 (3.2.1) guards initializers.
TEST(VoidValuePositionProbe, voidAsLocalInitializer) {
    EXPECT_NE(errIdOf("int32 x = D.nothing();\n        return x;"),
              "<compiled, no error>");
}

// RETURN position — Unit 1's backstop (1.2.1).
TEST(VoidValuePositionProbe, voidAsReturnValue) {
    EXPECT_NE(errIdOf("return D.nothing();"), "<compiled, no error>");
}

// CALL ARGUMENT — reaches overload resolution.
TEST(VoidValuePositionProbe, voidAsCallArgument) {
    EXPECT_NE(errIdOf("return D.takes(D.nothing());"), "<compiled, no error>");
}

// CONDITION — Unit 1 (1.2.2) guards conditions at evalCondition.
TEST(VoidValuePositionProbe, voidAsCondition) {
    EXPECT_NE(errIdOf("if (D.nothing()) { return 1; }\n        return 0;"),
              "<compiled, no error>");
}

// ASSIGNMENT RHS — the 1.2.3 gap. HANGS the compiler today.
TEST(VoidValuePositionProbe, voidAsAssignmentRhs) {
    EXPECT_NE(errIdOf("int32 x = 1;\n        x = D.nothing();\n        return x;"),
              "<compiled, no error>");
}
