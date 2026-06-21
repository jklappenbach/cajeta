//
// GfxColorTests — cajeta-gfx plan §1.d (graphics math foundation), the Color
// value type in cajeta.math and the sRGB<->linear transfer functions. Color is a
// value type over the builtin Vector<float32,4> (rgba). toLinear() decodes an
// sRGB-encoded color to linear light; toSrgb() encodes linear light back to
// sRGB; alpha passes through unchanged. Reference values from the standard IEC
// 61966-2-1 piecewise transfer function. Each test JITs a program returning 0 on
// success or a negative failure code.
//
// cajeta.math is lazily parsed; importing cajeta.math.Color triggers it.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& imports, const std::string& body) {
    std::string src =
        "package test;\n"
        + imports +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + body +
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* IMP = "import cajeta.math.Color;\n";

} // namespace

// 1d — sRGB 0.5 decodes to linear ~0.2140 (the canonical mid-gray value); the
// high-segment power curve. Alpha is preserved.
TEST(GfxColorTests, srgbToLinearMidGray) {
    EXPECT_EQ(runI32(IMP,
        "        Color c = stack Color(0.5f, 0.5f, 0.5f, 0.25f);\n"
        "        Color lin = c.toLinear();\n"
        "        if (lin.r() < 0.209f || lin.r() > 0.219f) { return -1; }\n"
        "        if (lin.g() < 0.209f || lin.g() > 0.219f) { return -2; }\n"
        "        if (lin.b() < 0.209f || lin.b() > 0.219f) { return -3; }\n"
        "        if (lin.a() < 0.249f || lin.a() > 0.251f) { return -4; }\n"   // alpha unchanged
        "        return 0;\n"), 0);
}

// 1d — linear 0.2140 encodes back to sRGB ~0.5 (inverse of the above).
TEST(GfxColorTests, linearToSrgbMidGray) {
    EXPECT_EQ(runI32(IMP,
        "        Color c = stack Color(0.2140f, 0.2140f, 0.2140f, 1.0f);\n"
        "        Color srgb = c.toSrgb();\n"
        "        if (srgb.r() < 0.495f || srgb.r() > 0.505f) { return -1; }\n"
        "        if (srgb.b() < 0.495f || srgb.b() > 0.505f) { return -2; }\n"
        "        return 0;\n"), 0);
}

// 1d — the low end uses the linear segment: sRGB 0.04 -> linear 0.04/12.92 =
// 0.003096 (below the 0.04045 knee).
TEST(GfxColorTests, srgbToLinearLowSegment) {
    EXPECT_EQ(runI32(IMP,
        "        Color c = stack Color(0.04f, 0.04f, 0.04f, 1.0f);\n"
        "        Color lin = c.toLinear();\n"
        "        if (lin.r() < 0.0026f || lin.r() > 0.0036f) { return -1; }\n"
        "        return 0;\n"), 0);
}

// 1d — round-trip: toSrgb(toLinear(x)) == x within tolerance, across both the
// low (linear) and high (power) segments.
TEST(GfxColorTests, roundTripSrgbLinearSrgb) {
    EXPECT_EQ(runI32(IMP,
        "        Color c = stack Color(0.02f, 0.5f, 0.9f, 0.7f);\n"
        "        Color lin = c.toLinear();\n"
        "        Color back = lin.toSrgb();\n"
        "        if (back.r() < 0.01f || back.r() > 0.03f) { return -1; }\n"
        "        if (back.g() < 0.49f || back.g() > 0.51f) { return -2; }\n"
        "        if (back.b() < 0.89f || back.b() > 0.91f) { return -3; }\n"
        "        if (back.a() < 0.69f || back.a() > 0.71f) { return -4; }\n"
        "        return 0;\n"), 0);
}

// 1d — endpoints are fixed points: 0 -> 0 and 1 -> 1 in both directions.
TEST(GfxColorTests, endpointsAreFixedPoints) {
    EXPECT_EQ(runI32(IMP,
        "        Color black = stack Color(0.0f, 0.0f, 0.0f, 1.0f);\n"
        "        Color blackLin = black.toLinear();\n"
        "        if (blackLin.r() < -0.001f || blackLin.r() > 0.001f) { return -1; }\n"
        "        Color white = stack Color(1.0f, 1.0f, 1.0f, 1.0f);\n"
        "        Color whiteLin = white.toLinear();\n"
        "        if (whiteLin.r() < 0.999f || whiteLin.r() > 1.001f) { return -2; }\n"
        "        Color whiteSrgb = white.toSrgb();\n"
        "        if (whiteSrgb.r() < 0.999f || whiteSrgb.r() > 1.001f) { return -3; }\n"
        "        return 0;\n"), 0);
}
