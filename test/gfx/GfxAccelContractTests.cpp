//
// GfxAccelContractTests — cajeta-accel plan unit 1: the acceleration-structure
// contract + header tag + Reference adapter (cajeta.gpu.Bvh + cajeta.gpu.Strategy).
//
// Spec docs/specs/cajeta-accel-spec.md §2 (contract) + §3.4-3.5 (the Reference
// = Exact@2 oracle). The Bvh facade wraps a frozen BVH block tagged with its
// encoding STRATEGY and branching WIDTH; with no override, build() delegates to
// the existing Lbvh (the binary full-float block) and tags it Exact/2 — the
// correctness oracle every other (strategy, width) cell is checked against.
// closestHit() drives the same stackless threaded walk the shipped
// SoftwareRayQuery traverses on device.
//
// Cross-checks: Bvh.closestHit matches a brute-force analytic scan for every ray
// (1.1.1 / 1.3.1); the header is readable without decoding nodes (1.1.2); the
// default build is byte-identical to Lbvh.build (1.1.3, the faithful pass-through
// — the @Accel(Reference) alias lands in unit 5). Pure host JIT, no device.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32Full(const std::string& imports, const std::string& members) {
    std::string src =
        "package test;\n"
        + imports +
        "public final class D {\n"
        + members +
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* IMP =
    "import cajeta.gpu.Bvh;\n"
    "import cajeta.gpu.Strategy;\n"
    "import cajeta.gpu.Lbvh;\n";

// A 4-box scene (unit cubes centred at x = 0, 2, 4, 6) plus a brute-force nearest
// scan — the analytic oracle. Reused across the contract tests.
const std::string HELPERS =
    "    public static #float32[] scene() {\n"
    "        float32[] b = heap float32[24];\n"
    "        b[0]=0.0f-0.5f; b[1]=0.0f-0.5f; b[2]=0.0f-0.5f; b[3]=0.5f; b[4]=0.5f; b[5]=0.5f;\n"
    "        b[6]=1.5f; b[7]=0.0f-0.5f; b[8]=0.0f-0.5f; b[9]=2.5f; b[10]=0.5f; b[11]=0.5f;\n"
    "        b[12]=3.5f; b[13]=0.0f-0.5f; b[14]=0.0f-0.5f; b[15]=4.5f; b[16]=0.5f; b[17]=0.5f;\n"
    "        b[18]=5.5f; b[19]=0.0f-0.5f; b[20]=0.0f-0.5f; b[21]=6.5f; b[22]=0.5f; b[23]=0.5f;\n"
    "        return #b;\n"
    "    }\n"
    "    public static float32 boxEntry(\n"
    "            float32 ox, float32 oy, float32 oz, float32 dx, float32 dy, float32 dz,\n"
    "            float32 tmin, float32 tmax,\n"
    "            float32 minx, float32 miny, float32 minz,\n"
    "            float32 maxx, float32 maxy, float32 maxz) {\n"
    "        float32 miss = tmax + 1.0f;\n"
    "        float32 tnear = tmin; float32 tfar = tmax;\n"
    "        if (dx < 0.0001f && dx > 0.0f - 0.0001f) {\n"
    "            if (ox < minx) { return miss; } if (ox > maxx) { return miss; }\n"
    "        } else {\n"
    "            float32 inv = 1.0f / dx; float32 t0 = (minx - ox) * inv; float32 t1 = (maxx - ox) * inv;\n"
    "            if (t0 > t1) { float32 tmp = t0; t0 = t1; t1 = tmp; }\n"
    "            if (t0 > tnear) { tnear = t0; } if (t1 < tfar) { tfar = t1; }\n"
    "        }\n"
    "        if (dy < 0.0001f && dy > 0.0f - 0.0001f) {\n"
    "            if (oy < miny) { return miss; } if (oy > maxy) { return miss; }\n"
    "        } else {\n"
    "            float32 inv = 1.0f / dy; float32 t0 = (miny - oy) * inv; float32 t1 = (maxy - oy) * inv;\n"
    "            if (t0 > t1) { float32 tmp = t0; t0 = t1; t1 = tmp; }\n"
    "            if (t0 > tnear) { tnear = t0; } if (t1 < tfar) { tfar = t1; }\n"
    "        }\n"
    "        if (dz < 0.0001f && dz > 0.0f - 0.0001f) {\n"
    "            if (oz < minz) { return miss; } if (oz > maxz) { return miss; }\n"
    "        } else {\n"
    "            float32 inv = 1.0f / dz; float32 t0 = (minz - oz) * inv; float32 t1 = (maxz - oz) * inv;\n"
    "            if (t0 > t1) { float32 tmp = t0; t0 = t1; t1 = tmp; }\n"
    "            if (t0 > tnear) { tnear = t0; } if (t1 < tfar) { tfar = t1; }\n"
    "        }\n"
    "        if (tnear <= tfar) { return tnear; }\n"
    "        return miss;\n"
    "    }\n"
    "    public static int32 brute(float32[] boxes, int32 count,\n"
    "            float32 ox, float32 oy, float32 oz, float32 dx, float32 dy, float32 dz,\n"
    "            float32 tmin, float32 tmax) {\n"
    "        float32 bestT = tmax; int32 bestPrim = 0 - 1; int32 i = 0;\n"
    "        while (i < count) {\n"
    "            int32 b6 = i * 6;\n"
    "            float32 nx = boxes[b6+0]; float32 ny = boxes[b6+1]; float32 nz = boxes[b6+2];\n"
    "            float32 xx = boxes[b6+3]; float32 xy = boxes[b6+4]; float32 xz = boxes[b6+5];\n"
    "            float32 e = D.boxEntry(ox,oy,oz,dx,dy,dz,tmin,bestT, nx,ny,nz,xx,xy,xz);\n"
    "            if (e < bestT) { bestT = e; bestPrim = i; }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return bestPrim;\n"
    "    }\n";

} // namespace

// 1.1.2 — the handle is self-describing: strategy / width / primCount / nodeCount
// readable without decoding nodes. The no-override build is Exact @ width 2 over 4
// prims (a full binary tree → 2n-1 = 7 nodes).
TEST(GfxAccelContractTests, headerTagReadable) {
    EXPECT_EQ(runI32Full(IMP, HELPERS +
        "    public static int32 run() {\n"
        "        float32[] boxes = D.scene();\n"
        "        Bvh b = Bvh.build(boxes, 4);\n"
        "        if (b.strategy() != Strategy.Exact) { return -1; }\n"
        "        if (b.width() != 2) { return -2; }\n"
        "        if (b.primCount() != 4) { return -3; }\n"
        "        if (b.nodeCount() != 7) { return -4; }\n"
        "        return 0;\n"
        "    }\n"), 0);
}

// 1.1.1 / 1.3.1 — closestHit matches the brute-force oracle for a spread of rays,
// and returns the analytically correct primitive: a -z ray over each box centre
// hits that box; an off-scene ray misses (-1).
TEST(GfxAccelContractTests, closestHitMatchesBrute) {
    EXPECT_EQ(runI32Full(IMP, HELPERS +
        "    public static int32 run() {\n"
        "        float32[] boxes = D.scene();\n"
        "        Bvh b = Bvh.build(boxes, 4);\n"
        "        float32 dnz = 0.0f - 1.0f;\n"
        // box 0
        "        int32 p0 = b.closestHit(0.0f,0.0f,5.0f, 0.0f,0.0f,dnz, 0.0f,100.0f);\n"
        "        if (p0 != D.brute(boxes,4, 0.0f,0.0f,5.0f, 0.0f,0.0f,dnz, 0.0f,100.0f)) { return -1; }\n"
        "        if (p0 != 0) { return -2; }\n"
        // box 2
        "        int32 p2 = b.closestHit(4.0f,0.0f,5.0f, 0.0f,0.0f,dnz, 0.0f,100.0f);\n"
        "        if (p2 != D.brute(boxes,4, 4.0f,0.0f,5.0f, 0.0f,0.0f,dnz, 0.0f,100.0f)) { return -3; }\n"
        "        if (p2 != 2) { return -4; }\n"
        // box 3
        "        int32 p3 = b.closestHit(6.0f,0.0f,5.0f, 0.0f,0.0f,dnz, 0.0f,100.0f);\n"
        "        if (p3 != D.brute(boxes,4, 6.0f,0.0f,5.0f, 0.0f,0.0f,dnz, 0.0f,100.0f)) { return -5; }\n"
        "        if (p3 != 3) { return -6; }\n"
        // miss
        "        int32 pm = b.closestHit(10.0f,0.0f,5.0f, 0.0f,0.0f,dnz, 0.0f,100.0f);\n"
        "        if (pm != D.brute(boxes,4, 10.0f,0.0f,5.0f, 0.0f,0.0f,dnz, 0.0f,100.0f)) { return -7; }\n"
        "        if (pm != 0 - 1) { return -8; }\n"
        "        return 0;\n"
        "    }\n"), 0);
}

// 1.1.3 — the no-override build is a faithful pass-through: byte-identical to
// Lbvh.build over the same primitives (the full 75-word block: 8 header + 7*9
// nodes + 4 primRefs). (@Accel(Reference) alias is verified in unit 5.)
TEST(GfxAccelContractTests, defaultBuildByteIdenticalToLbvh) {
    EXPECT_EQ(runI32Full(IMP, HELPERS +
        "    public static int32 run() {\n"
        "        float32[] boxes = D.scene();\n"
        "        Bvh b = Bvh.build(boxes, 4);\n"
        "        float32[] ref = Lbvh.build(boxes, 4);\n"
        "        int32 total = 8 + 7 * 9 + 4;\n"
        "        int32 i = 0;\n"
        "        while (i < total) {\n"
        "            if (b.word(i) != ref[i]) { return 0 - (100 + i); }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"), 0);
}
