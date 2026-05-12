//
// Session 3 / Step 3.5 — inter-procedural elision check.
//
// Multi-parameter free functions can't return a borrow because the language
// has no rule (without explicit lifetimes) for picking which parameter the
// return borrow inherits from. The compiler rejects the signature at
// prototype-generation time with `CAJETA_ERROR_BORROW_RETURN_MULTI_PARAM`.
//
// Methods (non-static) are exempt — their borrow-return inherits from `this`.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeClass(const std::string& body) {
    return "package test;\n"
           "public final class E {\n"
           + body +
           "    public static int32 run() { return 0; }\n"
           "}\n";
}

void expectMultiParamBorrowReturnError(const std::string& source) {
    try {
        CajetaJit::compile(source, "test.E");
        FAIL() << "expected CAJETA_ERROR_BORROW_RETURN_MULTI_PARAM but compile succeeded";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_BORROW_RETURN_MULTI_PARAM");
    }
}

} // namespace

// --- Valid signatures -------------------------------------------------------

TEST(ElisionTests, singleParamBorrowReturnOk) {
    // One borrow parameter, borrow return — elision ties the return to the
    // single input. Allowed.
    auto src = makeClass(
        "public static String first(String s) { return s; }\n");
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.E"));
}

TEST(ElisionTests, multiParamPrimitiveReturnOk) {
    // Multi-param but the return is a primitive value type — no borrow
    // involved, no ambiguity.
    auto src = makeClass(
        "public static int32 count(String a, String b) { return 0; }\n");
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.E"));
}

TEST(ElisionTests, multiParamOwnedReturnOk) {
    // Multi-param with `#`-prefixed return — caller takes ownership; no
    // borrow inheritance question to resolve.
    auto src = makeClass(
        "public static #String pick(String a, String b) { return \"x\"; }\n");
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.E"));
}

TEST(ElisionTests, noParamReturnOk) {
    // No parameters; the only way for a borrow to escape would be through a
    // captured static — allowed for now (static-lifetime is the universal
    // longer life). Future tightening may forbid this too.
    auto src = makeClass(
        "public static String hello() { return \"hi\"; }\n");
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.E"));
}

// --- Invalid signatures -----------------------------------------------------

TEST(ElisionTests, twoParamBorrowReturnRejected) {
    auto src = makeClass(
        "public static String pick(String a, String b) { return a; }\n");
    expectMultiParamBorrowReturnError(src);
}

TEST(ElisionTests, threeParamBorrowReturnRejected) {
    auto src = makeClass(
        "public static String first(String a, String b, String c) { return a; }\n");
    expectMultiParamBorrowReturnError(src);
}

TEST(ElisionTests, mixedTypeMultiParamBorrowReturnRejected) {
    // Mixing primitives with pointer params still trips the rule — what
    // matters is that the return is a borrow and there's more than one
    // input slot the borrow *could* come from.
    auto src = makeClass(
        "public static String describe(String s, int32 n) { return s; }\n");
    expectMultiParamBorrowReturnError(src);
}
