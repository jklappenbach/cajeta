//
// M5(b) — function-pointer / method-reference types carrying sret. With the
// value-return ABI now visible through CajetaFunctionType (see
// cajeta-docs/stdlib/ValueReturns.md), a lambda whose body is a `stack X(...)`
// construction lowers to a sret-shaped function, and a method reference to a
// value-returning method (one that returnsStackValue() per the body scan)
// produces a sret-shaped function-type. The call site allocates the result
// slot in its own frame and threads it as the closure's hidden arg 0.
//
// This file covers what is reachable WITHOUT the grammar threading step that
// will land alongside source migration. The sret form is reachable only via
// inference (no LHS-pinned expected type), and today the two practical
// avenues there — `var` declarations of fn-typed locals, and the
// `(expr)(args)` postfix-call form — are not implemented for fn types in
// the parser. As a result, the end-to-end matched-ABI tests for lambda
// `stack X(...)` callbacks and method-ref direct binding land in the follow-
// up commit that introduces source-side `#R` syntax + migrates existing
// `(P) -> R` sites that mean ownership.
//
// For now, the suite holds a primitive-fn smoke that exercises the same
// closure-record + indirect-call ABI without touching the sret path, so a
// regression in the shared L2 lambda machinery would be caught here.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Smoke: explicit fn-typed local with primitive return invoked through the
// indirect-call site. Confirms the shared closure ABI (the path the sret form
// extends) still works end-to-end.
TEST(ValueReturnFnTypeTests, primitiveFnTypedLocalSmoke) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        (int32) -> int32 f = (int32 x) -> x + 1;\n"
        "        return f(41);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}
