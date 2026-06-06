//
// Tests for expression forms that the parser accepts but codegen rejects with a
// cajeta::Exception (CAJETA_ERROR_NOT_IMPLEMENTED). Each test source uses one
// construct and verifies that compilation throws with that specific error id and
// a message naming the construct.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& returnType, const std::string& body) {
    return "package test;\n"
           "public final class U {\n"
           "    public static " + returnType + " run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

// Compile and capture the thrown cajeta::Exception so we can assert on errorId/message.
// EXPECT_THROW only checks that *something* of the right type was thrown — we want
// to confirm the codepath is the NOT_IMPLEMENTED one in particular.
void expectNotImplemented(const std::string& source, const std::string& expectedConstruct) {
    try {
        CajetaJit::compile(source, "test.U");
        FAIL() << "expected cajeta::Exception (not-implemented) but compile succeeded";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_NOT_IMPLEMENTED");
        EXPECT_NE(e.getMessage().find(expectedConstruct), std::string::npos)
            << "exception message '" << e.getMessage()
            << "' did not contain expected construct name '" << expectedConstruct << "'";
    } catch (std::exception& e) {
        FAIL() << "expected cajeta::Exception, got std::exception: " << e.what();
    }
}

} // namespace

TEST(UnsupportedExpressionTests, lambdaThrowsNotImplemented) {
    // L1/L1.5 supports typed-params, bare-identifier params, and expression
    // bodies. L2-2/L2-3 added primitive and heap captures; L2-4 added
    // block-body lambdas. The remaining unsupported form is the var-list
    // parameter shape (`(var a, var b) -> ...`), which needs richer target-
    // type inference to land.
    auto src = makeSource("int32",
        "(int32, int32) -> int32 f = (var a, var b) -> a + b;\n"
        "        return f(1, 2);");
    expectNotImplemented(src, "lambda");
}

TEST(UnsupportedExpressionTests, methodReferenceTypeThrowsNotImplemented) {
    // `Type::method` form — matches ctx->COLONCOLON() in fromContext, which we check
    // before NEW/identifier so it doesn't get swallowed by those branches.
    // Stored into a function-typed local rather than called directly — the
    // grammar doesn't allow `ref()` to call a method-reference value, so the
    // bare reference is the cleanest way to surface the construct.
    auto src = makeSource("int32",
        "(int32) -> string ref = int32::toString;\n"
        "        return 0;");
    expectNotImplemented(src, "method reference");
}

// `super.method()` (primary-super form) is now implemented — see
// MultipleInheritanceGapTests.superMethodCallReachesParent. The
// remaining unimplemented super form is the qualified `obj.super.foo()`
// shape (DOT-branch super-suffix), which only makes sense once inner
// classes exist.

// `obj.method<T>(...)` — explicit method-level type arguments
// (Form C). Now implemented as Phase 3 of cajeta-docs/stdlib/
// MethodLevelTemplate.md; see MethodTemplateExplicitArgsTests for
// coverage. The previously-stub UnsupportedExpression branch has
// been removed.
