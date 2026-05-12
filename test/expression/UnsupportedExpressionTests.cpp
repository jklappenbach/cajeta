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
    // `() -> 42` triggers ctx->lambdaExpression() in Expression::fromContext.
    auto src = makeSource("int32", "return ((() -> 42)());");
    expectNotImplemented(src, "lambda");
}

TEST(UnsupportedExpressionTests, methodReferenceTypeThrowsNotImplemented) {
    // `Type::method` form — matches ctx->COLONCOLON() in fromContext, which we check
    // before NEW/identifier so it doesn't get swallowed by those branches.
    auto src = makeSource("int32",
        "return int32::toString();");
    expectNotImplemented(src, "method reference");
}

TEST(UnsupportedExpressionTests, methodReferenceNewThrowsNotImplemented) {
    // `ClassType::new` form — same COLONCOLON path.
    auto src = makeSource("int32",
        "return int32::new();");
    expectNotImplemented(src, "method reference");
}

TEST(UnsupportedExpressionTests, superCallThrowsNotImplemented) {
    // `obj.super.foo()` triggers the DOT branch's super-suffix sub-case.
    auto src = makeSource("int32",
        "return super.something();");
    expectNotImplemented(src, "super");
}

TEST(UnsupportedExpressionTests, innerCreatorThrowsNotImplemented) {
    // `obj.new Inner()` — DOT branch detects the innerCreator suffix.
    auto src = makeSource("int32",
        "int32 obj = 0;\n"
        "return obj.new Inner();");
    expectNotImplemented(src, "inner-class");
}

TEST(UnsupportedExpressionTests, explicitGenericInvocationThrowsNotImplemented) {
    // `obj.<T>foo()` triggers the DOT branch's explicit-generic-invocation sub-case.
    auto src = makeSource("int32",
        "int32 obj = 0;\n"
        "return obj.<int32>method();");
    expectNotImplemented(src, "explicit generic invocation");
}
