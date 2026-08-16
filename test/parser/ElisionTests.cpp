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





// --- Invalid signatures -----------------------------------------------------


TEST(ElisionTests, threeParamBorrowReturnRejected) {
    auto src = makeClass(
        "public static String first(String a, String b, String c) { return a; }\n");
    expectMultiParamBorrowReturnError(src);
}

