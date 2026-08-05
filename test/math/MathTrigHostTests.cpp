//
// MathTrigHostTests — host-side Math trig intrinsics: atan, atan2, asin,
// acos (added for stdlib-completion U6's OKLab hue math; the kernel path
// already lowered them). Pinned against libm-accurate values; 1e-12.
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

} // namespace

TEST(MathTrigHostTests, atanAsinAcosAtan2) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static boolean close(float64 a, float64 b) {\n"
        "        float64 d = a - b; if (d < 0.0) { d = -d; } return d < 0.000000000001;\n"
        "    }\n"
        "    public static int32 run() {\n"
        // atan: atan(1) = pi/4
        "        if (!D.close(Math.atan(1.0), 0.7853981633974483)) { return -1; }\n"
        "        if (!D.close(Math.atan(-2.5), -1.1902899496825317)) { return -2; }\n"
        // asin/acos: asin(0.5) = pi/6, acos(0.5) = pi/3
        "        if (!D.close(Math.asin(0.5), 0.5235987755982989)) { return -3; }\n"
        "        if (!D.close(Math.acos(0.5), 1.0471975511965979)) { return -4; }\n"
        // atan2 quadrants: (1,1) pi/4; (1,-1) 3pi/4; (-1,-1) -3pi/4; (-1,1) -pi/4
        "        if (!D.close(Math.atan2(1.0, 1.0), 0.7853981633974483)) { return -5; }\n"
        "        if (!D.close(Math.atan2(1.0, -1.0), 2.356194490192345)) { return -6; }\n"
        "        if (!D.close(Math.atan2(-1.0, -1.0), -2.356194490192345)) { return -7; }\n"
        "        if (!D.close(Math.atan2(-1.0, 1.0), -0.7853981633974483)) { return -8; }\n"
        // axis cases
        "        if (!D.close(Math.atan2(0.0, 1.0), 0.0)) { return -9; }\n"
        "        if (!D.close(Math.atan2(1.0, 0.0), 1.5707963267948966)) { return -10; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
