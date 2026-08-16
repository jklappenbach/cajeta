//
// Control-flow statement tests: if/else, while, for, do-while, break, continue,
// nested loops. Each test JIT-compiles a small Cajeta program and asserts on the
// returned value to verify codegen correctness end-to-end.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& returnType, const std::string& body) {
    return "package test;\n"
           "public final class C {\n"
           "    public static " + returnType + " run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- if / else ---------------------------------------------------------------





// --- while -------------------------------------------------------------------



// Restore the array-count-as-loop-bound test that was deferred.

// --- for ---------------------------------------------------------------------



// --- do-while ----------------------------------------------------------------



// --- break + continue --------------------------------------------------------





// --- if without else returning a value (else falls through to next stmt) -----

TEST(ControlFlowTests, ifElseAsExpressionViaReturns) {
    // Both branches return; the merge block is unreachable. Verifies that
    // emitting unconditional `ret`s in both branches doesn't trip the
    // missing-terminator handling in Method::generateCode.
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 7;\n"
        "if (a > 0) {\n"
        "    return 1;\n"
        "} else {\n"
        "    return 0;\n"
        "}"), "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
