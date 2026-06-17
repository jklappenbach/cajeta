// M0 OptiX device programs — AABB candidate-count (the Toffee spatial-index /
// RTNN pattern). Reproduces cajeta's kRqMinDriver semantics on the RT cores:
// a degenerate ray at each query point; count the AABB primitives the point is
// inside (anyhit accumulates + ignores so traversal visits every overlapping box).
#include <optix.h>

struct Params {
    OptixTraversableHandle handle;
    const float3* queries;   // n query origins
    unsigned int* counts;    // n outputs
    unsigned int n;
    const float* boxes;      // 6*np: minX,minY,minZ,maxX,maxY,maxZ per box
};

extern "C" { __constant__ Params params; }

extern "C" __global__ void __raygen__count() {
    unsigned int i = optixGetLaunchIndex().x;
    if (i >= params.n) return;
    float3 o = params.queries[i];
    unsigned int p0 = 0;                          // candidate count carried in payload 0
    optixTrace(params.handle, o, make_float3(0.0f, 0.0f, 1.0f),
               0.0f, 0.001f, 0.0f,                // tmin, tmax (degenerate), time
               OptixVisibilityMask(255), OPTIX_RAY_FLAG_NONE,
               0, 1, 0,                           // SBT offset, stride, miss index
               p0);
    params.counts[i] = p0;
}

extern "C" __global__ void __intersection__box() {
    unsigned int idx = optixGetPrimitiveIndex();
    float3 o = optixGetObjectRayOrigin();
    const float* b = params.boxes + idx * 6;
    if (o.x >= b[0] && o.y >= b[1] && o.z >= b[2] &&
        o.x <= b[3] && o.y <= b[4] && o.z <= b[5]) {
        optixReportIntersection(0.0f, 0);         // the point is inside this box
    }
}

extern "C" __global__ void __anyhit__count() {
    optixSetPayload_0(optixGetPayload_0() + 1u);  // count this candidate...
    optixIgnoreIntersection();                    // ...and keep traversing
}

extern "C" __global__ void __miss__nop() {}
