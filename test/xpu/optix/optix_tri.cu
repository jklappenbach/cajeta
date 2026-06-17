// M0 OptiX device programs — triangle nearest-hit + committed getters.
// Reproduces cajeta's nearestHit/candidateGetters semantics on the RT cores:
// built-in triangle intersection; closesthit reports the committed (nearest) T,
// primitive index, and barycentrics. Scene: two triangles at z=2 (prim 0) and
// z=4 (prim 1); a ray from (0.25,0.25,10) straight down commits the nearest
// (z=4, prim 1, t=6) with barycentrics u=v=0.25.
#include <optix.h>

struct ParamsTri {
    OptixTraversableHandle handle;
    float*        outT;     // committed t
    unsigned int* outPrim;  // committed primitive index
    float*        outU;     // committed barycentric u
    float*        outV;     // committed barycentric v
    unsigned int* outHit;   // 1 if committed triangle, 0 if miss
};

extern "C" { __constant__ ParamsTri params; }

extern "C" __global__ void __raygen__nearest() {
    if (optixGetLaunchIndex().x != 0) return;
    optixTrace(params.handle, make_float3(0.25f, 0.25f, 10.0f),
               make_float3(0.0f, 0.0f, -1.0f),
               0.0f, 100.0f, 0.0f,
               OptixVisibilityMask(255), OPTIX_RAY_FLAG_NONE, 0, 1, 0);
}

extern "C" __global__ void __closesthit__nearest() {
    float2 b = optixGetTriangleBarycentrics();   // (b1, b2) == cajeta (u, v)
    params.outT[0]    = optixGetRayTmax();        // committed t at closesthit
    params.outPrim[0] = optixGetPrimitiveIndex();
    params.outU[0]    = b.x;
    params.outV[0]    = b.y;
    params.outHit[0]  = 1u;
}

extern "C" __global__ void __miss__nearest() {
    params.outHit[0] = 0u;
}
