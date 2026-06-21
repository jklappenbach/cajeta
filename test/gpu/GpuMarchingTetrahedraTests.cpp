//
// GpuMarchingTetrahedraTests — cajeta-gfx plan §2.b, the isosurface extractor
// (cajeta.gpu.MarchingTetrahedra). Marching tetrahedra is used in place of
// classic marching cubes: it is watertight and ambiguity-free BY CONSTRUCTION
// (fixed per-cube 6-tet decomposition → shared faces triangulated identically;
// no MC face-ambiguity). The extractor returns a flat triangle soup (nine
// floats per triangle). Each test JITs a small program returning 0 on success
// or a negative failure code, checked on ANALYTIC fields (empties, a single
// inside corner, and an SDF sphere whose vertices must lie on the surface).
//
// cajeta.gpu is lazily parsed; importing it triggers the parse.
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

const char* IMP = "import cajeta.gpu.MarchingTetrahedra;\n";

} // namespace

// 2b — a cell that does not straddle the iso level emits nothing: an all-outside
// grid (every corner > iso) and an all-inside grid (every corner < iso) both
// polygonize to an empty soup. Also exercises the zero-length return path.
TEST(GpuMarchingTetrahedraTests, emptyCellsEmitNoTriangles) {
    EXPECT_EQ(runI32(IMP,
        "        float32[] field = heap float32[8];\n"
        "        int32 i = 0;\n"
        "        while (i < 8) { field[i] = 1.0f; i = i + 1; }\n"   // all outside
        "        float32[] a = MarchingTetrahedra.polygonize(field, 2, 2, 2, 1.0f, 0.0f);\n"
        "        if (a.count() != 0) { return -1; }\n"
        "        int32 j = 0;\n"
        "        while (j < 8) { field[j] = -1.0f; j = j + 1; }\n"  // all inside
        "        float32[] b = MarchingTetrahedra.polygonize(field, 2, 2, 2, 1.0f, 0.0f);\n"
        "        if (b.count() != 0) { return -2; }\n"
        "        return 0;\n"), 0);
}

// 2b — one inside corner (cube corner 0 at the origin) is the local apex of all
// six tetrahedra, so each tet is case 0001 and emits one triangle: six triangles
// total, every vertex an edge midpoint inside the unit cell [0,1]^3.
TEST(GpuMarchingTetrahedraTests, singleCornerInsideEmitsSixPatches) {
    EXPECT_EQ(runI32(IMP,
        "        float32[] field = heap float32[8];\n"
        "        int32 i = 0;\n"
        "        while (i < 8) { field[i] = 1.0f; i = i + 1; }\n"
        "        field[0] = -1.0f;\n"                               // corner 0 inside
        "        float32[] tris = MarchingTetrahedra.polygonize(field, 2, 2, 2, 1.0f, 0.0f);\n"
        "        int32 tc = (int32) (tris.count() / 9);\n"
        "        if (tc != 6) { return -1; }\n"
        "        int32 vcount = (int32) (tris.count() / 3);\n"
        "        int32 v = 0;\n"
        "        while (v < vcount) {\n"
        "            float32 x = tris[v*3];\n"
        "            float32 y = tris[v*3+1];\n"
        "            float32 z = tris[v*3+2];\n"
        "            if (x < -0.01f || x > 1.01f) { return -2; }\n"
        "            if (y < -0.01f || y > 1.01f) { return -3; }\n"
        "            if (z < -0.01f || z > 1.01f) { return -4; }\n"
        "            v = v + 1;\n"
        "        }\n"
        "        return 0;\n"), 0);
}

// 2b — polygonizing an analytic SDF sphere puts every emitted vertex onto the
// surface. Field = |p - center| - radius sampled on a 9^3 grid (spacing 0.25,
// domain [0,2]^3), center (1,1,1), radius 0.6. Each vertex's squared distance to
// the center must lie within (radius ± cellSize)^2 = (0.35..0.85)^2, and the
// surface must be non-empty.
TEST(GpuMarchingTetrahedraTests, spherePolygonizesOntoSurface) {
    EXPECT_EQ(runI32(IMP,
        "        int32 N = 9;\n"
        "        float32 h = 0.25f;\n"
        "        float32 rad = 0.6f;\n"
        "        float32[] field = heap float32[729];\n"
        "        int32 k = 0;\n"
        "        while (k < N) {\n"
        "            int32 j = 0;\n"
        "            while (j < N) {\n"
        "                int32 i = 0;\n"
        "                while (i < N) {\n"
        "                    float32 x = ((float32) i) * h;\n"
        "                    float32 y = ((float32) j) * h;\n"
        "                    float32 z = ((float32) k) * h;\n"
        "                    float32 dx = x - 1.0f;\n"
        "                    float32 dy = y - 1.0f;\n"
        "                    float32 dz = z - 1.0f;\n"
        "                    float32 d = Math.sqrt(dx*dx + dy*dy + dz*dz);\n"
        "                    field[(k*N + j)*N + i] = d - rad;\n"
        "                    i = i + 1;\n"
        "                }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            k = k + 1;\n"
        "        }\n"
        "        float32[] tris = MarchingTetrahedra.polygonize(field, N, N, N, h, 0.0f);\n"
        "        int32 tc = (int32) (tris.count() / 9);\n"
        "        if (tc < 1) { return -1; }\n"
        "        int32 vcount = (int32) (tris.count() / 3);\n"
        "        int32 v = 0;\n"
        "        while (v < vcount) {\n"
        "            float32 vx = tris[v*3] - 1.0f;\n"
        "            float32 vy = tris[v*3+1] - 1.0f;\n"
        "            float32 vz = tris[v*3+2] - 1.0f;\n"
        "            float32 d2 = vx*vx + vy*vy + vz*vz;\n"
        "            if (d2 < 0.1225f || d2 > 0.7225f) { return -2; }\n"
        "            v = v + 1;\n"
        "        }\n"
        "        return 0;\n"), 0);
}
