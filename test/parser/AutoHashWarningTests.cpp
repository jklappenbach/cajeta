//
// CAJETA_WARN_HASH_EQUALS_MISMATCH must respect @AutoHash — the
// warning fires during the visitor's body walk, BEFORE
// tryGeneratePrototype() runs synthesizeAutoHash(), so it can't see
// the synthesized hash() and told @AutoHash users to add @AutoHash
// (tour OperatorOverloadDemo). The check must treat the annotation
// (and @Data / @Value, which imply it) as satisfying hash().
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <sstream>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Compile with std::cerr captured; return the captured text.
std::string compileCapturingCerr(const std::string& src) {
    std::ostringstream captured;
    std::streambuf* old = std::cerr.rdbuf(captured.rdbuf());
    try {
        auto jit = CajetaJit::compile(src, "test.D");
    } catch (...) {
        std::cerr.rdbuf(old);
        throw;
    }
    std::cerr.rdbuf(old);
    return captured.str();
}

std::string vecSrc(const std::string& classAnnotation,
                   const std::string& extraMembers) {
    return
        "package test;\n"
        + classAnnotation +
        "public class Vec {\n"
        "    public int32 x;\n"
        "    public Vec() { return; }\n"
        "    public static boolean operator==(Vec a, Vec b) {\n"
        "        return a.x == b.x;\n"
        "    }\n"
        + extraMembers +
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
}

const char* kWarn = "CAJETA_WARN_HASH_EQUALS_MISMATCH";

} // namespace

// Control: operator== with no hash() and no annotation warns.
TEST(AutoHashWarningTests, warnsWithoutHashOrAnnotation) {
    EXPECT_NE(compileCapturingCerr(vecSrc("", "")).find(kWarn),
              std::string::npos);
}

// @AutoHash synthesizes hash() — no warning.
TEST(AutoHashWarningTests, autoHashSuppressesWarning) {
    EXPECT_EQ(compileCapturingCerr(vecSrc("@AutoHash\n", "")).find(kWarn),
              std::string::npos);
}

// Manual hash() — no warning (existing behavior, guards the capture).
TEST(AutoHashWarningTests, manualHashSuppressesWarning) {
    EXPECT_EQ(compileCapturingCerr(vecSrc("",
        "    public int64 hash() { return (int64) this.x; }\n")).find(kWarn),
              std::string::npos);
}
