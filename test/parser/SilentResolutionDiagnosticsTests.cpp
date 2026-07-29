//
// silent-resolution diagnostics — Unit 1, the backstop.
// Plan: agents/silent-resolution-diagnostics-plan.md
//
//   - 1.1.1 a misspelled method on a plain class receiver must NOT compile.
//     Today it lowers to `null`, the ReturnStatement backstop prints a
//     warning to cerr, and the compile SUCCEEDS emitting `ret null` — the
//     characterization test this whole plan hangs off.
//   - 1.1.2 the resulting error is LOCATED and names the offending
//     expression, not merely the enclosing method.
//   - 1.1.3 the backstop fires only on a MISSING value, never on a
//     legitimately-null or absent one (void return, `String s = null`).
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// `return p.volme();` sits on LINE 9 — the line a located diagnostic must
// name. `volume()` exists; `volme()` is the typo.
constexpr int kTypoCallLine = 9;

const char* kMisspelledMethodSrc =
    "package test;\n"                                            // 1
    "public class Point {\n"                                     // 2
    "    public int32 v = 7;\n"                                  // 3
    "    public int32 volume() { return v; }\n"                  // 4
    "}\n"                                                        // 5
    "public final class D {\n"                                   // 6
    "    public static int32 run() {\n"                          // 7
    "        Point p = heap Point();\n"                          // 8
    "        return p.volme();\n"                                // 9
    "    }\n"                                                    // 10
    "}\n";                                                       // 11

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// 1.1.1: the characterization test. A method that does not exist on the
// receiver's type must fail the compile. RED before Unit 1 (the call lowers
// to null, `ret null` is emitted, and this returns 0).
TEST(SilentResolutionDiagnosticsTests, misspelledMethodOnClassReceiverFailsCompile) {
    EXPECT_THROW(runI32(kMisspelledMethodSrc), cajeta::Exception);
}

// 1.1.2: that failure is located and points at the offending expression.
TEST(SilentResolutionDiagnosticsTests, nullInValuePositionIsLocated) {
    try {
        runI32(kMisspelledMethodSrc);
        FAIL() << "expected the null-in-value-position backstop to fire";
    } catch (cajeta::Exception& e) {
        EXPECT_TRUE(e.hasLocation()) << "backstop must carry a source span";
        EXPECT_FALSE(e.getFile().empty());
        // The typo'd call, not just "somewhere in run()".
        EXPECT_EQ(e.getLine(), kTypoCallLine);
    }
}

// 1.2.3: a CALL ARGUMENT that lowers to nothing must fail at the argument,
// not slide into the callee as a null. A `void` call has no value, so using
// one as an argument is the reachable form now that Unit 2 makes an
// unresolvable member throw at the resolution site.
TEST(SilentResolutionDiagnosticsTests, voidValueAsCallArgumentIsRejected) {
    try {
        runI32(
            "package test;\n"
            "public final class D {\n"
            "    public static void nothing() { return; }\n"
            "    public static int32 takes(int32 a) { return a; }\n"
            "    public static int32 run() {\n"
            "        return D.takes(D.nothing());\n"
            "    }\n"
            "}\n");
        FAIL() << "expected a void-valued argument to fail the compile";
    } catch (cajeta::Exception& e) {
        // Overload resolution already rejects it: a void argument matches no
        // candidate, so the arg position needs no separate guard.
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_NO_MATCHING_OVERLOAD")
            << "got: " << e.getErrorId() << " — " << e.getMessage();
        EXPECT_TRUE(e.hasLocation()) << "the argument guard must carry a span";
    }
}

// 1.2.3: the same for an ASSIGNMENT RHS. `x = D.nothing()` has nothing to
// store; today it stores nothing and x keeps its old value silently.
TEST(SilentResolutionDiagnosticsTests, voidValueAsAssignmentRhsIsRejected) {
    try {
        runI32(
            "package test;\n"
            "public final class D {\n"
            "    public static void nothing() { return; }\n"
            "    public static int32 run() {\n"
            "        int32 x = 1;\n"
            "        x = D.nothing();\n"
            "        return x;\n"
            "    }\n"
            "}\n");
        FAIL() << "expected a void-valued assignment RHS to fail the compile";
    } catch (cajeta::Exception& e) {
        // Assignment is a binary op, so it shares 1.2.2's operand channel.
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_NULL_OPERAND")
            << "got: " << e.getErrorId() << " — " << e.getMessage();
        EXPECT_TRUE(e.hasLocation()) << "the RHS guard must carry a span";
    }
}

// 1.2.3 (over-rejection guard): legitimate null and legitimately-typed
// arguments/RHS still compile. `null` IS a value; a void call in STATEMENT
// position is legal and must stay so.
TEST(SilentResolutionDiagnosticsTests, legitimateArgumentsAndRhsStillCompile) {
    int32_t got = runI32(
        "package test;\n"
        "public final class D {\n"
        "    public static void nothing() { return; }\n"
        "    public static int32 takesRef(String s) { if (s == null) { return 3; } return 0; }\n"
        "    public static int32 run() {\n"
        "        D.nothing();\n"
        "        String s = null;\n"
        "        int32 x = 0;\n"
        "        x = D.takesRef(null);\n"
        "        x = D.takesRef(s);\n"
        "        return x;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(got, 3);
}

// 1.1.3: the backstop must not fire on values that are legitimately absent
// (a void return) or legitimately null (a null-initialized reference).
// These are the false positives that would otherwise force the check to be
// weakened; they must compile and run.
TEST(SilentResolutionDiagnosticsTests, legitimateVoidAndNullStillCompile) {
    int32_t got = runI32(
        "package test;\n"
        "public final class D {\n"
        "    public static void nothing() { return; }\n"
        "    public static String nada() { return null; }\n"
        "    public static int32 run() {\n"
        "        String s = null;\n"
        "        D.nothing();\n"
        "        String t = D.nada();\n"
        "        if (s == null && t == null) { return 5; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(got, 5);
}
