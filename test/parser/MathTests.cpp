// cajeta.lang.Math — Math.max / Math.min / Math.clamp end-to-end.
// Each is a static method-level-templated utility (docs/specification/
// MethodLevelTemplate.md). The test exercises monomorphization per
// primitive T at the call site.

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

int64_t runI64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

constexpr const char* PRELUDE =
    "package test;\n"
    "import cajeta.lang.Math;\n";

} // namespace

TEST(MathTests, maxI32PicksLarger) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Math.max(3, 7);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

TEST(MathTests, maxI32PicksFirstWhenTied) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Math.max(5, 5);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

TEST(MathTests, minI32PicksSmaller) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Math.min(3, 7);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(MathTests, maxI64Specialization) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        return Math.max(100L, 200L);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 200LL);
}

TEST(MathTests, clampWithinRange) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Math.clamp(50, 0, 100);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 50);
}

TEST(MathTests, clampBelowLowerBound) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Math.clamp(-10, 0, 100);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

TEST(MathTests, clampAboveUpperBound) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Math.clamp(999, 0, 100);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 100);
}

// Both i32 and i64 specializations in the same compilation unit —
// verifies independent monomorphization (cached per arg list).
TEST(MathTests, twoSpecializationsCoexist) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 a = Math.max(3, 7);\n"
        "        int64 b = Math.max(100L, 200L);\n"
        "        return a + (int32) b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 207);
}
