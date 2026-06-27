//
// benchmark-fidelity Unit 7 — string-build-concat equal-workload guard.
// The competitors (cpp/rust/go/java/python) all build BUILD_ITERS=500 copies of
// the 8-byte piece "abcdefgh" = 4000 bytes and report input_size=4000. The
// Cajeta bench previously appended 4000 times (32000 bytes), an unfair 8x more
// work. This pins the equal workload: 500 appends -> exactly 4000 bytes.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kBuildProgram =
    "package test;\n"
    "import cajeta.lang.StringBuilder;\n"
    "public final class B {\n"
    "    public static int64 buildSize(int32 n) {\n"
    "        StringBuilder sb = heap StringBuilder();\n"
    "        int32 i = 0;\n"
    "        while (i < n) { sb.append(\"abcdefgh\"); i = i + 1; }\n"
    "        String s = sb.toString();\n"
    "        return s.size();\n"
    "    }\n"
    "}\n";

} // namespace

// 7.a.1 — n=500 builds exactly 4000 bytes (the competitors' workload).
TEST(StringBuildWorkload, fiveHundredAppendsIs4000Bytes) {
    auto jit = CajetaJit::compile(kBuildProgram, "test.B");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("buildSize");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(500), 4000);
}
