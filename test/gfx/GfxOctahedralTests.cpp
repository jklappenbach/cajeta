//
// GfxOctahedralTests — cajeta-gfx plan §3 (slice 3c): octahedral unit-vector
// encoding (cajeta.gpu.Octahedral).
//
// Octahedral encoding (Cigolle et al. 2014) maps a unit 3-vector to a 2-D point
// in [-1, 1]^2 and back — the standard compact normal storage for G-buffers
// (two channels instead of three, near-uniform angular error). The sphere is
// projected onto an octahedron by the L1 norm; the lower hemisphere folds out
// across the |u|+|v|=1 diamond. The clean analytic property is the ROUND TRIP:
// decode(encode(n)) recovers n to within float precision, for both hemispheres.
//
// Pure scalar float32 math (Math.abs/max/sqrt; no vector value types), so it is
// host-testable and lowers identically on the GPU.
//
// GPU-free (pure host JIT; no device).
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

const char* IMP =
    "import cajeta.gpu.Octahedral;\n"
    "import cajeta.lang.Math;\n";

} // namespace

// 3c — the six cardinal axes round-trip exactly: ±x, ±y, ±z encode and decode
// back to themselves. (+z -> (0,0); -z -> the diamond corner (1,1).)
TEST(GfxOctahedralTests, cardinalAxesRoundTrip) {
    EXPECT_EQ(runI32(IMP,
        "        int32 axis = 0;\n"
        "        while (axis < 6) {\n"
        "            float32 nx = 0.0f;\n"
        "            float32 ny = 0.0f;\n"
        "            float32 nz = 0.0f;\n"
        "            if (axis == 0) { nx = 1.0f; }\n"
        "            if (axis == 1) { nx = 0.0f - 1.0f; }\n"
        "            if (axis == 2) { ny = 1.0f; }\n"
        "            if (axis == 3) { ny = 0.0f - 1.0f; }\n"
        "            if (axis == 4) { nz = 1.0f; }\n"
        "            if (axis == 5) { nz = 0.0f - 1.0f; }\n"
        "            float32 u = Octahedral.encodeU(nx, ny, nz);\n"
        "            float32 v = Octahedral.encodeV(nx, ny, nz);\n"
        "            float32 dx = Octahedral.decodeX(u, v);\n"
        "            float32 dy = Octahedral.decodeY(u, v);\n"
        "            float32 dz = Octahedral.decodeZ(u, v);\n"
        "            float32 ex = dx - nx;\n"
        "            if (ex < 0.0f) { ex = 0.0f - ex; }\n"
        "            if (ex > 0.005f) { return -1; }\n"
        "            float32 ey = dy - ny;\n"
        "            if (ey < 0.0f) { ey = 0.0f - ey; }\n"
        "            if (ey > 0.005f) { return -2; }\n"
        "            float32 ez = dz - nz;\n"
        "            if (ez < 0.0f) { ez = 0.0f - ez; }\n"
        "            if (ez > 0.005f) { return -3; }\n"
        "            axis = axis + 1;\n"
        "        }\n"
        "        return 0;\n"), 0);
}

// 3c — a general upper-hemisphere (nz > 0) unit vector round-trips.
TEST(GfxOctahedralTests, upperHemisphereRoundTrip) {
    EXPECT_EQ(runI32(IMP,
        "        float32 x = 1.0f;\n"
        "        float32 y = 2.0f;\n"
        "        float32 z = 3.0f;\n"
        "        float32 len = Math.sqrt(x * x + y * y + z * z);\n"
        "        float32 nx = x / len;\n"
        "        float32 ny = y / len;\n"
        "        float32 nz = z / len;\n"
        "        float32 u = Octahedral.encodeU(nx, ny, nz);\n"
        "        float32 v = Octahedral.encodeV(nx, ny, nz);\n"
        "        float32 dx = Octahedral.decodeX(u, v);\n"
        "        float32 dy = Octahedral.decodeY(u, v);\n"
        "        float32 dz = Octahedral.decodeZ(u, v);\n"
        "        float32 ex = dx - nx;\n"
        "        if (ex < 0.0f) { ex = 0.0f - ex; }\n"
        "        if (ex > 0.005f) { return -1; }\n"
        "        float32 ey = dy - ny;\n"
        "        if (ey < 0.0f) { ey = 0.0f - ey; }\n"
        "        if (ey > 0.005f) { return -2; }\n"
        "        float32 ez = dz - nz;\n"
        "        if (ez < 0.0f) { ez = 0.0f - ez; }\n"
        "        if (ez > 0.005f) { return -3; }\n"
        "        return 0;\n"), 0);
}

// 3c — a general LOWER-hemisphere (nz < 0) unit vector round-trips (exercises
// the diamond fold / octWrap path).
TEST(GfxOctahedralTests, lowerHemisphereRoundTrip) {
    EXPECT_EQ(runI32(IMP,
        "        float32 x = 1.0f;\n"
        "        float32 y = 0.0f - 2.0f;\n"
        "        float32 z = 0.0f - 2.0f;\n"
        "        float32 len = Math.sqrt(x * x + y * y + z * z);\n"
        "        float32 nx = x / len;\n"
        "        float32 ny = y / len;\n"
        "        float32 nz = z / len;\n"
        "        float32 u = Octahedral.encodeU(nx, ny, nz);\n"
        "        float32 v = Octahedral.encodeV(nx, ny, nz);\n"
        "        float32 dx = Octahedral.decodeX(u, v);\n"
        "        float32 dy = Octahedral.decodeY(u, v);\n"
        "        float32 dz = Octahedral.decodeZ(u, v);\n"
        "        float32 ex = dx - nx;\n"
        "        if (ex < 0.0f) { ex = 0.0f - ex; }\n"
        "        if (ex > 0.005f) { return -1; }\n"
        "        float32 ey = dy - ny;\n"
        "        if (ey < 0.0f) { ey = 0.0f - ey; }\n"
        "        if (ey > 0.005f) { return -2; }\n"
        "        float32 ez = dz - nz;\n"
        "        if (ez < 0.0f) { ez = 0.0f - ez; }\n"
        "        if (ez > 0.005f) { return -3; }\n"
        "        return 0;\n"), 0);
}

// 3c — encoded components stay within the [-1, 1] square, and the decoded
// vector is unit length, scanning a grid of directions over both hemispheres.
TEST(GfxOctahedralTests, encodedInRangeDecodedUnit) {
    EXPECT_EQ(runI32(IMP,
        "        int32 i = 0;\n"
        "        while (i < 512) {\n"
        "            int32 a = i % 8;\n"
        "            int32 b = (i / 8) % 8;\n"
        "            int32 c = (i / 64) % 8;\n"
        "            float32 fa = a;\n"
        "            float32 fb = b;\n"
        "            float32 fc = c;\n"
        "            float32 x = fa - 3.5f;\n"
        "            float32 y = fb - 3.5f;\n"
        "            float32 z = fc - 3.5f;\n"
        "            float32 len = Math.sqrt(x * x + y * y + z * z);\n"
        "            float32 nx = x / len;\n"
        "            float32 ny = y / len;\n"
        "            float32 nz = z / len;\n"
        "            float32 u = Octahedral.encodeU(nx, ny, nz);\n"
        "            float32 v = Octahedral.encodeV(nx, ny, nz);\n"
        "            if (u < -1.001f) { return -1; }\n"
        "            if (u > 1.001f) { return -2; }\n"
        "            if (v < -1.001f) { return -3; }\n"
        "            if (v > 1.001f) { return -4; }\n"
        "            float32 dx = Octahedral.decodeX(u, v);\n"
        "            float32 dy = Octahedral.decodeY(u, v);\n"
        "            float32 dz = Octahedral.decodeZ(u, v);\n"
        "            float32 dlen = Math.sqrt(dx * dx + dy * dy + dz * dz);\n"
        "            if (dlen < 0.99f) { return -5; }\n"
        "            if (dlen > 1.01f) { return -6; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 0;\n"), 0);
}
