//
// Session 2 / Step 2.6 — use-after-move static-check tests.
//
// The `#` operator transfers ownership of its operand. After a transfer, the
// source identifier is no longer readable; the compiler must reject any
// subsequent access. These tests confirm both directions: valid programs
// compile, invalid ones throw `CAJETA_ERROR_USE_AFTER_MOVE`.
//
// Path-based tracking (field-path moves, alias-mutation invalidation) and
// inter-procedural elision land in Session 3 — those cases are out of scope
// here.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "public final class U {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

// Compile and capture the cajeta::Exception; assert errorId is the expected
// CAJETA_ERROR_USE_AFTER_MOVE.
void expectUseAfterMove(const std::string& source, const std::string& expectedName) {
    try {
        CajetaJit::compile(source, "test.U");
        FAIL() << "expected use-after-move error but compile succeeded";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_USE_AFTER_MOVE");
        EXPECT_NE(e.getMessage().find(expectedName), std::string::npos)
            << "exception message '" << e.getMessage()
            << "' did not contain expected identifier '" << expectedName << "'";
    } catch (std::exception& e) {
        FAIL() << "expected cajeta::Exception (use-after-move), got std::exception: " << e.what();
    }
}

} // namespace

// --- Valid programs: the move is the last use ------------------------------

TEST(UseAfterMoveTests, moveIsLastUse) {
    // Moving `s` into `t` and never reading `s` again is fine.
    auto src = makeSource(
        "String s = \"hello\";\n"
        "String t #= s;\n"
        "return 0;");
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.U"));
}

TEST(UseAfterMoveTests, multipleMovesOfDifferentVariables) {
    // Two separate moves, neither variable read again — both fine.
    auto src = makeSource(
        "String a = \"x\";\n"
        "String b = \"y\";\n"
        "String c #= a;\n"
        "String d #= b;\n"
        "return 0;");
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.U"));
}

TEST(UseAfterMoveTests, useBeforeMoveIsFine) {
    // Reading `s` before the move is fine; the use-after-move check fires on
    // reads *after* the move only.
    auto src = makeSource(
        "String s = \"hello\";\n"
        "int32 n = (int32) s.size();\n"
        "String t #= s;\n"
        "return n;");
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.U"));
}

TEST(UseAfterMoveTests, moveInsideArgument) {
    // Moving into a method argument — `s` is consumed by the call; not read
    // again afterward.
    auto src =
        "package test;\n"
        "public final class U {\n"
        "    public static void consume(#String s) { }\n"
        "    public static int32 run() {\n"
        "        String s = \"hi\";\n"
        "        consume(#s);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.U"));
}

TEST(UseAfterMoveTests, moveOnReturnExpression) {
    // Returning a moved local — the function frame is exiting, no further use.
    auto src =
        "package test;\n"
        "public final class U {\n"
        "    public static #String make() {\n"
        "        String s = \"hi\";\n"
        "        return #s;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.U"));
}

// --- Invalid programs: use after the move ----------------------------------

TEST(UseAfterMoveTests, readAfterMoveOnAssignment) {
    auto src = makeSource(
        "String s = \"hello\";\n"
        "String t #= s;\n"
        "int32 n = (int32) s.size();\n"   // s was moved into t — error
        "return n;");
    expectUseAfterMove(src, "s");
}

TEST(UseAfterMoveTests, readAfterMoveByPrintln) {
    auto src = makeSource(
        "String s = \"hello\";\n"
        "String t #= s;\n"
        "System.stdout.println(s);\n"     // s was moved
        "return 0;");
    expectUseAfterMove(src, "s");
}

TEST(UseAfterMoveTests, readAfterMoveAsIntrinsicArgument) {
    // Use-after-move read appears as an argument to a String intrinsic.
    // The intrinsic dispatch path evaluates the argument expression, which
    // routes back into IdentifierExpression::generateCode and triggers the
    // use-after-move check.
    auto src = makeSource(
        "String s = \"hi\";\n"
        "String t #= s;\n"
        "int32 n = Integer.parseInt(s);\n"   // s was moved
        "return n;");
    expectUseAfterMove(src, "s");
}

TEST(UseAfterMoveTests, doubleMove) {
    // Moving twice from the same source — the second `#s` would already be
    // reading a moved identifier.
    auto src = makeSource(
        "String s = \"hello\";\n"
        "String a #= s;\n"
        "String b #= s;\n"                // double-move on s
        "return 0;");
    expectUseAfterMove(src, "s");
}

TEST(UseAfterMoveTests, moveThenReturnSource) {
    auto src =
        "package test;\n"
        "public final class U {\n"
        "    public static #String run() {\n"
        "        String s = \"hi\";\n"
        "        String t #= s;\n"
        "        return #s;\n"            // s already moved into t
        "    }\n"
        "}\n";
    expectUseAfterMove(src, "s");
}
