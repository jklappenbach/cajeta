//
// CastTests — tensor-plan.md Phase 1 (Increment 2): RoundingMode + the
// RoundingMode-aware float→int conversion (cajeta.math.Cast.roundToInt).
//

#include <gtest/gtest.h>

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

const char* PRE =
    "package test;\n"
    "import cajeta.math.Cast;\n"
    "import cajeta.math.RoundingMode;\n";

} // namespace

// 1c — cast honors each RoundingMode for the float→int direction (the
// implementable, meaningful rounding case). STOCHASTIC falls back to
// NEAREST_EVEN until the RNG lands.
TEST(CastTests, roundToIntModes) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        if (Cast.roundToInt<int64>(2.5, RoundingMode.NEAREST_EVEN) != 2L) { return -1; }\n"
        "        if (Cast.roundToInt<int64>(3.5, RoundingMode.NEAREST_EVEN) != 4L) { return -2; }\n"
        "        if (Cast.roundToInt<int64>(2.5, RoundingMode.NEAREST_AWAY) != 3L) { return -3; }\n"
        "        if (Cast.roundToInt<int64>(-2.5, RoundingMode.NEAREST_AWAY) != -3L) { return -4; }\n"
        "        if (Cast.roundToInt<int64>(-2.5, RoundingMode.FLOOR) != -3L) { return -5; }\n"
        "        if (Cast.roundToInt<int64>(-2.5, RoundingMode.CEIL) != -2L) { return -6; }\n"
        "        if (Cast.roundToInt<int64>(2.7, RoundingMode.TRUNCATE) != 2L) { return -7; }\n"
        "        if (Cast.roundToInt<int64>(-2.7, RoundingMode.TRUNCATE) != -2L) { return -8; }\n"
        "        if (Cast.roundToInt<int64>(2.2, RoundingMode.CEIL) != 3L) { return -9; }\n"
        "        if (Cast.roundToInt<int64>(2.9, RoundingMode.FLOOR) != 2L) { return -10; }\n"
        "        if (Cast.roundToInt<int64>(2.5, RoundingMode.STOCHASTIC) != 2L) { return -11; }\n" // falls back to NEAREST_EVEN
        // output dtype generic over I — int32 result
        "        if (Cast.roundToInt<int32>(2.5, RoundingMode.NEAREST_AWAY) != 3) { return -12; }\n"
        "        if (Cast.roundToInt<int32>(-1.5, RoundingMode.NEAREST_EVEN) != -2) { return -13; }\n" // tie→even (-2)
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
