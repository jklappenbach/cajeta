//
// Session 3.5 / Step 3.4 — path-based borrow tracking.
//
// `String n = #person.name` records the dotted path `person.name` on the
// active scope's moved-paths set. Subsequent reads of the same path — or any
// path passing through a moved prefix — are rejected at codegen time with
// CAJETA_ERROR_USE_AFTER_MOVE.
//
// The check fires at the START of DotExpression::generateCode, before any
// codegen that depends on the field actually existing. That lets us validate
// the analysis even where downstream class-instance codegen isn't fully
// wired up (user-class field allocation isn't end-to-end in v1; the path
// tracker is independent of that and runs early).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

void expectUseAfterMove(const std::string& source, const std::string& expectedFragment) {
    try {
        CajetaJit::compile(source, "test.P");
        FAIL() << "expected use-after-move error but compile succeeded";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_USE_AFTER_MOVE");
        EXPECT_NE(e.getMessage().find(expectedFragment), std::string::npos)
            << "exception message '" << e.getMessage()
            << "' did not contain expected fragment '" << expectedFragment << "'";
    } catch (std::exception& e) {
        FAIL() << "expected cajeta::Exception (use-after-move), got std::exception: " << e.what();
    }
}

std::string source(const std::string& body) {
    return "package test;\n"
           "public final class P {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "        return 0;\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- Path-based moves through a String pseudo-path --------------------------
//
// Strings don't have user-defined fields, so `s.foo` resolves to nothing at
// the class-lookup step. But the path-tracking check runs first, recording the
// path and rejecting later reads — independent of whether the field exists.

TEST(PathBorrowTests, readSamePathAfterMoveErrors) {
    auto src = source(
        "String s = \"hello\";\n"
        "String moved = #s.foo;\n"      // marks path "s.foo" as moved
        "String n = s.foo;");           // reads it again — error
    expectUseAfterMove(src, "s.foo");
}

TEST(PathBorrowTests, readDeeperPathAfterRootMoveErrors) {
    // Move the root identifier `s` itself, then try to read through it.
    auto src = source(
        "String s = \"hello\";\n"
        "String moved = #s;\n"          // root moved
        "String n = s.foo;");           // any path through s is invalid
    expectUseAfterMove(src, "s.foo");
}

TEST(PathBorrowTests, doubleMoveOnSamePathErrors) {
    auto src = source(
        "String s = \"hello\";\n"
        "String a = #s.foo;\n"
        "String b = #s.foo;");          // path already moved
    expectUseAfterMove(src, "s.foo");
}

TEST(PathBorrowTests, deeperPathMoveBlocksTransitiveRead) {
    // Three-level path; mark `s.foo.bar`, then read it. The check walks
    // prefixes; the exact-match case fires here.
    auto src = source(
        "String s = \"hello\";\n"
        "String moved = #s.foo.bar;\n"
        "String n = s.foo.bar;");
    expectUseAfterMove(src, "s.foo.bar");
}

TEST(PathBorrowTests, deeperPathMoveBlocksDeeperRead) {
    // Mark `s.foo`, then try to read `s.foo.bar`. The deeper path passes
    // through a moved prefix and is rejected.
    auto src = source(
        "String s = \"hello\";\n"
        "String moved = #s.foo;\n"
        "String n = s.foo.bar;");       // s.foo is moved → s.foo.bar invalid
    expectUseAfterMove(src, "s.foo.bar");
}

// --- Valid: different sub-paths are independent ----------------------------

TEST(PathBorrowTests, siblingPathStillReadable) {
    // Moving `s.foo` shouldn't touch `s.bar`. The latter read is fine.
    // (Compile still doesn't produce useful IR for these synthetic fields,
    // but the path tracker should not raise an error.)
    auto src = source(
        "String s = \"hello\";\n"
        "String moved = #s.foo;\n"
        "String n = s.bar;");           // different sub-path; OK
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.P"));
}

TEST(PathBorrowTests, unmovedPathReadable) {
    // No moves anywhere — DotExpression should not raise a path-move error.
    auto src = source(
        "String s = \"hello\";\n"
        "String n = s.foo;");
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.P"));
}
