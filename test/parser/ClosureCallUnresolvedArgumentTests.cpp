// A closure call whose argument does not resolve must report a diagnostic
// like a method call does, never abort the compiler. Found 2026-09-26 by the
// doc-snippet cleanup: `g(undef)` on a `(float32) -> float32` local ended in
// SIGABRT from a thrown `char const*`, where `D.f(undef)` reports
// CAJETA_ERROR_ARG_INVALID.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
std::string diagnosticFor(const std::string& src) {
    try {
        auto jit = CajetaJit::compile(src, "test.D");
        return "<compiled>";
    } catch (cajeta::Exception& e) {
        return e.getErrorId();
    }
}
}  // namespace

TEST(ClosureCallUnresolvedArgumentTests, methodCallReportsTheControl) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    static float32 f(float32 x) { return x * x; }\n"
        "    public static float32 run() {\n"
        "        return D.f(undef);\n"
        "    }\n"
        "}\n";
    EXPECT_NE(diagnosticFor(src), "<compiled>");
}

TEST(ClosureCallUnresolvedArgumentTests, closureCallReportsInsteadOfAborting) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static float32 run() {\n"
        "        (float32) -> float32 g = (float32 x) -> x * x;\n"
        "        float32 r = g(undef);\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    std::string id = diagnosticFor(src);
    EXPECT_NE(id, "<compiled>");
    EXPECT_FALSE(id.empty());
}
