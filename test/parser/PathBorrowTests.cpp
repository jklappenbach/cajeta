//
// Session 3.5 / Step 3.4 — path-based borrow tracking.
//
// `String n = #person.name` records the dotted path `person.name` on the
// active scope's moved-paths set. Subsequent reads of the same path — or any
// path passing through a moved prefix — are rejected at codegen time with
// CAJETA_ERROR_USE_AFTER_MOVE.
//
// The check fires at the START of DotExpression::generateCode, before any
// codegen that depends on the field actually existing.
//
// The fixtures use REAL classes with real nested fields. They originally used
// `String s` with synthetic members (`s.foo`) that do not exist, because when
// this suite was written user-class field allocation was not end-to-end. That
// only ever compiled because the member check was silently absent; it is not
// absent any more. See the note on `source()` below.
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

// Real classes with real nested fields, so `s.foo` and `s.foo.bar` are genuine
// paths.
//
// These fixtures used to be `String s = "hello"` with SYNTHETIC members
// (`s.foo`, `s.bar`) that do not exist on String — the header note above
// explains why: when this suite was written, user-class field allocation was
// not end-to-end, and the path tracker runs before the member lookup, so fake
// fields were a cheap way to exercise it. That worked only because the compiler
// silently accepted a member that did not exist. It no longer does
// (silent-resolution-diagnostics Unit 2: `no member 'foo' on 'cajeta.lang.String'`).
//
// The path shapes under test are UNCHANGED — s.foo, s.foo.bar, s.bar. Only the
// receiver is now a type that really has them, so the suite tests the path
// tracker rather than the absence of a member check.
std::string source(const std::string& body) {
    return "package test;\n"
           "public class Inner {\n"
           "    public String bar = \"b\";\n"
           "}\n"
           "public class Outer {\n"
           "    public Inner foo;\n"
           "    public String bar = \"sib\";\n"
           "}\n"
           "public final class P {\n"
           "    public static int32 run() {\n"
           "        Outer s = heap Outer();\n"
           "        s.foo = heap Inner();\n"
           "        " + body + "\n"
           "        return 0;\n"
           "    }\n"
           "}\n";
}

} // namespace

TEST(PathBorrowTests, readSamePathAfterMoveErrors) {
    auto src = source(
        "Inner moved = #s.foo;\n"      // marks path "s.foo" as moved
        "Inner n = s.foo;");           // reads it again — error
    expectUseAfterMove(src, "s.foo");
}

TEST(PathBorrowTests, readDeeperPathAfterRootMoveErrors) {
    // Move the root identifier `s` itself, then try to read through it.
    auto src = source(
        "Outer moved = #s;\n"          // root moved
        "Inner n = s.foo;");           // any path through s is invalid
    expectUseAfterMove(src, "s.foo");
}

TEST(PathBorrowTests, doubleMoveOnSamePathErrors) {
    auto src = source(
        "Inner a = #s.foo;\n"
        "Inner b = #s.foo;");          // path already moved
    expectUseAfterMove(src, "s.foo");
}

TEST(PathBorrowTests, deeperPathMoveBlocksTransitiveRead) {
    // Three-level path; mark `s.foo.bar`, then read it. The check walks
    // prefixes; the exact-match case fires here.
    auto src = source(
        "String moved = #s.foo.bar;\n"
        "String n = s.foo.bar;");
    expectUseAfterMove(src, "s.foo.bar");
}

TEST(PathBorrowTests, deeperPathMoveBlocksDeeperRead) {
    // Mark `s.foo`, then try to read `s.foo.bar`. The deeper path passes
    // through a moved prefix and is rejected.
    auto src = source(
        "Inner moved = #s.foo;\n"
        "String n = s.foo.bar;");       // s.foo is moved → s.foo.bar invalid
    expectUseAfterMove(src, "s.foo.bar");
}

// --- Valid: different sub-paths are independent ----------------------------

TEST(PathBorrowTests, siblingPathStillReadable) {
    // Moving `s.foo` shouldn't touch `s.bar` — a different sub-path.
    auto src = source(
        "Inner moved = #s.foo;\n"
        "String n = s.bar;");           // different sub-path; OK
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.P"));
}

TEST(PathBorrowTests, unmovedPathReadable) {
    // No moves anywhere — DotExpression should not raise a path-move error.
    auto src = source(
        "Inner n = s.foo;");
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.P"));
}
