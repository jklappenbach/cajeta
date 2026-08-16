//
// Tests for cajeta.lang.Optional<T> — minimal surface (P6.2).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

TEST(OptionalTests, ctorAndIsPresent) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Optional<int32> opt = heap Optional<int32>(true, 42);\n"
        "        if (opt.isPresent()) { return 1; }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(OptionalTests, ctorIsEmpty) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Optional<int32> opt = heap Optional<int32>(false, 0);\n"
        "        if (opt.isEmpty()) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(OptionalTests, orElseReturnsFallbackOnEmpty) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Optional<int32> empty = heap Optional<int32>(false, 0);\n"
        "        return empty.orElse(99);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

TEST(OptionalTests, orElseReturnsValueOnPresent) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Optional<int32> opt = heap Optional<int32>(true, 7);\n"
        "        return opt.orElse(99);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Q11 (updated by optional-absence 2.2.1) — Optional.get() on an empty
// Optional throws NoOptionalValueException (recoverable, catch-by-type),
// replacing the historical untyped `throw 1` (CAJETA_ERROR_NONE_UNWRAP).
// Full coverage of the checked-get contract lives in
// OptionalCheckedGetTests; this pins the catch-and-continue shape.

