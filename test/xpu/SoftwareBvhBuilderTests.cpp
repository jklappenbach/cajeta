//
// Software BVH builder — unit test (cajeta-gpu ray-query-to-core, inc 1).
//
// Exercises the REAL builder (runtime/native/cajeta_bvh.c, #included directly —
// it is self-contained pure C) plus a reference threaded-BVH traversal that
// re-derives the frozen layout independently. For a set of query points we assert
// the traversal's candidate set (leaves whose AABB contains the point — the
// degenerate-ray spatial-index query) matches a brute-force point-in-box scan.
//
// This proves the noun (the BVH the portable cajeta RayQuery will walk) is correct
// before any kernel-lowering integration. The cajeta @Device traversal (inc 1,
// task 4) mirrors this same walk; the end-to-end cross-check against the Vulkan
// native path is ToffeeSpatialIndexDeviceTests on the CPU backend (task 6).
//

#include "gtest/gtest.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

// The real builder, compiled into this TU. Its functions are `static`, so there
// is no link clash with the copy embedded as runtime bitcode.
#include "../../runtime/native/cajeta_bvh.c"

// The noun seam's shared impl contract (inc-4 brick #2) — the AS noun's recorded
// impl identity, mirrored by LoweringTarget::NounImpl (comment-synced).
#include "../../runtime/native/cajeta_noun_impl.h"

namespace {

// Re-derive the layout independently (do NOT reuse the builder's read path) so the
// test is a genuine second implementation of the contract. The block is all
// float32; structural integers are read with the same `(uint32)f` cast the cajeta
// traversal uses.
uint32_t u(float f) { return (uint32_t) f; }

// Threaded stackless walk: collect the caller primitive index of every leaf whose
// AABB contains `p` (inclusive) — the inc-1 degenerate-ray candidate set.
std::set<uint32_t> traverse(const float* blk, float px, float py, float pz) {
    const uint32_t nodeCnt  = u(blk[1]);
    const uint32_t nodesOff = u(blk[4]);
    const uint32_t prOff    = u(blk[5]);
    EXPECT_EQ(blk[0], CAJ_BVH_VERSION);
    EXPECT_EQ(u(blk[7]), CAJ_BVH_NODE_WORDS);

    std::set<uint32_t> hits;
    uint32_t i = 0;
    uint32_t guard = 0;
    while (i < nodeCnt) {
        if (guard++ > 4u * nodeCnt + 8u) {
            ADD_FAILURE() << "traversal failed to terminate";
            break;
        }
        const float* nd = blk + nodesOff + (uint64_t) i * CAJ_BVH_NODE_WORDS;
        float mn0 = nd[0], mn1 = nd[1], mn2 = nd[2];
        float mx0 = nd[3], mx1 = nd[4], mx2 = nd[5];
        uint32_t escape = u(nd[6]), firstPrim = u(nd[7]), primCount = u(nd[8]);
        bool inside = px >= mn0 && px <= mx0 && py >= mn1 && py <= mx1 &&
                      pz >= mn2 && pz <= mx2;
        if (inside && primCount > 0) {           // leaf hit
            for (uint32_t k = 0; k < primCount; ++k)
                hits.insert(u(blk[prOff + firstPrim + k]));
        }
        i = (inside && primCount == 0) ? i + 1   // interior hit: descend
                                       : escape;  // miss, or leaf: skip subtree
    }
    return hits;
}

// Brute force: which boxes contain the point (the oracle the BVH must match).
std::set<uint32_t> bruteForce(const std::vector<float>& boxes, uint32_t count,
                              float px, float py, float pz) {
    std::set<uint32_t> hits;
    for (uint32_t i = 0; i < count; ++i) {
        const float* b = boxes.data() + (uint64_t) i * 6u;
        if (px >= b[0] && px <= b[3] && py >= b[1] && py <= b[4] &&
            pz >= b[2] && pz <= b[5])
            hits.insert(i);
    }
    return hits;
}

// ── Triangle geometry (inc 2): Möller-Trumbore reference ─────────────────────

struct Ray { float o[3]; float d[3]; float tMin; float tMax; };

// Möller-Trumbore ray-triangle intersection over [tMin, tMax]; the oracle the
// BVH triangle leaf test must match.
bool rayTri(const Ray& r, const float* v0, const float* v1, const float* v2) {
    float e1[3], e2[3], p[3], tv[3], q[3];
    for (int k = 0; k < 3; ++k) { e1[k] = v1[k] - v0[k]; e2[k] = v2[k] - v0[k]; }
    auto cross = [](const float* a, const float* b, float* o) {
        o[0] = a[1]*b[2] - a[2]*b[1];
        o[1] = a[2]*b[0] - a[0]*b[2];
        o[2] = a[0]*b[1] - a[1]*b[0];
    };
    auto dot = [](const float* a, const float* b) {
        return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
    };
    cross(r.d, e2, p);
    float det = dot(e1, p);
    if (det > -1e-8f && det < 1e-8f) return false;     // ray parallel to triangle
    float inv = 1.0f / det;
    for (int k = 0; k < 3; ++k) tv[k] = r.o[k] - v0[k];
    float u = dot(tv, p) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    cross(tv, e1, q);
    float v = dot(r.d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    float t = dot(e2, q) * inv;
    return t >= r.tMin && t <= r.tMax;
}

// Ray vs AABB slab over [tMin,tMax] (descent test), guarded for ~0 dir components.
bool rayBox(const Ray& r, const float* mn, const float* mx) {
    float tN = r.tMin, tF = r.tMax;
    for (int k = 0; k < 3; ++k) {
        if (r.d[k] > -1e-8f && r.d[k] < 1e-8f) {
            if (r.o[k] < mn[k] || r.o[k] > mx[k]) return false;
        } else {
            float inv = 1.0f / r.d[k];
            float t0 = (mn[k] - r.o[k]) * inv, t1 = (mx[k] - r.o[k]) * inv;
            if (t0 > t1) { float tmp = t0; t0 = t1; t1 = tmp; }
            if (t0 > tN) tN = t0;
            if (t1 < tF) tF = t1;
        }
    }
    return tN <= tF;
}

// Threaded walk over a triangle BVH: descend via node-AABB slab, Möller-Trumbore
// at leaves; collect the caller triangle indices hit.
std::set<uint32_t> traverseTris(const float* blk, const Ray& r) {
    const uint32_t nodeCnt  = u(blk[1]);
    const uint32_t nodesOff = u(blk[4]);
    const uint32_t prOff    = u(blk[5]);
    const uint32_t primCnt  = u(blk[2]);
    const uint32_t dataOff  = prOff + primCnt;            // implicit primData offset
    EXPECT_EQ(u(blk[6]) & 2u, 2u) << "expected TRIANGLES geometry flag";

    std::set<uint32_t> hits;
    uint32_t i = 0, guard = 0;
    while (i < nodeCnt) {
        if (guard++ > 4u * nodeCnt + 8u) { ADD_FAILURE() << "no terminate"; break; }
        const float* nd = blk + nodesOff + (uint64_t) i * CAJ_BVH_NODE_WORDS;
        float mn[3] = {nd[0], nd[1], nd[2]}, mx[3] = {nd[3], nd[4], nd[5]};
        uint32_t escape = u(nd[6]), firstPrim = u(nd[7]), primCount = u(nd[8]);
        bool hit = rayBox(r, mn, mx);
        if (hit && primCount > 0) {                       // leaf
            for (uint32_t k = 0; k < primCount; ++k) {
                uint32_t t = u(blk[prOff + firstPrim + k]);
                const float* d = blk + dataOff + (uint64_t) t * 9u;
                if (rayTri(r, d + 0, d + 3, d + 6)) hits.insert(t);
            }
        }
        i = (hit && primCount == 0) ? i + 1 : escape;
    }
    return hits;
}

std::set<uint32_t> bruteForceTris(const std::vector<float>& verts, uint32_t triCount,
                                  const Ray& r) {
    std::set<uint32_t> hits;
    for (uint32_t t = 0; t < triCount; ++t) {
        const float* d = verts.data() + (uint64_t) t * 9u;
        if (rayTri(r, d + 0, d + 3, d + 6)) hits.insert(t);
    }
    return hits;
}

// Build half-extent `h` boxes around `points` (xyz triples) — the SpatialIndex
// box-per-point construction.
std::vector<float> boxesAround(const std::vector<float>& points, float h) {
    std::vector<float> boxes(points.size() * 2u);
    uint32_t n = (uint32_t) (points.size() / 3u);
    for (uint32_t i = 0; i < n; ++i) {
        const float* p = points.data() + (uint64_t) i * 3u;
        float* b = boxes.data() + (uint64_t) i * 6u;
        b[0] = p[0] - h; b[1] = p[1] - h; b[2] = p[2] - h;
        b[3] = p[0] + h; b[4] = p[1] + h; b[5] = p[2] + h;
    }
    return boxes;
}

void expectMatchesBruteForce(const std::vector<float>& boxes, uint32_t count,
                             const std::vector<std::array<float, 3>>& queries) {
    int64_t handle = cajeta_xpu_cpu_accel_build_aabbs(boxes.data(), count);
    ASSERT_NE(handle, 0);
    const float* blk = (const float*) (intptr_t) handle;

    EXPECT_EQ(u(blk[2]), count);                     // primCount
    EXPECT_EQ(u(blk[1]), 2u * count - 1u);           // 1 prim/leaf full tree

    for (auto& q : queries) {
        auto got = traverse(blk, q[0], q[1], q[2]);
        auto want = bruteForce(boxes, count, q[0], q[1], q[2]);
        EXPECT_EQ(got, want) << "query (" << q[0] << "," << q[1] << "," << q[2]
                             << "): BVH candidate set != brute force";
    }
    free((void*) (intptr_t) handle);
}

} // namespace

// The Toffee fixed-radius scene: 3 points along x, half-extent 0.5. The four query
// points are the exact ones ToffeeSpatialIndexDeviceTests uses (expecting 1/0/1/1).
TEST(SoftwareBvhBuilderTests, toffeeFixedRadiusScene) {
    std::vector<float> pts = {0.f,0.f,0.f, 10.f,0.f,0.f, 20.f,0.f,0.f};
    auto boxes = boxesAround(pts, 0.5f);
    std::vector<std::array<float,3>> q = {
        {{0.0f,0.f,0.f}}, {{5.0f,0.f,0.f}}, {{10.0f,0.f,0.f}}, {{19.7f,0.f,0.f}}};
    expectMatchesBruteForce(boxes, 3u, q);

    // Spot-check the actual counts match the device test's expectations.
    int64_t h = cajeta_xpu_cpu_accel_build_aabbs(boxes.data(), 3u);
    const float* blk = (const float*) (intptr_t) h;
    EXPECT_EQ(traverse(blk, 0.0f,0,0).size(), 1u);
    EXPECT_EQ(traverse(blk, 5.0f,0,0).size(), 0u);
    EXPECT_EQ(traverse(blk, 10.0f,0,0).size(), 1u);
    EXPECT_EQ(traverse(blk, 19.7f,0,0).size(), 1u);
    free((void*) (intptr_t) h);
}

// Overlapping boxes: a query point inside several boxes must return ALL of them
// (the over-count the exact-L2 refinement later prunes — Toffee radiusExact).
TEST(SoftwareBvhBuilderTests, overlappingBoxesReturnAll) {
    // Three unit boxes all containing the origin (half-extent 1.0 around 3 points).
    std::vector<float> pts = {0.f,0.f,0.f, 0.9f,0.f,0.f, 0.6f,0.6f,0.f};
    auto boxes = boxesAround(pts, 1.0f);
    int64_t h = cajeta_xpu_cpu_accel_build_aabbs(boxes.data(), 3u);
    const float* blk = (const float*) (intptr_t) h;
    auto got = traverse(blk, 0.f, 0.f, 0.f);
    EXPECT_EQ(got.size(), 3u) << "all three overlapping boxes contain the origin";
    EXPECT_EQ(got, (std::set<uint32_t>{0u, 1u, 2u}));
    free((void*) (intptr_t) h);
}

// A single primitive — the degenerate tree (root is the only leaf).
TEST(SoftwareBvhBuilderTests, singlePrimitive) {
    std::vector<float> boxes = {-1.f,-1.f,-1.f, 1.f,1.f,1.f};
    int64_t h = cajeta_xpu_cpu_accel_build_aabbs(boxes.data(), 1u);
    const float* blk = (const float*) (intptr_t) h;
    EXPECT_EQ(u(blk[1]), 1u);                                  // one node
    EXPECT_EQ(traverse(blk, 0.f,0.f,0.f), (std::set<uint32_t>{0u}));
    EXPECT_TRUE(traverse(blk, 2.f,0.f,0.f).empty());
    free((void*) (intptr_t) h);
}

// A larger, irregular 3-D cloud: every grid query must agree with brute force,
// stressing descent/skip across a real hierarchy and the primRef remap.
TEST(SoftwareBvhBuilderTests, gridCloudMatchesBruteForce) {
    std::vector<float> pts;
    for (int x = 0; x < 5; ++x)
        for (int y = 0; y < 4; ++y)
            for (int z = 0; z < 3; ++z) {
                pts.push_back((float) x * 2.0f);
                pts.push_back((float) y * 2.0f);
                pts.push_back((float) z * 2.0f);
            }
    uint32_t count = (uint32_t) (pts.size() / 3u);          // 60 points
    auto boxes = boxesAround(pts, 0.75f);                    // disjoint boxes

    std::vector<std::array<float,3>> q;
    for (float x = -1.f; x <= 9.f; x += 1.0f)
        for (float y = -1.f; y <= 7.f; y += 1.0f)
            q.push_back({{x, y, 2.0f}});                     // a z=2 slice
    q.push_back({{0.f,0.f,0.f}});                            // exact hits
    q.push_back({{8.f,6.f,4.f}});
    q.push_back({{100.f,100.f,100.f}});                     // far miss

    expectMatchesBruteForce(boxes, count, q);
}

// ── Triangle BVH (inc 2) ─────────────────────────────────────────────────────

// A small mesh of axis-spread triangles; every ray's BVH hit set (slab descent +
// Möller-Trumbore leaves) must equal brute-force Möller-Trumbore over all tris.
TEST(SoftwareBvhBuilderTests, triangleMeshMatchesBruteForce) {
    // 4 triangles, each a unit triangle in the z=const plane at z = 0,2,4,6,
    // offset along x so their bounding boxes are disjoint.
    std::vector<float> verts;
    auto tri = [&](float ox, float oy, float z) {
        float v[9] = { ox,oy,z,  ox+1.f,oy,z,  ox,oy+1.f,z };
        verts.insert(verts.end(), v, v + 9);
    };
    tri(0.f, 0.f, 0.f);    // tri 0
    tri(3.f, 0.f, 2.f);    // tri 1
    tri(6.f, 0.f, 4.f);    // tri 2
    tri(9.f, 0.f, 6.f);    // tri 3
    uint32_t triCount = 4;

    int64_t h = cajeta_xpu_cpu_accel_build_triangles(verts.data(), triCount, 3u);
    ASSERT_NE(h, 0);
    const float* blk = (const float*) (intptr_t) h;
    EXPECT_EQ(u(blk[2]), triCount);
    EXPECT_EQ(u(blk[6]) & 2u, 2u);                 // TRIANGLES flag

    std::vector<Ray> rays;
    // Down-z rays through each triangle's interior (point (ox+0.2, oy+0.2)).
    rays.push_back({{0.2f, 0.2f, 5.f}, {0.f, 0.f, -1.f}, 0.f, 100.f});  // hits tri0
    rays.push_back({{3.2f, 0.2f, 5.f}, {0.f, 0.f, -1.f}, 0.f, 100.f});  // hits tri1
    rays.push_back({{6.2f, 0.2f, 5.f}, {0.f, 0.f, -1.f}, 0.f, 100.f});  // hits tri2
    rays.push_back({{9.2f, 0.2f, 7.f}, {0.f, 0.f, -1.f}, 0.f, 100.f});  // hits tri3
    // A ray that misses every triangle (outside all in x/y).
    rays.push_back({{50.f, 50.f, 5.f}, {0.f, 0.f, -1.f}, 0.f, 100.f});
    // A ray pointing the wrong way (away from the triangles) — no hit (t < 0).
    rays.push_back({{0.2f, 0.2f, 5.f}, {0.f, 0.f, 1.f}, 0.f, 100.f});
    // A bounded ray too short to reach the triangle.
    rays.push_back({{0.2f, 0.2f, 5.f}, {0.f, 0.f, -1.f}, 0.f, 1.f});
    // Outside the barycentric triangle (in the box corner the tri doesn't cover).
    rays.push_back({{0.9f, 0.9f, 5.f}, {0.f, 0.f, -1.f}, 0.f, 100.f});

    for (size_t i = 0; i < rays.size(); ++i) {
        auto got = traverseTris(blk, rays[i]);
        auto want = bruteForceTris(verts, triCount, rays[i]);
        EXPECT_EQ(got, want) << "ray " << i
                             << ": triangle BVH hit set != brute force";
    }
    free((void*) (intptr_t) h);
}

// A denser, irregular triangle cloud swept by a fan of rays — stresses descent /
// skip across a real triangle hierarchy and the primRef remap to the leaf test.
TEST(SoftwareBvhBuilderTests, triangleCloudMatchesBruteForce) {
    std::vector<float> verts;
    uint32_t triCount = 0;
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y) {
            float ox = (float) x * 2.5f, oy = (float) y * 2.5f, z = (float) (x + y);
            float v[9] = { ox,oy,z,  ox+1.f,oy,z,  ox,oy+1.f,z };
            verts.insert(verts.end(), v, v + 9);
            ++triCount;
        }
    int64_t h = cajeta_xpu_cpu_accel_build_triangles(verts.data(), triCount, 3u);
    ASSERT_NE(h, 0);
    const float* blk = (const float*) (intptr_t) h;

    std::vector<Ray> rays;
    for (float x = -1.f; x <= 9.f; x += 0.7f)
        for (float y = -1.f; y <= 9.f; y += 0.7f)
            rays.push_back({{x, y, 20.f}, {0.f, 0.f, -1.f}, 0.f, 100.f});

    for (size_t i = 0; i < rays.size(); ++i) {
        auto got = traverseTris(blk, rays[i]);
        auto want = bruteForceTris(verts, triCount, rays[i]);
        EXPECT_EQ(got, want) << "ray " << i << " (" << rays[i].o[0] << ","
                             << rays[i].o[1] << "): mismatch";
    }
    free((void*) (intptr_t) h);
}

// Noun-impl contract (inc-4 brick #2). These ordinals are a stable runtime
// contract: AccelerationStructure.cajeta stores `impl` as this value, the Vulkan
// launch reads it from the AS POD (offset 12) to assert the noun/verb match, and
// __cajeta_xpu_accel_free dispatches free on it. They MUST stay mirrored by
// LoweringTarget::NounImpl in the compiler (comment-synced, like CAJETA_KP_*);
// renumbering silently breaks all three coupling points.
TEST(NounImplContract, ordinalsAreStable) {
    EXPECT_EQ(0, (int) CAJ_AS_IMPL_SOFTWARE_BVH);
    EXPECT_EQ(1, (int) CAJ_AS_IMPL_VULKAN_NATIVE);
}

// The default-impl policy: native iff the active backend advertises native ray
// query, else the portable software BVH (the floor). The capability-heuristic
// brick generalizes this; today it is the single source impl == backend flows from.
TEST(NounImplContract, defaultPolicyFollowsNativeAvailability) {
    EXPECT_EQ(CAJ_AS_IMPL_SOFTWARE_BVH,  caj_default_as_impl(0));
    EXPECT_EQ(CAJ_AS_IMPL_VULKAN_NATIVE, caj_default_as_impl(1));
}
