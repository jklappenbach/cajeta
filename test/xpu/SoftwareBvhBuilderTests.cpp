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
// native path is PrismSpatialIndexDeviceTests on the CPU backend (task 6).
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

// The Prism fixed-radius scene: 3 points along x, half-extent 0.5. The four query
// points are the exact ones PrismSpatialIndexDeviceTests uses (expecting 1/0/1/1).
TEST(SoftwareBvhBuilderTests, prismFixedRadiusScene) {
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
// (the over-count the exact-L2 refinement later prunes — Prism radiusExact).
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
