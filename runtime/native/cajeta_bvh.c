// Portable software BVH: a flat, pointer-free tree packed into one contiguous
// block of float32 words, so it binds like any Buffer<float32> on every backend.
// Structural integers ride as exact floats — no uint↔float bitcast exists yet.
// Layout, all float32 words (v1, frozen):
//   header (8): [0] version [1] nodeCount [2] primCount [3] rootIndex
//               [4] nodesOffset [5] primRefOffset [6] flags [7] nodeStride
//   node   (9 each), depth-first, left child at index+1:
//               [0..5] aabb min.xyz, max.xyz
//               [6] escape: the next node on a miss, or nodeCount to stop
//               [7] firstPrim (leaf: index into primRef) [8] primCount (leaf: >0)
//   primRef (primCount): the caller's original primitive index per leaf slot
//   primData (triangles only, 9 per prim): the 3 vertices, for Möller-Trumbore
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
// primData starts at primRefOffset + primCount, recomputed by the traversal.
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

// Recursively builds a threaded (stackless) subtree over prims[lo..hi), returning
// its root index; `*next` is the node count and the DFS cursor, `*pr` primRef's.
static uint32_t caj_bvh_build(struct caj_bvh_prim* prims, uint32_t lo, uint32_t hi,
                              float* nodes, uint32_t* next,
                              float* primRef, uint32_t* pr) {
    uint32_t idx = (*next)++;
    float* nd = nodes + (uint64_t) idx * CAJ_BVH_NODE_WORDS;

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

// Total float32 words of a built block, read from its own header — the one size
// both the builder and any uploader bind. Covers both geometries.
static uint32_t caj_bvh_block_words(const float* blk) {
    uint32_t primCount = (uint32_t) blk[2];               // [2] primCount
    uint32_t flags     = (uint32_t) blk[6];               // [6] flags
    uint32_t words     = (uint32_t) blk[5] + primCount;   // primRefOffset + primRef
    if (flags & CAJ_BVH_FLAG_TRIANGLES) words += primCount * CAJ_BVH_TRI_WORDS;
    return words;
}

// Builds over `count` AABBs (6 floats each) into a fresh block, returned as an
// int64 handle (CPU convention: handle == host pointer), or 0 on failure.
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

// Builds over `triCount` triangles of a soup: vertex `v` of triangle `t` starts at
// word `(t*3 + v) * stride`, stride >= 3 floats per vertex. Returns the block as an
// int64 handle, or 0 on failure; the vertices are copied tightly into primData.
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
