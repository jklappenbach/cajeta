//
// P6.3 — definite-assignment (DA) flow analysis for switch / loops /
// try-catch. The existing P3a/P3b pattern covers if/else: IfStatement::
// generateCode snapshots the NYA set before each branch, runs both,
// then merges (union). Loops and switch / try-catch don't do this
// today, which produces two kinds of bug:
//
//   (a) FALSE NEGATIVE: an inner-branch assignment leaks out of the
//       branch. A variable assigned only inside a `while (false)`
//       body, or only on the try arm, or only on a subset of switch
//       cases, would be considered DA-after even though it might not
//       have run / been assigned.
//
//   (b) FALSE POSITIVE: a variable assigned in EVERY branch but never
//       merged at the join would still be NYA-after — except today's
//       no-snapshot path effectively makes all assignments leak, so
//       this case "works for the wrong reason" and the read just
//       happens to succeed.
//
// These tests cover (a) — the cases the compiler should REJECT but
// currently accepts. Each one expects CAJETA_ERROR_VARIABLE_NOT_ASSIGNED.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

void expectNotAssigned(const std::string& src) {
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_VARIABLE_NOT_ASSIGNED";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VARIABLE_NOT_ASSIGNED")
            << "got error: " << e.getMessage();
    }
}

} // namespace

// --- Loops: while-body assignment must not leak out --------------------
//
// `while (cond)` may execute zero times. Anything assigned only inside
// the body might not have been assigned at the join — read after must
// be rejected.

TEST(DefiniteAssignmentTests, whileBodyAssignmentDoesNotEscape) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    int32 x;\n"
        "    boolean go = false;\n"
        "    while (go) { x = 5; }\n"
        "    return x;\n"
        "  }\n"
        "}\n";
    expectNotAssigned(src);
}

TEST(DefiniteAssignmentTests, forBodyAssignmentDoesNotEscape) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    int32 x;\n"
        "    for (int32 i = 0; i < 0; i = i + 1) { x = 5; }\n"
        "    return x;\n"
        "  }\n"
        "}\n";
    expectNotAssigned(src);
}

// do-while runs the body at least once — assignments DO escape and
// the variable IS definitely assigned. This is the asymmetry the fix
// needs to preserve.

TEST(DefiniteAssignmentTests, doWhileBodyAssignmentEscapes) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    int32 x;\n"
        "    int32 once = 0;\n"
        "    do { x = 7; once = once + 1; } while (once < 1);\n"
        "    return x;\n"  // 7 — body guaranteed to run once
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// --- Switch: assignment must cover EVERY arm including default ---------
//
// Switch without `default:` leaves a "no case matched" path that
// definitely didn't assign anything inside the switch. A read after
// the switch must be rejected.

TEST(DefiniteAssignmentTests, switchWithoutDefaultDoesNotEscape) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    int32 x;\n"
        "    int32 k = 99;\n"
        "    switch (k) {\n"
        "      case 0: x = 1; break;\n"
        "      case 1: x = 2; break;\n"
        "    }\n"
        "    return x;\n"
        "  }\n"
        "}\n";
    expectNotAssigned(src);
}

// Switch with default covering every case → DA after. Positive case
// pinning the join-side of the fix.

TEST(DefiniteAssignmentTests, switchWithDefaultAssigningEveryArm) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    int32 x;\n"
        "    int32 k = 99;\n"
        "    switch (k) {\n"
        "      case 0: x = 1; break;\n"
        "      case 1: x = 2; break;\n"
        "      default: x = 42; break;\n"
        "    }\n"
        "    return x;\n"   // 42
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Switch with default but one arm doesn't assign → NYA after.

TEST(DefiniteAssignmentTests, switchWithDefaultButOneArmUnassigned) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    int32 x;\n"
        "    int32 k = 0;\n"
        "    switch (k) {\n"
        "      case 0: x = 1; break;\n"
        "      case 1: break;\n"             // no assignment
        "      default: x = 42; break;\n"
        "    }\n"
        "    return x;\n"
        "  }\n"
        "}\n";
    expectNotAssigned(src);
}

// --- Try-catch: assignment must cover BOTH try and catch ---------------
//
// A `try` body that throws hands control to the catch — anything the
// try assigned isn't guaranteed to have run. The catch arm must also
// assign for the variable to be DA after.

TEST(DefiniteAssignmentTests, tryAssignsCatchDoesNot) {
    auto src =
        "package test;\n"
        "import cajeta.error.RecoverableException;\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    int32 x;\n"
        "    try { x = 1; } catch (RecoverableException e) { }\n"
        "    return x;\n"
        "  }\n"
        "}\n";
    expectNotAssigned(src);
}

// Both try and catch assign → DA after.

TEST(DefiniteAssignmentTests, tryAndCatchBothAssign) {
    auto src =
        "package test;\n"
        "import cajeta.error.RecoverableException;\n"
        "public final class D {\n"
        "  public static int32 run() {\n"
        "    int32 x;\n"
        "    try { x = 11; } catch (RecoverableException e) { x = 22; }\n"
        "    return x;\n"   // 11 (no throw)
        "  }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}
