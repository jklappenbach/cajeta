// PARKED 2026-08-04 (stdlib-completion U6, blocked): these tests are ready
// but the implementation cannot land — a stdlib method returning a record
// other than its own type mis-compiles the ENTIRE stdlib bundle (even
// unrelated Color tests crash). Defect: specs/record-cross-type-return-spec.md.
// The finished implementation files are preserved in the defect spec's
// appendix reference; restore this file to test/math/ when the defect closes.
//
//
// OklabTests — stdlib-completion plan Unit 6: OKLab on cajeta.math.Color
// (spec §6, §9.7). sRGB <-> OKLab <-> OKLCh, deltaE (Euclidean in OKLab),
// and gamut DETECTION — an out-of-gamut conversion result is queryable,
// never silently clipped.
//
// Reference = Björn Ottosson's published OKLab matrices, evaluated in
// float64 by tools/fixtures/gen_oklab.py (no scipy — the oracle is the
// published constants). Primaries and white/black are exact under the
// float32 sRGB transfer (0 and 1 encode losslessly), pinned at 1e-6;
// the full sRGB round trip is STATED at 1e-5 (float32 channels bound it).
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
    "import cajeta.math.Color;\n"
    "import cajeta.math.Oklab;\n"
    "import cajeta.math.Oklch;\n";

const char* HELPERS =
    "public final class D {\n"
    "    public static boolean near(float64 a, float64 b, float64 tol) {\n"
    "        float64 d = a - b; if (d < 0.0) { d = -d; } return d < tol;\n"
    "    }\n";

} // namespace

// 6.1.2 — white, black, and the primaries match the published OKLab
// coordinates.
TEST(OklabTests, referenceValues) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Oklab w = Oklab.fromColor(stack Color(1.0f, 1.0f, 1.0f, 1.0f));\n"
        "        if (!D.near(w.l, 1.0, 0.000001)) { return -1; }\n"
        "        if (!D.near(w.a, 0.0, 0.000001)) { return -2; }\n"
        "        if (!D.near(w.b, 0.0, 0.000001)) { return -3; }\n"
        "        Oklab k = Oklab.fromColor(stack Color(0.0f, 0.0f, 0.0f, 1.0f));\n"
        "        if (k.l != 0.0 || k.a != 0.0 || k.b != 0.0) { return -4; }\n"
        "        Oklab r = Oklab.fromColor(stack Color(1.0f, 0.0f, 0.0f, 1.0f));\n"
        "        if (!D.near(r.l, 0.6279553606145516, 0.000001)) { return -5; }\n"
        "        if (!D.near(r.a, 0.22486306106597403, 0.000001)) { return -6; }\n"
        "        if (!D.near(r.b, 0.12584629853073506, 0.000001)) { return -7; }\n"
        "        Oklab g = Oklab.fromColor(stack Color(0.0f, 1.0f, 0.0f, 1.0f));\n"
        "        if (!D.near(g.l, 0.8664396115356694, 0.000001)) { return -8; }\n"
        "        if (!D.near(g.a, -0.23388757418790815, 0.000001)) { return -9; }\n"
        "        Oklab u = Oklab.fromColor(stack Color(0.0f, 0.0f, 1.0f, 1.0f));\n"
        "        if (!D.near(u.l, 0.4520137183853429, 0.000001)) { return -10; }\n"
        "        if (!D.near(u.b, -0.31152814767837517, 0.000001)) { return -11; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 6.1.1 — sRGB -> OKLab -> sRGB round-trips within the STATED tolerance
// (1e-5 on each channel; the float32 sRGB transfer bounds it).
TEST(OklabTests, roundTripWithinStatedTolerance) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static boolean rt(float32 r, float32 g, float32 b) {\n"
        "        Color c = stack Color(r, g, b, 1.0f);\n"
        "        Color back = Color.fromOklab(Oklab.fromColor(c), 1.0f);\n"
        "        if (!D.near((float64) back.r(), (float64) r, 0.00001)) { return false; }\n"
        "        if (!D.near((float64) back.g(), (float64) g, 0.00001)) { return false; }\n"
        "        if (!D.near((float64) back.b(), (float64) b, 0.00001)) { return false; }\n"
        "        return true;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        if (!D.rt(0.5f, 0.25f, 0.1f)) { return -1; }\n"
        "        if (!D.rt(0.9f, 0.9f, 0.05f)) { return -2; }\n"
        "        if (!D.rt(0.02f, 0.4f, 0.83f)) { return -3; }\n"
        "        if (!D.rt(1.0f, 1.0f, 1.0f)) { return -4; }\n"
        "        if (!D.rt(0.0f, 0.0f, 0.0f)) { return -5; }\n"
        "        if (!D.rt(0.33f, 0.33f, 0.33f)) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 6.1.3 — deltaE (Euclidean in OKLab) between red and blue matches the
// reference.
TEST(OklabTests, deltaEMatchesReference) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Oklab r = Oklab.fromColor(stack Color(1.0f, 0.0f, 0.0f, 1.0f));\n"
        "        Oklab u = Oklab.fromColor(stack Color(0.0f, 0.0f, 1.0f, 1.0f));\n"
        "        if (!D.near(Oklab.deltaE(r, u), 0.53708981869576, 0.000001)) { return -1; }\n"
        "        if (Oklab.deltaE(r, r) != 0.0) { return -2; }\n"
        "        if (!D.near(Oklab.deltaE(r, u), Oklab.deltaE(u, r), 0.0)) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 6.1.4 — OKLCh polar conversion round-trips; hue lands in [0, 360) and
// wraps correctly (blue's hue is ~264°, i.e. a negative atan2 wrapped up).
TEST(OklabTests, oklchRoundTripAndHueWrap) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Oklab u = Oklab.fromColor(stack Color(0.0f, 0.0f, 1.0f, 1.0f));\n"
        "        Oklch p = Oklch.fromLab(u);\n"
        "        if (!D.near(p.l, 0.4520137183853429, 0.000001)) { return -1; }\n"
        "        if (!D.near(p.c, 0.3132143716646012, 0.000001)) { return -2; }\n"
        "        if (!D.near(p.h, 264.052020638055, 0.0001)) { return -3; }\n"
        "        if (p.h < 0.0 || p.h >= 360.0) { return -4; }\n"
        "        Oklab back = Oklab.fromLch(p);\n"
        "        if (!D.near(back.a, u.a, 0.0000000001)) { return -5; }\n"
        "        if (!D.near(back.b, u.b, 0.0000000001)) { return -6; }\n"
        // hue for a color in the first quadrant stays put
        "        Oklab r = Oklab.fromColor(stack Color(1.0f, 0.0f, 0.0f, 1.0f));\n"
        "        Oklch rp = Oklch.fromLab(r);\n"
        "        if (rp.h < 0.0 || rp.h >= 90.0) { return -7; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 6.1.5 — an out-of-gamut result is DETECTABLE, not silently clipped:
// Oklch(0.7, 0.3, 30°) leaves sRGB; fromOklab keeps the out-of-range
// channels and inSrgbGamut() reports them.
TEST(OklabTests, outOfGamutDetectable) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Oklch loud = stack Oklch(0.7, 0.3, 30.0);\n"
        "        Color c = Color.fromOklab(Oklab.fromLch(loud), 1.0f);\n"
        "        if (c.inSrgbGamut()) { return -1; }\n"
        // channels were NOT clipped into [0,1]
        "        boolean anyOutside = false;\n"
        "        if (c.r() < 0.0f || c.r() > 1.0f) { anyOutside = true; }\n"
        "        if (c.g() < 0.0f || c.g() > 1.0f) { anyOutside = true; }\n"
        "        if (c.b() < 0.0f || c.b() > 1.0f) { anyOutside = true; }\n"
        "        if (!anyOutside) { return -2; }\n"
        // an ordinary color is in gamut
        "        Color ok = stack Color(0.5f, 0.25f, 0.1f, 1.0f);\n"
        "        if (!ok.inSrgbGamut()) { return -3; }\n"
        "        if (!Color.fromOklab(Oklab.fromColor(ok), 1.0f).inSrgbGamut()) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
