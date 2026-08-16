//
// Literal expression tests: integer / float / bool / string literals are compiled
// into trivial methods that return the literal, JIT'd, and the returned value is
// asserted against the expected constant.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <cstring>

using cajeta_test::CajetaJit;

namespace {

// Smallest possible source — a static method that returns a literal.
std::string makeSource(const std::string& returnType, const std::string& body) {
    return "package test;\n"
           "public final class L {\n"
           "    public static " + returnType + " run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace




// L-suffix pins the literal's Cajeta type to int64. Without the
// suffix-aware strip, APInt(64, "8L", 10) misreads the non-digit `L`
// and produces garbage (surfaced as `8L` → 79 during method-template
// testing).
TEST(LiteralExpressionTests, integerLiteralLongSuffix) {
    auto jit = CajetaJit::compile(makeSource("int64", "return 8L;"), "test.L");
    auto fn = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 8LL);
}



// Digit-separator underscores (`1_000_000`) are stripped before APInt
// parses; the lexer accepts them per Java's syntax.

// Hex literal with L suffix.

// Binary literal.





