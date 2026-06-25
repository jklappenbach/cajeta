//
// XpuMeshSimplifierTests — cajeta-gfx plan §2.a, the mesh-level half of the
// Garland–Heckbert edge-collapse simplifier: per-vertex quadric ACCUMULATION
// from an indexed triangle mesh (cajeta.xpu.MeshSimplifier.accumulateQuadrics),
// built on the cajeta.xpu.Qem quadric core. A vertex's accumulated quadric is
// the sum of the faceQuadric of every triangle incident to it; evaluated at the
// vertex it is ~0, and off-position it measures the summed squared distance to
// those incident planes. Each test JITs a small program returning 0 on success
// or a negative failure code, checked on ANALYTIC (cube/fan) golden meshes.
//
// cajeta.xpu is lazily parsed; importing it triggers the parse.
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
    "import cajeta.xpu.Qem;\n"
    "import cajeta.xpu.MeshSimplifier;\n";

} // namespace

// 2a — three triangles in the three axis planes (z=0, x=0, y=0) all share the
// origin vertex 0. Its accumulated quadric is therefore qx+qy+qz, the squared
// distance to the origin: error at (1,2,2) is 1+4+4 = 9, and 0 at the origin.
// This pins down that accumulation sums the right incident face planes.
TEST(XpuMeshSimplifierTests, accumulateSumsIncidentFacePlanes) {
    EXPECT_EQ(runI32(IMP,
        "        float32[] pos = heap float32[12];\n"
        "        pos[0] = 0.0f; pos[1] = 0.0f; pos[2] = 0.0f;\n"
        "        pos[3] = 1.0f; pos[4] = 0.0f; pos[5] = 0.0f;\n"
        "        pos[6] = 0.0f; pos[7] = 1.0f; pos[8] = 0.0f;\n"
        "        pos[9] = 0.0f; pos[10] = 0.0f; pos[11] = 1.0f;\n"
        "        int32[] idx = heap int32[9];\n"
        "        idx[0] = 0; idx[1] = 1; idx[2] = 2;\n"   // z = 0
        "        idx[3] = 0; idx[4] = 2; idx[5] = 3;\n"   // x = 0
        "        idx[6] = 0; idx[7] = 3; idx[8] = 1;\n"   // y = 0
        "        float32[] q = MeshSimplifier.accumulateQuadrics(pos, idx, 4);\n"
        "        Matrix<float32,4,4> q0 = MeshSimplifier.vertexQuadric(q, 0);\n"
        "        Vector<float32,3> probe = stack Vector<float32,3>(1.0f, 2.0f, 2.0f);\n"
        "        float32 e = Qem.error(q0, probe);\n"
        "        if (e < 8.99f || e > 9.01f) { return -1; }\n"
        "        Vector<float32,3> o = stack Vector<float32,3>(0.0f, 0.0f, 0.0f);\n"
        "        float32 e0 = Qem.error(q0, o);\n"
        "        if (e0 < -0.01f || e0 > 0.01f) { return -2; }\n"
        "        return 0;\n"), 0);
}

// 2a — accumulation counts EACH incident face. A flat fan in z=0 around center
// vertex 0 (4 triangles) gives vertex 0 a quadric of 4·(z=0), so error off the
// plane by h is 4h² (here h=1 → 4). The rim vertex 1 sits in only 2 of those
// triangles → 2h² (→ 2). On-plane error is 0 for both.
TEST(XpuMeshSimplifierTests, accumulationCountsEachIncidentFace) {
    EXPECT_EQ(runI32(IMP,
        "        float32[] pos = heap float32[15];\n"
        "        pos[0] = 0.0f;  pos[1] = 0.0f;  pos[2] = 0.0f;\n"
        "        pos[3] = 1.0f;  pos[4] = 0.0f;  pos[5] = 0.0f;\n"
        "        pos[6] = 0.0f;  pos[7] = 1.0f;  pos[8] = 0.0f;\n"
        "        pos[9] = -1.0f; pos[10] = 0.0f; pos[11] = 0.0f;\n"
        "        pos[12] = 0.0f; pos[13] = -1.0f; pos[14] = 0.0f;\n"
        "        int32[] idx = heap int32[12];\n"
        "        idx[0] = 0; idx[1] = 1; idx[2] = 2;\n"
        "        idx[3] = 0; idx[4] = 2; idx[5] = 3;\n"
        "        idx[6] = 0; idx[7] = 3; idx[8] = 4;\n"
        "        idx[9] = 0; idx[10] = 4; idx[11] = 1;\n"
        "        float32[] q = MeshSimplifier.accumulateQuadrics(pos, idx, 5);\n"
        "        Matrix<float32,4,4> q0 = MeshSimplifier.vertexQuadric(q, 0);\n"
        "        Matrix<float32,4,4> q1 = MeshSimplifier.vertexQuadric(q, 1);\n"
        "        Vector<float32,3> off = stack Vector<float32,3>(5.0f, 7.0f, 1.0f);\n"
        "        float32 ec = Qem.error(q0, off);\n"
        "        if (ec < 3.99f || ec > 4.01f) { return -1; }\n"     // center in 4 faces
        "        float32 er = Qem.error(q1, off);\n"
        "        if (er < 1.99f || er > 2.01f) { return -2; }\n"     // rim vertex in 2 faces
        "        Vector<float32,3> onp = stack Vector<float32,3>(5.0f, 7.0f, 0.0f);\n"
        "        float32 e0 = Qem.error(q0, onp);\n"
        "        if (e0 < -0.01f || e0 > 0.01f) { return -3; }\n"
        "        return 0;\n"), 0);
}

// 2a — collapsing an edge inside a flat (coplanar) region is free: the combined
// quadric is still pure-z, so the optimal position lies on the plane (cost 0).
// Edge (0,1) of the flat fan above.
TEST(XpuMeshSimplifierTests, flatRegionCollapsesAtZeroCost) {
    EXPECT_EQ(runI32(IMP,
        "        float32[] pos = heap float32[15];\n"
        "        pos[0] = 0.0f;  pos[1] = 0.0f;  pos[2] = 0.0f;\n"
        "        pos[3] = 1.0f;  pos[4] = 0.0f;  pos[5] = 0.0f;\n"
        "        pos[6] = 0.0f;  pos[7] = 1.0f;  pos[8] = 0.0f;\n"
        "        pos[9] = -1.0f; pos[10] = 0.0f; pos[11] = 0.0f;\n"
        "        pos[12] = 0.0f; pos[13] = -1.0f; pos[14] = 0.0f;\n"
        "        int32[] idx = heap int32[12];\n"
        "        idx[0] = 0; idx[1] = 1; idx[2] = 2;\n"
        "        idx[3] = 0; idx[4] = 2; idx[5] = 3;\n"
        "        idx[6] = 0; idx[7] = 3; idx[8] = 4;\n"
        "        idx[9] = 0; idx[10] = 4; idx[11] = 1;\n"
        "        float32[] q = MeshSimplifier.accumulateQuadrics(pos, idx, 5);\n"
        "        Matrix<float32,4,4> q0 = MeshSimplifier.vertexQuadric(q, 0);\n"
        "        Matrix<float32,4,4> q1 = MeshSimplifier.vertexQuadric(q, 1);\n"
        "        Vector<float32,3> mid = stack Vector<float32,3>(0.5f, 0.0f, 0.0f);\n"
        "        float32 cost = Qem.edgeCollapseCost(q0, q1, mid);\n"
        "        if (cost < -0.01f || cost > 0.01f) { return -1; }\n"
        "        return 0;\n"), 0);
}

// 2a — an edge collapse combines its endpoints' accumulated quadrics. On the
// orthogonal mesh, vertex 0 carries qx+qy+qz and vertex 1 (in the z=0 and y=0
// faces) carries qy+qz; the combined quadric qx+2qy+2qz has error
// 1+2+2 = 5 at (1,1,1). Its optimal collapse position is the origin (cost 0).
TEST(XpuMeshSimplifierTests, edgeCollapseCombinesEndpointQuadrics) {
    EXPECT_EQ(runI32(IMP,
        "        float32[] pos = heap float32[12];\n"
        "        pos[0] = 0.0f; pos[1] = 0.0f; pos[2] = 0.0f;\n"
        "        pos[3] = 1.0f; pos[4] = 0.0f; pos[5] = 0.0f;\n"
        "        pos[6] = 0.0f; pos[7] = 1.0f; pos[8] = 0.0f;\n"
        "        pos[9] = 0.0f; pos[10] = 0.0f; pos[11] = 1.0f;\n"
        "        int32[] idx = heap int32[9];\n"
        "        idx[0] = 0; idx[1] = 1; idx[2] = 2;\n"
        "        idx[3] = 0; idx[4] = 2; idx[5] = 3;\n"
        "        idx[6] = 0; idx[7] = 3; idx[8] = 1;\n"
        "        float32[] q = MeshSimplifier.accumulateQuadrics(pos, idx, 4);\n"
        "        Matrix<float32,4,4> q0 = MeshSimplifier.vertexQuadric(q, 0);\n"
        "        Matrix<float32,4,4> q1 = MeshSimplifier.vertexQuadric(q, 1);\n"
        "        Matrix<float32,4,4> sum = q0 + q1;\n"
        "        Vector<float32,3> probe = stack Vector<float32,3>(1.0f, 1.0f, 1.0f);\n"
        "        float32 e = Qem.error(sum, probe);\n"
        "        if (e < 4.99f || e > 5.01f) { return -1; }\n"
        "        Vector<float32,3> mid = stack Vector<float32,3>(0.5f, 0.0f, 0.0f);\n"
        "        float32 cost = Qem.edgeCollapseCost(q0, q1, mid);\n"
        "        if (cost < -0.01f || cost > 0.01f) { return -2; }\n"
        "        return 0;\n"), 0);
}

// A flat 3x3 vertex grid (z=0), triangulated into 8 triangles, with the index
// list defining the two-triangles-per-cell mesh below:
//   6--7--8
//   | /| /|
//   3--4--5
//   | /| /|
//   0--1--2
// Positions are x=i%3, y=i/3, z=0. Shared by the driver tests.
static const char* GRID_3X3 =
    "        float32[] pos = heap float32[27];\n"
    "        pos[0]=0.0f;  pos[1]=0.0f;  pos[2]=0.0f;\n"
    "        pos[3]=1.0f;  pos[4]=0.0f;  pos[5]=0.0f;\n"
    "        pos[6]=2.0f;  pos[7]=0.0f;  pos[8]=0.0f;\n"
    "        pos[9]=0.0f;  pos[10]=1.0f; pos[11]=0.0f;\n"
    "        pos[12]=1.0f; pos[13]=1.0f; pos[14]=0.0f;\n"
    "        pos[15]=2.0f; pos[16]=1.0f; pos[17]=0.0f;\n"
    "        pos[18]=0.0f; pos[19]=2.0f; pos[20]=0.0f;\n"
    "        pos[21]=1.0f; pos[22]=2.0f; pos[23]=0.0f;\n"
    "        pos[24]=2.0f; pos[25]=2.0f; pos[26]=0.0f;\n"
    "        int32[] idx = heap int32[24];\n"
    "        idx[0]=0;  idx[1]=1;  idx[2]=4;\n"
    "        idx[3]=0;  idx[4]=4;  idx[5]=3;\n"
    "        idx[6]=1;  idx[7]=2;  idx[8]=5;\n"
    "        idx[9]=1;  idx[10]=5; idx[11]=4;\n"
    "        idx[12]=3; idx[13]=4; idx[14]=7;\n"
    "        idx[15]=3; idx[16]=7; idx[17]=6;\n"
    "        idx[18]=4; idx[19]=5; idx[20]=8;\n"
    "        idx[21]=4; idx[22]=8; idx[23]=7;\n";

// 2a (driver) — simplifying a flat sheet collapses it down to (at most) the
// target triangle count at zero geometric cost: every surviving vertex stays on
// the z=0 plane (the flat quadric's optimal collapse falls back to the in-plane
// midpoint), and no degenerate triangle survives in the output.
TEST(XpuMeshSimplifierTests, simplifyReducesFlatSheetPreservingPlane) {
    EXPECT_EQ(runI32(IMP, std::string(GRID_3X3) +
        "        int32[] out = MeshSimplifier.simplify(pos, idx, 9, 2);\n"
        "        int32 rc = (int32) (out.count() / 3);\n"
        "        if (rc < 1 || rc > 2) { return -1; }\n"          // reduced 8 -> <= target
        "        int32 i = 0;\n"
        "        while (i < rc) {\n"
        "            int32 a = out[i*3];\n"
        "            int32 b = out[i*3+1];\n"
        "            int32 c = out[i*3+2];\n"
        "            if (a == b || b == c || a == c) { return -2; }\n"   // non-degenerate
        "            float32 za = pos[a*3+2];\n"
        "            float32 zb = pos[b*3+2];\n"
        "            float32 zc = pos[c*3+2];\n"
        "            if (za < -0.01f || za > 0.01f) { return -3; }\n"    // planarity preserved
        "            if (zb < -0.01f || zb > 0.01f) { return -4; }\n"
        "            if (zc < -0.01f || zc > 0.01f) { return -5; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 0;\n"), 0);
}

// 2a (driver) — simplify is a no-op when the target is already met: a target at
// or above the input triangle count leaves all 8 triangles intact.
TEST(XpuMeshSimplifierTests, simplifyIsNoOpWhenTargetMet) {
    EXPECT_EQ(runI32(IMP, std::string(GRID_3X3) +
        "        int32[] out = MeshSimplifier.simplify(pos, idx, 9, 100);\n"
        "        int32 rc = (int32) (out.count() / 3);\n"
        "        if (rc != 8) { return -1; }\n"
        "        return 0;\n"), 0);
}

// 2a (driver) — the collapse loop terminates and fully decimates at target 0:
// every triangle is eventually collapsed away, leaving an empty index list (and
// no infinite loop / crash on the degenerate extreme).
TEST(XpuMeshSimplifierTests, simplifyFullyDecimatesAtTargetZero) {
    EXPECT_EQ(runI32(IMP, std::string(GRID_3X3) +
        "        int32[] out = MeshSimplifier.simplify(pos, idx, 9, 0);\n"
        "        int32 rc = (int32) (out.count() / 3);\n"
        "        if (rc != 0) { return -1; }\n"
        "        return 0;\n"), 0);
}
