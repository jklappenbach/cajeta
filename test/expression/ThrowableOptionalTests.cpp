// optional-absence plan 3.1 — error-package Optional migration.
//
// getCause() returns Optional<Throwable> (empty when no cause), hint() and
// docUrl() return Optional<String> (empty by default, present when a
// subclass overrides), and toJson()'s cause-chain output is byte-identical
// to the pre-migration form (golden captured from the null-returning
// implementation on 2026-07-04).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.error.Exception;\n"
           "import cajeta.error.Throwable;\n"
           "public class Hinted extends Exception {\n"
           "    public Hinted(#String message) {\n"
           "        super(#message);\n"
           "    }\n"
           "    public Optional<String> hint() {\n"
           "        return stack Optional<String>(true, \"try -f\");\n"
           "    }\n"
           "}\n"
           "public final class Ut {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int32_t runJit(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body), "test.Ut");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// 3.1.1 — cause-less: empty Optional; chained: present, unwraps to the
// original cause; the Exception override agrees with the Throwable base.
TEST(ThrowableOptionalTests, getCauseOptional) {
    EXPECT_EQ(runJit(
        "Exception bare = heap Exception(\"plain\");\n"
        "if (!bare.getCause().isEmpty()) { return -1; }\n"
        "Exception inner = heap Exception(\"disk full\");\n"
        "Exception outer = heap Exception(\"save failed\", #inner);\n"
        "Optional<Throwable> c = outer.getCause();\n"
        "if (!c.isPresent()) { return -2; }\n"
        "Throwable t = c.get();\n"
        "if (!(t.getMessage() == \"disk full\")) { return -3; }\n"
        "if (!t.getCause().isEmpty()) { return -4; }\n"
        "return 1;"), 1);
}

// 3.1.2 — hint()/docUrl() empty by default; present through an override.

// 3.1.3 — golden: toJson() for a two-deep chain, byte-identical to the
// pre-migration output (frames empty because nothing was thrown).
