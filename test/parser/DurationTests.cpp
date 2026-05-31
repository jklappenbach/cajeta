//
// R9.2 — cajeta.time.Duration. Minimal value type: int64 nanos field,
// four static factories (ofNanos / ofMillis / ofSeconds / ofMinutes),
// one accessor (toNanos). These tests pin the factory conversions and
// round-trip the stack-construction + sret return path (which the M5
// value-return ABI work made first-class).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int64_t runI64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

} // namespace

TEST(DurationTests, ofNanosRoundTrip) {
    auto src =
        "package test;\n"
        "import cajeta.time.Duration;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Duration d = Duration.ofNanos(42);\n"
        "        return d.toNanos();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 42);
}

TEST(DurationTests, ofMillisConvertsToNanos) {
    auto src =
        "package test;\n"
        "import cajeta.time.Duration;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Duration d = Duration.ofMillis(3);\n"
        "        return d.toNanos();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 3000000);
}

TEST(DurationTests, ofSecondsConvertsToNanos) {
    auto src =
        "package test;\n"
        "import cajeta.time.Duration;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Duration d = Duration.ofSeconds(2);\n"
        "        return d.toNanos();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 2000000000);
}

TEST(DurationTests, ofMinutesConvertsToNanos) {
    auto src =
        "package test;\n"
        "import cajeta.time.Duration;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Duration d = Duration.ofMinutes(1);\n"
        "        return d.toNanos();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 60000000000LL);
}
