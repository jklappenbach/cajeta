// Portable software BVH (cajeta-gpu ray-query-to-core, inc 1).
//
// The software acceleration-structure noun: a flat, pointer-free BVH packed into
// one contiguous block of float32 words, so it binds/reads like any Buffer<float32>
// on every backend (here it is host memory; on a GPU it would be an uploaded
// buffer). The portable cajeta traversal (cajeta.xpu.SoftwareRayQuery) walks
// it; this builder is the build-from-description side. See
// cajeta-docs/gpu/RayQuery.md §3 for the layout contract — frozen here as v1.
//
// Self-contained pure C (no runtime globals, no backend deps), so it is both
// `#include`d into cajeta_runtime.c and compiled directly by a unit test
// (test/xpu/SoftwareBvhBuilderTests.cpp) to exercise the real builder.
//
// ALL-FLOAT32 ENCODING. The block is float32 so the cajeta traversal reads it from
// one Buffer<float32> with no bit-reinterpret (cajeta core has no uint↔float
// bitcast yet): AABB extents are floats; structural integers (counts, indices,
// escape links) are stored as exact floats — `(float)i`, recovered with the
// ordinary `(uint32)f` cast. Exact for indices < 2^24 (~16M nodes), the v1 limit;
// a uint↔float reinterpret primitive would lift it (a follow-up).
//
// Layout (all float32 words):
//   header  (CAJ_BVH_HDR_WORDS = 8):
//     [0] version 1.0   [1] nodeCount     [2] primCount   [3] rootIndex (0)
//     [4] nodesOffset   [5] primRefOffset [6] flags(bit0=hasAabbs)
//     [7] nodeStride (= CAJ_BVH_NODE_WORDS)
//   node    (CAJ_BVH_NODE_WORDS = 9 each), depth-first, left child at index+1:
//     [0..5] aabb  minX,minY,minZ, maxX,maxY,maxZ
//     [6] escape   index of the next node when this subtree is missed/finished
//                  (== nodeCount at the end → "stop")
//     [7] firstPrim  leaf: index into primRef;  interior: 0 (unused)
//     [8] primCount  leaf: prim count (>0);     interior: 0
//   primRef (primCount words): original caller primitive index per leaf slot, so
//                  candidatePrimitiveIndex() returns the caller's index.
//
// v1: AABB geometry, one primitive per leaf (each leaf's aabb is the prim's box,
// so the slab test on the node IS the primitive test). Median split on the
// longest centroid axis — correct, simple; LBVH / binned-SAH are quality
// follow-ups behind this same layout (inc 4).
#ifndef CAJETA_BVH_C
#define CAJETA_BVH_C

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CAJ_BVH_VERSION        1.0f
#define CAJ_BVH_HDR_WORDS      8u
#define CAJ_BVH_NODE_WORDS     9u
#define CAJ_BVH_FLAG_AABBS     1u
#define CAJ_BVH_FLAG_TRIANGLES 2u
// Triangle geometry stores 9 floats per primitive (3 vertices x xyz) in a
// primData region appended after primRef; its offset is primRefOffset + primCount
// (implicit, recomputed by the traversal). The leaf node's AABB (the triangle's
// bounding box) drives descent; the leaf TEST is Möller-Trumbore against these
// vertices. AABB geometry needs no primData (the leaf node's AABB IS the prim).
#define CAJ_BVH_TRI_WORDS      9u

struct caj_bvh_prim {
    float    c[3];        // centroid
    float    bmin[3];
    float    bmax[3];
    uint32_t orig;        // caller primitive index
};

static int caj_bvh_cmp_x(const void* a, const void* b) {
    float d = ((const struct caj_bvh_prim*) a)->c[0] -
              ((const struct caj_bvh_prim*) b)->c[0];
    return (d < 0.0f) ? -1 : (d > 0.0f) ? 1 : 0;
}
static int caj_bvh_cmp_y(const void* a, const void* b) {
    float d = ((const struct caj_bvh_prim*) a)->c[1] -
              ((const struct caj_bvh_prim*) b)->c[1];
    return (d < 0.0f) ? -1 : (d > 0.0f) ? 1 : 0;
}
static int caj_bvh_cmp_z(const void* a, const void* b) {
    float d = ((const struct caj_bvh_prim*) a)->c[2] -
              ((const struct caj_bvh_prim*) b)->c[2];
    return (d < 0.0f) ? -1 : (d > 0.0f) ? 1 : 0;
}

// Recursively build a threaded (stackless) BVH over prims[lo..hi) into `nodes`,
// returning this subtree's root node index. `*next` is the running node count
// (also the DFS write cursor). primRef receives the leaf order; `*pr` its cursor.
static uint32_t caj_bvh_build(struct caj_bvh_prim* prims, uint32_t lo, uint32_t hi,
                              float* nodes, uint32_t* next,
                              float* primRef, uint32_t* pr) {
    uint32_t idx = (*next)++;
    float* nd = nodes + (uint64_t) idx * CAJ_BVH_NODE_WORDS;

    // Box over [lo,hi) and centroid bounds (to pick the split axis).
    float bmin[3] = {  1e30f,  1e30f,  1e30f };
    float bmax[3] = { -1e30f, -1e30f, -1e30f };
    float cmin[3] = {  1e30f,  1e30f,  1e30f };
    float cmax[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = lo; i < hi; ++i) {
        for (int k = 0; k < 3; ++k) {
            if (prims[i].bmin[k] < bmin[k]) bmin[k] = prims[i].bmin[k];
            if (prims[i].bmax[k] > bmax[k]) bmax[k] = prims[i].bmax[k];
            if (prims[i].c[k]    < cmin[k]) cmin[k] = prims[i].c[k];
            if (prims[i].c[k]    > cmax[k]) cmax[k] = prims[i].c[k];
        }
    }
    for (int k = 0; k < 3; ++k) { nd[k] = bmin[k]; nd[3 + k] = bmax[k]; }

    if (hi - lo <= 1u) {                         // leaf: one primitive
        uint32_t first = (*pr);
        primRef[(*pr)++] = (float) prims[lo].orig;
        nd[7] = (float) first;
        nd[8] = 1.0f;
    } else {
        int axis = 0;                            // longest centroid extent
        float ext = cmax[0] - cmin[0];
        if (cmax[1] - cmin[1] > ext) { ext = cmax[1] - cmin[1]; axis = 1; }
        if (cmax[2] - cmin[2] > ext) {                          axis = 2; }
        qsort(prims + lo, hi - lo, sizeof(struct caj_bvh_prim),
              axis == 0 ? caj_bvh_cmp_x : axis == 1 ? caj_bvh_cmp_y
                                                    : caj_bvh_cmp_z);
        uint32_t mid = lo + (hi - lo) / 2u;
        caj_bvh_build(prims, lo, mid, nodes, next, primRef, pr);   // left = idx+1
        caj_bvh_build(prims, mid, hi, nodes, next, primRef, pr);
        nd[7] = 0.0f;
        nd[8] = 0.0f;                            // interior
    }
    nd[6] = (float) (*next);                     // escape = past this subtree
    return idx;
}

// Total size (in float32 words) of a built block, read from its header — the
// single source of truth both the builder and any uploader use to copy/bind the
// whole block. Used by the Vulkan software-AS path (cajeta_runtime.c) to upload a
// software BVH into a storage buffer. Handles both geometries: AABB =
// header+nodes+primRef; triangles append primCount*9 words of primData.
static uint32_t caj_bvh_block_words(const float* blk) {
    uint32_t primCount = (uint32_t) blk[2];               // [2] primCount
    uint32_t flags     = (uint32_t) blk[6];               // [6] flags
    uint32_t words     = (uint32_t) blk[5] + primCount;   // primRefOffset + primRef
    if (flags & CAJ_BVH_FLAG_TRIANGLES) words += primCount * CAJ_BVH_TRI_WORDS;
    return words;
}

// Build the software BVH over `count` AABBs (6 floats each: min.xyz,max.xyz) into
// a freshly-malloc'd float32 block; returns the block as an int64 handle (the CPU
// buffer convention — handle == host pointer), or 0 on failure. Freed by
// __cajeta_xpu_accel_free's CPU case (plain free()).
static int64_t cajeta_xpu_cpu_accel_build_aabbs(const float* boxes, uint32_t count) {
    if (!boxes || count == 0) return 0;
    uint32_t nodeCount = 2u * count - 1u;        // full binary tree, 1 prim/leaf
    uint64_t words = CAJ_BVH_HDR_WORDS +
                     (uint64_t) nodeCount * CAJ_BVH_NODE_WORDS + count;
    float* blk = (float*) calloc(words, sizeof(float));
    struct caj_bvh_prim* prims =
        (struct caj_bvh_prim*) malloc((size_t) count * sizeof(struct caj_bvh_prim));
    if (!blk || !prims) { free(blk); free(prims); return 0; }

    for (uint32_t i = 0; i < count; ++i) {
        const float* b = boxes + (uint64_t) i * 6u;
        for (int k = 0; k < 3; ++k) {
            prims[i].bmin[k] = b[k];
            prims[i].bmax[k] = b[3 + k];
            prims[i].c[k]    = 0.5f * (b[k] + b[3 + k]);
        }
        prims[i].orig = i;
    }

    uint32_t nodesOff   = CAJ_BVH_HDR_WORDS;
    uint32_t primRefOff = nodesOff + nodeCount * CAJ_BVH_NODE_WORDS;
    uint32_t next = 0u, pr = 0u;
    caj_bvh_build(prims, 0u, count, blk + nodesOff, &next, blk + primRefOff, &pr);

    blk[0] = CAJ_BVH_VERSION;
    blk[1] = (float) next;    // actual node count (== nodeCount for 1 prim/leaf)
    blk[2] = (float) count;
    blk[3] = 0.0f;            // root
    blk[4] = (float) nodesOff;
    blk[5] = (float) primRefOff;
    blk[6] = (float) CAJ_BVH_FLAG_AABBS;
    blk[7] = (float) CAJ_BVH_NODE_WORDS;

    free(prims);
    return (int64_t) (intptr_t) blk;
}

// Build the software BVH over `triCount` triangles. `verts` is a triangle soup:
// vertex `v` of triangle `t` starts at word `(t*3 + v) * stride` (stride floats
// per vertex; `stride` >= 3, tightly-packed = 3). Each leaf's AABB is the
// triangle's bounding box (drives descent); the triangle's 9 vertex floats are
// copied tightly into the primData region (indexed by the original triangle id)
// for the Möller-Trumbore leaf test. Returns the float32 block as an int64 handle
// (host pointer / CPU buffer convention), or 0 on failure.
static int64_t cajeta_xpu_cpu_accel_build_triangles(const float* verts,
                                                    uint32_t triCount,
                                                    uint32_t stride) {
    if (!verts || triCount == 0 || stride < 3u) return 0;
    uint32_t nodeCount = 2u * triCount - 1u;     // full binary tree, 1 tri/leaf
    uint64_t words = CAJ_BVH_HDR_WORDS +
                     (uint64_t) nodeCount * CAJ_BVH_NODE_WORDS +
                     triCount + (uint64_t) triCount * CAJ_BVH_TRI_WORDS;
    float* blk = (float*) calloc(words, sizeof(float));
    struct caj_bvh_prim* prims =
        (struct caj_bvh_prim*) malloc((size_t) triCount * sizeof(struct caj_bvh_prim));
    if (!blk || !prims) { free(blk); free(prims); return 0; }

    uint32_t nodesOff   = CAJ_BVH_HDR_WORDS;
    uint32_t primRefOff = nodesOff + nodeCount * CAJ_BVH_NODE_WORDS;
    uint32_t primDataOff = primRefOff + triCount;

    for (uint32_t t = 0; t < triCount; ++t) {
        const float* v0 = verts + (uint64_t)(t * 3u + 0u) * stride;
        const float* v1 = verts + (uint64_t)(t * 3u + 1u) * stride;
        const float* v2 = verts + (uint64_t)(t * 3u + 2u) * stride;
        for (int k = 0; k < 3; ++k) {
            float lo = v0[k] < v1[k] ? v0[k] : v1[k];  lo = lo < v2[k] ? lo : v2[k];
            float hi = v0[k] > v1[k] ? v0[k] : v1[k];  hi = hi > v2[k] ? hi : v2[k];
            prims[t].bmin[k] = lo;
            prims[t].bmax[k] = hi;
            prims[t].c[k]    = (v0[k] + v1[k] + v2[k]) * (1.0f / 3.0f);
        }
        prims[t].orig = t;
        // Tight 9-float copy at the original triangle index.
        float* d = blk + primDataOff + (uint64_t) t * CAJ_BVH_TRI_WORDS;
        for (int k = 0; k < 3; ++k) { d[0 + k] = v0[k]; d[3 + k] = v1[k]; d[6 + k] = v2[k]; }
    }

    uint32_t next = 0u, pr = 0u;
    caj_bvh_build(prims, 0u, triCount, blk + nodesOff, &next, blk + primRefOff, &pr);

    blk[0] = CAJ_BVH_VERSION;
    blk[1] = (float) next;
    blk[2] = (float) triCount;
    blk[3] = 0.0f;            // root
    blk[4] = (float) nodesOff;
    blk[5] = (float) primRefOff;
    blk[6] = (float) CAJ_BVH_FLAG_TRIANGLES;
    blk[7] = (float) CAJ_BVH_NODE_WORDS;

    free(prims);
    return (int64_t) (intptr_t) blk;
}

#endif // CAJETA_BVH_C
