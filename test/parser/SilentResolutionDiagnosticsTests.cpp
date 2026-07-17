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
