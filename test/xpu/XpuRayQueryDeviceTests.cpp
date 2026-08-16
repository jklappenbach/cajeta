//
// XPU ray-query device tests — the RayQuery / AccelerationStructure path across
// backends: CPU software BVH, Vulkan, NVPTX/OptiX, plus the impl-tier override
// and fallback rules. The CPU software leg is the reference every device leg
// must match.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"   // portable setenv/unsetenv on MinGW
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/amd/HipDriver.h"
#include "cajeta/xpu/nvidia/CudaDriver.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using cajeta_test::CajetaJit;
using cajeta::xpu::vulkan::VulkanDriver;

// Minimal self-contained CPU ray-query exec (self-contained): build an AccelerationStructure over 3 AABBs and run a RayQuery walk
// in a kernel on the CPU software path. Directly exercises the ray-query-to-core
// integration (software BVH builder + SoftwareRayQuery walk) without the broader
// stdlib closure. Same 1/0/1/1 expectation as the fixed-radius scene it replaced.
const char* kRqMinDriver =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class RqMin {\n"
    "    @Kernel\n"
    "    public static void countHits(AccelerationStructure scene,\n"
    "                                 KernelBuffer<float32> qx, KernelBuffer<float32> qy,\n"
    "                                 KernelBuffer<float32> qz, KernelBuffer<uint32> out,\n"
    "                                 uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          qx[i], qy[i], qz[i], 0.0f,\n"
    "                          0.0f, 0.0f, 1.0f, 0.001f);\n"
    "            uint32 c = 0;\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 1) { c = c + 1; }\n"
    "            }\n"
    "            out[i] = c;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 np = 3;\n"
    "        float32[] boxes = heap float32[np * 6];\n"
    "        boxes[0]=-0.5f;  boxes[1]=-0.5f; boxes[2]=-0.5f;  boxes[3]=0.5f;  boxes[4]=0.5f; boxes[5]=0.5f;\n"
    "        boxes[6]=9.5f;   boxes[7]=-0.5f; boxes[8]=-0.5f;  boxes[9]=10.5f; boxes[10]=0.5f; boxes[11]=0.5f;\n"
    "        boxes[12]=19.5f; boxes[13]=-0.5f; boxes[14]=-0.5f; boxes[15]=20.5f; boxes[16]=0.5f; boxes[17]=0.5f;\n"
    "        AccelerationStructure scene = heap AccelerationStructure(boxes, np);\n"
    "        uint32 n = 4;\n"
    "        float32[] hqx = heap float32[n]; float32[] hqy = heap float32[n]; float32[] hqz = heap float32[n];\n"
    "        uint32[] hout = heap uint32[n];\n"
    "        hqx[0]=0.0f;  hqy[0]=0.0f; hqz[0]=0.0f;\n"
    "        hqx[1]=5.0f;  hqy[1]=0.0f; hqz[1]=0.0f;\n"
    "        hqx[2]=10.0f; hqy[2]=0.0f; hqz[2]=0.0f;\n"
    "        hqx[3]=19.7f; hqy[3]=0.0f; hqz[3]=0.0f;\n"
    "        hout[0]=9; hout[1]=9; hout[2]=9; hout[3]=9;\n"
    "        KernelBuffer<float32> qx = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<float32> qy = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<float32> qz = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<uint32> out = heap KernelBuffer<uint32>(n);\n"
    "        qx.upload(hqx); qy.upload(hqy); qz.upload(hqz); out.upload(hout);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        countHits.launch(s, grid: [1], block: [64])(scene, qx, qy, qz, out, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        if (hout[0] != 1) { return 100; }\n"
    "        if (hout[1] != 0) { return 101; }\n"
    "        if (hout[2] != 1) { return 102; }\n"
    "        if (hout[3] != 1) { return 103; }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

// Triangle ray query (inc 2): build a triangle BVH and cast rays that hit / miss
// each triangle, confirming the triangle candidate and checking committedType().
// Exercises the Möller-Trumbore leaf test end to end. (Uses confirm + committed
// rather than counting unconfirmed candidates: enumerating unconfirmed non-opaque
// triangle candidates is non-deterministic on RADV — see candidateFrontFace.)
const char* kTriDriver =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class TriRq {\n"
    "    @Kernel\n"
    "    public static void countTri(AccelerationStructure scene,\n"
    "                                KernelBuffer<float32> ox, KernelBuffer<float32> oy,\n"
    "                                KernelBuffer<uint32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          ox[i], oy[i], 20.0f, 0.0f,\n"   // origin above mesh
    "                          0.0f, 0.0f, -1.0f, 100.0f);\n"  // ray straight down
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 0) { rq.confirmIntersection(); }\n"
    "            }\n"
    "            uint32 c = 0;\n"   // confirm + committed: the reliable hit query
    "            if (rq.committedType() == 1) { c = 1; }\n"
    "            out[i] = c;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 nt = 4;\n"
    "        float32[] verts = heap float32[nt * 9];\n"
    "        for (uint32 t = 0; t < nt; t = t + 1) {\n"
    "            float32 bx = (float32)(t * 3);\n"
    "            float32 z = (float32)(t * 2);\n"
    "            verts[t*9+0]=bx;      verts[t*9+1]=0.0f;     verts[t*9+2]=z;\n"
    "            verts[t*9+3]=bx+1.0f; verts[t*9+4]=0.0f;     verts[t*9+5]=z;\n"
    "            verts[t*9+6]=bx;      verts[t*9+7]=1.0f;     verts[t*9+8]=z;\n"
    "        }\n"
    "        AccelerationStructure mesh = heap AccelerationStructure(verts, nt, 3);\n"
    "        uint32 n = 5;\n"
    "        float32[] hox = heap float32[n]; float32[] hoy = heap float32[n];\n"
    "        uint32[] hout = heap uint32[n];\n"
    "        hox[0]=0.2f;  hoy[0]=0.2f;\n"   // through tri0 interior -> 1
    "        hox[1]=3.2f;  hoy[1]=0.2f;\n"   // through tri1 -> 1
    "        hox[2]=6.2f;  hoy[2]=0.2f;\n"   // through tri2 -> 1
    "        hox[3]=9.2f;  hoy[3]=0.2f;\n"   // through tri3 -> 1
    "        hox[4]=50.0f; hoy[4]=50.0f;\n"  // misses all -> 0
    "        hout[0]=9; hout[1]=9; hout[2]=9; hout[3]=9; hout[4]=9;\n"
    "        KernelBuffer<float32> ox = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<float32> oy = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<uint32> out = heap KernelBuffer<uint32>(n);\n"
    "        ox.upload(hox); oy.upload(hoy); out.upload(hout);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        countTri.launch(s, grid: [1], block: [64])(mesh, ox, oy, out, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        if (hout[0] != 1) { return 100; }\n"
    "        if (hout[1] != 1) { return 101; }\n"
    "        if (hout[2] != 1) { return 102; }\n"
    "        if (hout[3] != 1) { return 103; }\n"
    "        if (hout[4] != 0) { return 104; }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

// Inc 3a: candidate getters (distance + barycentrics) on the CPU software path.
// A single triangle v0=(0,0,0), v1=(1,0,0), v2=(0,1,0) in z=0; a ray from
// (0.25,0.25,5) straight down hits at t=5 with barycentrics u=0.25, v=0.25
// (the hit point is v0 + u*(v1-v0) + v*(v2-v0) = (u, v, 0)).
const char* kBaryDriver =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class BaryRq {\n"
    "    @Kernel\n"
    "    public static void getBary(AccelerationStructure scene, KernelBuffer<float32> out) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i == 0) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          0.25f, 0.25f, 5.0f, 0.0f,\n"
    "                          0.0f, 0.0f, -1.0f, 100.0f);\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 0) {\n"
    "                    out[0] = rq.candidateDistance();\n"
    "                    out[1] = rq.candidateBarycentricU();\n"
    "                    out[2] = rq.candidateBarycentricV();\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        float32[] verts = heap float32[9];\n"
    "        verts[0]=0.0f; verts[1]=0.0f; verts[2]=0.0f;\n"
    "        verts[3]=1.0f; verts[4]=0.0f; verts[5]=0.0f;\n"
    "        verts[6]=0.0f; verts[7]=1.0f; verts[8]=0.0f;\n"
    "        AccelerationStructure tri = heap AccelerationStructure(verts, 1, 3);\n"
    "        float32[] hout = heap float32[3];\n"
    "        hout[0]=-9.0f; hout[1]=-9.0f; hout[2]=-9.0f;\n"
    "        KernelBuffer<float32> out = heap KernelBuffer<float32>(3);\n"
    "        out.upload(hout);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        getBary.launch(s, grid: [1], block: [64])(tri, out);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        float32 dt = hout[0] - 5.0f;\n"
    "        if (dt * dt > 0.001f) { return 100; }\n"
    "        float32 du = hout[1] - 0.25f;\n"
    "        if (du * du > 0.0001f) { return 101; }\n"
    "        float32 dv = hout[2] - 0.25f;\n"
    "        if (dv * dv > 0.0001f) { return 102; }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

// Inc 3b: nearest-hit via confirm + committed getters (CPU software). Two
// triangles stacked along the ray at z=2 (prim 0) and z=4 (prim 1), both covering
// the ray's xy. A ray from z=10 going down confirms every triangle candidate; the
// committed (nearest) hit must be the z=4 triangle (prim 1) at t=6 — the tMax
// shrink on confirm guarantees the closest wins regardless of traversal order.
const char* kNearestDriver =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class NearRq {\n"
    "    @Kernel\n"
    "    public static void nearest(AccelerationStructure scene,\n"
    "                               KernelBuffer<float32> outT, KernelBuffer<uint32> outI) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i == 0) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          0.25f, 0.25f, 10.0f, 0.0f,\n"
    "                          0.0f, 0.0f, -1.0f, 100.0f);\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 0) { rq.confirmIntersection(); }\n"
    "            }\n"
    "            outT[0] = rq.committedDistance();\n"
    "            outI[0] = rq.committedType();\n"
    "            outI[1] = rq.committedPrimitiveIndex();\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        float32[] verts = heap float32[18];\n"
    "        verts[0]=0.0f;  verts[1]=0.0f;  verts[2]=2.0f;\n"   // prim 0 @ z=2
    "        verts[3]=1.0f;  verts[4]=0.0f;  verts[5]=2.0f;\n"
    "        verts[6]=0.0f;  verts[7]=1.0f;  verts[8]=2.0f;\n"
    "        verts[9]=0.0f;  verts[10]=0.0f; verts[11]=4.0f;\n"  // prim 1 @ z=4
    "        verts[12]=1.0f; verts[13]=0.0f; verts[14]=4.0f;\n"
    "        verts[15]=0.0f; verts[16]=1.0f; verts[17]=4.0f;\n"
    "        AccelerationStructure mesh = heap AccelerationStructure(verts, 2, 3);\n"
    "        float32[] ht = heap float32[1]; ht[0]=-9.0f;\n"
    "        uint32[] hi = heap uint32[2]; hi[0]=9; hi[1]=9;\n"
    "        KernelBuffer<float32> outT = heap KernelBuffer<float32>(1);\n"
    "        KernelBuffer<uint32> outI = heap KernelBuffer<uint32>(2);\n"
    "        outT.upload(ht); outI.upload(hi);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        nearest.launch(s, grid: [1], block: [64])(mesh, outT, outI);\n"
    "        s.sync();\n"
    "        outT.download(ht); outI.download(hi);\n"
    "        if (hi[0] != 1) { return 100; }\n"            // committed type = triangle
    "        float32 dt = ht[0] - 6.0f;\n"
    "        if (dt * dt > 0.001f) { return 101; }\n"      // nearest distance = 6
    "        if (hi[1] != 1) { return 102; }\n"            // nearest prim = z=4 (idx 1)
    "        return 777;\n"
    "    }\n"
    "}\n";

// Inc 3b: front-face getter. A CCW triangle in z=0 (geometric normal +z): a ray
// from above (+z, going down) hits the front (true); a ray from below hits the
// back (false). The cajeta convention (MT det > 0) is set to match Vulkan's
// default CCW front-face, so CPU and native agree.
const char* kFrontDriver =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class FrontRq {\n"
    "    @Kernel\n"
    "    public static void getFront(AccelerationStructure scene,\n"
    "                                KernelBuffer<float32> oz, KernelBuffer<float32> dz,\n"
    "                                KernelBuffer<uint32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          0.25f, 0.25f, oz[i], 0.0f,\n"
    "                          0.0f, 0.0f, dz[i], 100.0f);\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 0) { rq.confirmIntersection(); }\n"
    "            }\n"
    "            uint32 f = 0;\n"   // committed front-face — the reliable query
    "            if (rq.committedType() == 1) {\n"
    "                if (rq.committedFrontFace()) { f = 1; } else { f = 2; }\n"
    "            }\n"
    "            out[i] = f;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        float32[] verts = heap float32[9];\n"
    "        verts[0]=0.0f; verts[1]=0.0f; verts[2]=0.0f;\n"
    "        verts[3]=1.0f; verts[4]=0.0f; verts[5]=0.0f;\n"
    "        verts[6]=0.0f; verts[7]=1.0f; verts[8]=0.0f;\n"
    "        AccelerationStructure tri = heap AccelerationStructure(verts, 1, 3);\n"
    "        uint32 n = 2;\n"
    "        float32[] hoz = heap float32[n]; float32[] hdz = heap float32[n];\n"
    "        hoz[0]=5.0f;  hdz[0]=-1.0f;\n"   // from above, going down -> front
    "        hoz[1]=-5.0f; hdz[1]=1.0f;\n"    // from below, going up   -> back
    "        uint32[] hout = heap uint32[n]; hout[0]=9; hout[1]=9;\n"
    "        KernelBuffer<float32> oz = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<float32> dz = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<uint32> out = heap KernelBuffer<uint32>(n);\n"
    "        oz.upload(hoz); dz.upload(hdz); out.upload(hout);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        getFront.launch(s, grid: [1], block: [64])(tri, oz, dz, out, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        if (hout[0] != 1) { return 100; }\n"   // front hit -> true
    "        if (hout[1] != 2) { return 101; }\n"   // back hit  -> false
    "        return 777;\n"
    "    }\n"
    "}\n";

// ── The RayQuery walk matrix (test-battery-restructure 2.2) ─────────────────
// The five walk drivers above (AABB count, triangle hit via confirm+committed,
// nearest-hit commit, candidate getters, front-face) are the SAME sources on
// every backend leg — that sameness is the point: each backend's 777 must
// equal the CPU 777, the verb following the noun across backends. Folded, the
// five per-backend single-scenario compiles become ONE multi-source compile
// per backend: a master class runs every scenario and encodes the first
// failure as scenario*1000 + the scenario's own code (0 = all hold). Every
// original assertion (the per-scenario fail codes) survives in the encoding.
namespace {
const char* kWalkAllDriver =
    "package test;\n"
    "public class WalkAll {\n"
    "    public static int32 run() {\n"
    "        int32 r = RqMin.run();\n"
    "        if (r != 777) { return 1000 + r; }\n"
    "        r = TriRq.run();\n"
    "        if (r != 777) { return 2000 + r; }\n"
    "        r = NearRq.run();\n"
    "        if (r != 777) { return 3000 + r; }\n"
    "        r = BaryRq.run();\n"
    "        if (r != 777) { return 4000 + r; }\n"
    "        r = FrontRq.run();\n"
    "        if (r != 777) { return 5000 + r; }\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

// thousands digit -> scenario; remainder -> that scenario's own fail code.
const char* kWalkDecode =
    " (1xxx aabbCount; 2xxx triangleHit; 3xxx nearestHit; 4xxx candidate"
    "Getters [4100 distance, 4101 baryU, 4102 baryV]; 5xxx frontFace"
    " [5100 front, 5101 back]; -1 compile, -2 lookup)";

int runWalkMatrix(cajeta::xpu::Backend be) {
    std::map<std::string, std::string> sources = {
        {"test.RqMin", kRqMinDriver},   {"test.TriRq", kTriDriver},
        {"test.NearRq", kNearestDriver}, {"test.BaryRq", kBaryDriver},
        {"test.FrontRq", kFrontDriver}, {"test.WalkAll", kWalkAllDriver},
    };
    CajetaJit::Options o;
    o.xpuBackends = {be};
    auto jit = CajetaJit::compile(sources, "test.WalkAll", o);
    if (jit == nullptr) return -1;
    auto fn = jit->lookup<int (*)()>("run");
    if (fn == nullptr) return -2;
    return fn();
}
} // namespace

// The CPU software path: AccelerationStructure builds the portable software
// BVH (runtime/native/cajeta_bvh.c) and RayQuery lowers to the cajeta
// SoftwareRayQuery walk — no device required. The reference every device leg
// must match.
TEST(XpuRayQueryDeviceTests, rayQueryWalkMatrixOnCpuSoftwareBvh) {
    int r = runWalkMatrix(cajeta::xpu::Backend::Cpu);
    EXPECT_EQ(r, 0) << "fail code " << r << kWalkDecode;
}

// The Vulkan NATIVE path (VK_KHR_acceleration_structure BLAS + OpRayQuery;
// getters via OpRayQueryGetIntersectionT / Barycentrics, Confirm/Generate via
// the cajeta-llvm fork intrinsics; Möller-Trumbore in hardware; non-opaque
// geometry so triangle candidates enumerate in the proceed() loop). Must match
// the CPU leg scenario for scenario.
TEST(XpuRayQueryDeviceTests, rayQueryWalkMatrixOnDevice) {
    if (!VulkanDriver::rayQueryAvailable()) {
        GTEST_SKIP() << "no Vulkan ray-query (acceleration-structure) device";
    }
    int r = runWalkMatrix(cajeta::xpu::Backend::Spirv);
    EXPECT_EQ(r, 0) << "fail code " << r << kWalkDecode
                    << " (Vulkan native != software reference)";
}

// ── Ray-query-to-core (inc 1): the same driver on the CPU SOFTWARE path ──
// No ray-query-capable device required — the AccelerationStructure builds a
// portable software BVH (runtime/native/cajeta_bvh.c) and RayQuery lowers to the
// cajeta SoftwareRayQuery walk. The results must match the Vulkan native path
// above (777 / 888): that agreement is what makes inline ray query genuinely core.
// Each ctest case is a fresh process, so the CPU-only bundle selects the CPU
// backend (priority CUDA>HIP>Vulkan>CPU is moot when only CPU is bundled).


// ── Capability heuristic + override (inc-4 brick #3) ────────────────────────
// The SAME minimal AABB ray query, built with AccelerationStructure.of(...,
// AsImpl.Software) — the explicit override forcing the portable software BVH even
// on a ray-query-capable GPU. On Vulkan this builds a software BVH into a storage
// buffer and runs the "<name>$sw" kernel variant (the SoftwareRayQuery walk in
// plain SPIR-V), selected at launch by the recorded impl. Identical to
// kRqMinDriver except the AS construction, so its 777 must match the native
// (minimalRayQueryOnDevice) and CPU (minimalRayQueryOnCpuSoftwareBvh) legs — the
// three-way equality that proves the noun's recorded impl drives the verb.
const char* kRqMinSoftwareDriver =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.AsImpl;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class RqMinSw {\n"
    "    @Kernel\n"
    "    public static void countHits(AccelerationStructure scene,\n"
    "                                 KernelBuffer<float32> qx, KernelBuffer<float32> qy,\n"
    "                                 KernelBuffer<float32> qz, KernelBuffer<uint32> out,\n"
    "                                 uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          qx[i], qy[i], qz[i], 0.0f,\n"
    "                          0.0f, 0.0f, 1.0f, 0.001f);\n"
    "            uint32 c = 0;\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 1) { c = c + 1; }\n"
    "            }\n"
    "            out[i] = c;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 np = 3;\n"
    "        float32[] boxes = heap float32[np * 6];\n"
    "        boxes[0]=-0.5f;  boxes[1]=-0.5f; boxes[2]=-0.5f;  boxes[3]=0.5f;  boxes[4]=0.5f; boxes[5]=0.5f;\n"
    "        boxes[6]=9.5f;   boxes[7]=-0.5f; boxes[8]=-0.5f;  boxes[9]=10.5f; boxes[10]=0.5f; boxes[11]=0.5f;\n"
    "        boxes[12]=19.5f; boxes[13]=-0.5f; boxes[14]=-0.5f; boxes[15]=20.5f; boxes[16]=0.5f; boxes[17]=0.5f;\n"
    "        AccelerationStructure scene #= AccelerationStructure.of(boxes, np, AsImpl.Software);\n"
    "        uint32 n = 4;\n"
    "        float32[] hqx = heap float32[n]; float32[] hqy = heap float32[n]; float32[] hqz = heap float32[n];\n"
    "        uint32[] hout = heap uint32[n];\n"
    "        hqx[0]=0.0f;  hqy[0]=0.0f; hqz[0]=0.0f;\n"
    "        hqx[1]=5.0f;  hqy[1]=0.0f; hqz[1]=0.0f;\n"
    "        hqx[2]=10.0f; hqy[2]=0.0f; hqz[2]=0.0f;\n"
    "        hqx[3]=19.7f; hqy[3]=0.0f; hqz[3]=0.0f;\n"
    "        hout[0]=9; hout[1]=9; hout[2]=9; hout[3]=9;\n"
    "        KernelBuffer<float32> qx = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<float32> qy = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<float32> qz = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<uint32> out = heap KernelBuffer<uint32>(n);\n"
    "        qx.upload(hqx); qy.upload(hqy); qz.upload(hqz); out.upload(hout);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        countHits.launch(s, grid: [1], block: [64])(scene, qx, qy, qz, out, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        if (hout[0] != 1) { return 100; }\n"
    "        if (hout[1] != 0) { return 101; }\n"
    "        if (hout[2] != 1) { return 102; }\n"
    "        if (hout[3] != 1) { return 103; }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

// Forced-NATIVE twin of kRqMinSoftwareDriver: AccelerationStructure.of(...,
// AsImpl.Native) — the explicit override forcing the native VK_KHR_acceleration_
// structure BLAS + OpRayQuery on a ray-query-capable GPU (falls back to software
// only if the device has none). Identical scene/rays to kRqMinDriver and
// kRqMinSoftwareDriver, so its 777 must match both — proving the explicit Native
// override drives the native path, independent of the AUTO default policy (so a
// future AUTO/gate regression can't silently downgrade this leg to software).
const char* kRqMinNativeDriver =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.AsImpl;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class RqMinNat {\n"
    "    @Kernel\n"
    "    public static void countHits(AccelerationStructure scene,\n"
    "                                 KernelBuffer<float32> qx, KernelBuffer<float32> qy,\n"
    "                                 KernelBuffer<float32> qz, KernelBuffer<uint32> out,\n"
    "                                 uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          qx[i], qy[i], qz[i], 0.0f,\n"
    "                          0.0f, 0.0f, 1.0f, 0.001f);\n"
    "            uint32 c = 0;\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 1) { c = c + 1; }\n"
    "            }\n"
    "            out[i] = c;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 np = 3;\n"
    "        float32[] boxes = heap float32[np * 6];\n"
    "        boxes[0]=-0.5f;  boxes[1]=-0.5f; boxes[2]=-0.5f;  boxes[3]=0.5f;  boxes[4]=0.5f; boxes[5]=0.5f;\n"
    "        boxes[6]=9.5f;   boxes[7]=-0.5f; boxes[8]=-0.5f;  boxes[9]=10.5f; boxes[10]=0.5f; boxes[11]=0.5f;\n"
    "        boxes[12]=19.5f; boxes[13]=-0.5f; boxes[14]=-0.5f; boxes[15]=20.5f; boxes[16]=0.5f; boxes[17]=0.5f;\n"
    "        AccelerationStructure scene #= AccelerationStructure.of(boxes, np, AsImpl.Native);\n"
    "        uint32 n = 4;\n"
    "        float32[] hqx = heap float32[n]; float32[] hqy = heap float32[n]; float32[] hqz = heap float32[n];\n"
    "        uint32[] hout = heap uint32[n];\n"
    "        hqx[0]=0.0f;  hqy[0]=0.0f; hqz[0]=0.0f;\n"
    "        hqx[1]=5.0f;  hqy[1]=0.0f; hqz[1]=0.0f;\n"
    "        hqx[2]=10.0f; hqy[2]=0.0f; hqz[2]=0.0f;\n"
    "        hqx[3]=19.7f; hqy[3]=0.0f; hqz[3]=0.0f;\n"
    "        hout[0]=9; hout[1]=9; hout[2]=9; hout[3]=9;\n"
    "        KernelBuffer<float32> qx = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<float32> qy = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<float32> qz = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<uint32> out = heap KernelBuffer<uint32>(n);\n"
    "        qx.upload(hqx); qy.upload(hqy); qz.upload(hqz); out.upload(hout);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        countHits.launch(s, grid: [1], block: [64])(scene, qx, qy, qz, out, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        if (hout[0] != 1) { return 100; }\n"
    "        if (hout[1] != 0) { return 101; }\n"
    "        if (hout[2] != 1) { return 102; }\n"
    "        if (hout[3] != 1) { return 103; }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

// Impl-recording probe: the SAME AABB scene/rays as kRqMinDriver with the default
// (AUTO) ctor, but run() returns 700 + scene.implTag() once the counts are
// verified. On a ray-query Vulkan device AUTO records native (impl 1) -> 701; a
// software fallback would record impl 0 -> 700. This is the honest proof that the
// AUTO *OnDevice legs are genuinely native and not silently downgraded to the
// software BVH (which would also produce the correct 1/0/1/1 counts).
const char* kImplProbeDriver =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class RqImpl {\n"
    "    @Kernel\n"
    "    public static void countHits(AccelerationStructure scene,\n"
    "                                 KernelBuffer<float32> qx, KernelBuffer<float32> qy,\n"
    "                                 KernelBuffer<float32> qz, KernelBuffer<uint32> out,\n"
    "                                 uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          qx[i], qy[i], qz[i], 0.0f,\n"
    "                          0.0f, 0.0f, 1.0f, 0.001f);\n"
    "            uint32 c = 0;\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 1) { c = c + 1; }\n"
    "            }\n"
    "            out[i] = c;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 np = 3;\n"
    "        float32[] boxes = heap float32[np * 6];\n"
    "        boxes[0]=-0.5f;  boxes[1]=-0.5f; boxes[2]=-0.5f;  boxes[3]=0.5f;  boxes[4]=0.5f; boxes[5]=0.5f;\n"
    "        boxes[6]=9.5f;   boxes[7]=-0.5f; boxes[8]=-0.5f;  boxes[9]=10.5f; boxes[10]=0.5f; boxes[11]=0.5f;\n"
    "        boxes[12]=19.5f; boxes[13]=-0.5f; boxes[14]=-0.5f; boxes[15]=20.5f; boxes[16]=0.5f; boxes[17]=0.5f;\n"
    "        AccelerationStructure scene = heap AccelerationStructure(boxes, np);\n"
    "        uint32 n = 4;\n"
    "        float32[] hqx = heap float32[n]; float32[] hqy = heap float32[n]; float32[] hqz = heap float32[n];\n"
    "        uint32[] hout = heap uint32[n];\n"
    "        hqx[0]=0.0f;  hqy[0]=0.0f; hqz[0]=0.0f;\n"
    "        hqx[1]=5.0f;  hqy[1]=0.0f; hqz[1]=0.0f;\n"
    "        hqx[2]=10.0f; hqy[2]=0.0f; hqz[2]=0.0f;\n"
    "        hqx[3]=19.7f; hqy[3]=0.0f; hqz[3]=0.0f;\n"
    "        hout[0]=9; hout[1]=9; hout[2]=9; hout[3]=9;\n"
    "        KernelBuffer<float32> qx = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<float32> qy = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<float32> qz = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<uint32> out = heap KernelBuffer<uint32>(n);\n"
    "        qx.upload(hqx); qy.upload(hqy); qz.upload(hqz); out.upload(hout);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        countHits.launch(s, grid: [1], block: [64])(scene, qx, qy, qz, out, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        if (hout[0] != 1) { return 100; }\n"
    "        if (hout[1] != 0) { return 101; }\n"
    "        if (hout[2] != 1) { return 102; }\n"
    "        if (hout[3] != 1) { return 103; }\n"
    "        return 700 + scene.implTag();\n"   // 701 = native, 700 = software
    "    }\n"
    "}\n";

// The Vulkan impl-override matrix (test-battery-restructure 2.2; was auto
// RecordsNativeImplOnDevice + forcedSoftwareOfApiOnDevice + forcedNativeOfApi
// OnDevice — the AUTO-native walk reference minimalRayQueryOnDevice lives on
// in rayQueryWalkMatrixOnDevice): the SAME AABB scene under all three impl
// selections in ONE compile.
//   (1) AUTO must RECORD native (implTag 1 -> 701) — the honest proof the
//       *OnDevice legs are genuinely native, not silently running the software
//       fallback (which would also produce the correct 1/0/1/1 counts). Before
//       the Win32 un-gate of caj_native_rayquery_available(), AUTO resolved to
//       software on Windows even on the 4090; this guards the fix on-device.
//   (2) FORCED software: AsImpl.Software on the same device builds a software
//       BVH and runs the $sw variant; its 777 == native == CPU. One backend,
//       either impl, the verb following the noun.
//   (3) FORCED native: AsImpl.Native builds the native BLAS + OpRayQuery on
//       its own, not just as the AUTO default — green even if a future change
//       alters the AUTO/caj_native_rayquery_available policy.
namespace {
const char* kImplAllDriver =
    "package test;\n"
    "public class ImplAll {\n"
    "    public static int32 run() {\n"
    "        int32 r = RqImpl.run();\n"
    "        if (r != 701) { return 1000 + r; }\n"
    "        r = RqMinSw.run();\n"
    "        if (r != 777) { return 2000 + r; }\n"
    "        r = RqMinNat.run();\n"
    "        if (r != 777) { return 3000 + r; }\n"
    "        return 0;\n"
    "    }\n"
    "}\n";
} // namespace

TEST(XpuRayQueryDeviceTests, implOverrideRecordingMatrixOnDevice) {
    if (!VulkanDriver::rayQueryAvailable()) {
        GTEST_SKIP() << "no Vulkan ray-query (acceleration-structure) device";
    }
    std::map<std::string, std::string> sources = {
        {"test.RqImpl", kImplProbeDriver},
        {"test.RqMinSw", kRqMinSoftwareDriver},
        {"test.RqMinNat", kRqMinNativeDriver},
        {"test.ImplAll", kImplAllDriver},
    };
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(sources, "test.ImplAll", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 0) << "fail code " << r
                    << " (1xxx AUTO probe: 1700 = AUTO recorded SOFTWARE not "
                       "native, 11xx wrong counts; 2xxx forced-software; "
                       "3xxx forced-native; remainder = the leg's own code)";
}

// ===========================================================================
// NVIDIA (NVPTX → cubin, CudaDriver) software-BVH ray query. NVIDIA has no cajeta
// native inline ray-query seam, so NvptxTarget.accelImpl() == SoftwareBvh: the
// AccelerationStructure noun builds the portable software BVH (the shared CPU
// builder), the CUDA noun provider uploads it into a device buffer, and the kernel
// runs the cajeta.xpu.SoftwareRayQuery walk under its base name (no $sw twin
// — the whole NVPTX kernel is the software walk). The SAME driver sources as the
// CPU software-BVH legs above, so each NVPTX 777 must equal the CPU 777 — the verb
// following the noun across a third backend. Skips cleanly without a CUDA device.
//
// M3 Phase 3: these five drivers are all SUPPORTED OptiX shapes, so under AUTO on a
// 4090 the launch now lazily builds + prefers the OptiX rep (RT cores) — which would
// keep them green (still 777) but stop exercising the NVPTX SOFTWARE walk they exist
// to cover. Each forces CAJETA_GPU_AS_IMPL=software so it keeps testing the software
// path after the AUTO→OptiX flip; the AUTO/RT-core path is covered by the OptiX device
// suite + the M3 lazy/selection tests.
// ---------------------------------------------------------------------------

// RAII env guard (CAJETA_GPU_AS_IMPL for the scope), restored on destruction.
namespace { struct AsImplEnvGuard {
    AsImplEnvGuard(const char* v) { setenv("CAJETA_GPU_AS_IMPL", v, 1); }
    ~AsImplEnvGuard() { unsetenv("CAJETA_GPU_AS_IMPL"); }
}; }


// ── NVIDIA OptiX RT-core AS tier (Milestone 1) ──────────────────────────────
// (AsImplEnvGuard is defined above, before the software-BVH section.)

// Build-only driver (no kernel launch): the OptiX *verb* (traversal via optixTrace)
// is M2, so M1 validates that the CUDA noun provider BUILDS + RECORDS the OptiX AS
// tier and frees it. run() builds an AABB AS and returns 700 + implTag(): on the
// 4090 with the OptiX SDK and CAJETA_GPU_AS_IMPL=optix that is 702 (CAJ_AS_IMPL_OPTIX);
// 700 would mean it fell back to the software floor (OptiX unavailable at runtime).
const char* kOptixImplDriver =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class RqOptix {\n"
    // A @Kernel (even unlaunched) makes the bundle register the CUDA backend at
    // module init, so it is the active backend when the AS builds below — without
    // it the build wouldn't route to the CUDA noun provider's OptiX arm.
    "    @Kernel\n"
    "    public static void noop(KernelBuffer<uint32> b) {\n"
    "        uint32 i = KernelThread.globalIdX(); if (i == 0) { b[0] = 1; }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 np = 3;\n"
    "        float32[] boxes = heap float32[np * 6];\n"
    "        boxes[0]=-0.5f;  boxes[1]=-0.5f; boxes[2]=-0.5f;  boxes[3]=0.5f;  boxes[4]=0.5f; boxes[5]=0.5f;\n"
    "        boxes[6]=9.5f;   boxes[7]=-0.5f; boxes[8]=-0.5f;  boxes[9]=10.5f; boxes[10]=0.5f; boxes[11]=0.5f;\n"
    "        boxes[12]=19.5f; boxes[13]=-0.5f; boxes[14]=-0.5f; boxes[15]=20.5f; boxes[16]=0.5f; boxes[17]=0.5f;\n"
    "        AccelerationStructure scene = heap AccelerationStructure(boxes, np);\n"
    "        return 700 + scene.implTag();\n"   // 702 = OptiX, 700 = software fallback
    "    }\n"
    "}\n";

// Defined unconditionally by OptixAccel.cpp — the real probe when CAJETA_HAS_OPTIX
// is on, and a `return 0` stub when it is not.
extern "C" int cajeta_xpu_optix_available(void);

TEST(XpuRayQueryDeviceTests, optixRecordsImplOnNvptxDevice) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    // CUDA presence is not OptiX presence. A box with a CUDA driver but no OptiX
    // engine (no SDK at build time, or no loadable nvoptix at run time) records
    // the software tier and returns 700, and this test FAILED there rather than
    // skipping — a red suite on every non-OptiX CUDA host.
    //
    // The probe, not the 700 sentinel the lazy-build tests below use: 700 is
    // ambiguous here. It means EITHER the engine is absent (skip) OR the engine
    // is present and the CUDA OptiX noun arm did not take — which is the exact
    // defect this test exists to catch. Skipping on 700 would make it vacuous on
    // the machines that matter.
    if (!cajeta_xpu_optix_available()) {
        GTEST_SKIP() << "CUDA present but OptiX engine unavailable — the OptiX AS "
                        "tier cannot be recorded on this host";
    }
    AsImplEnvGuard forceOptix("optix");   // CUDA: optix|native -> the OptiX AS tier
    std::map<std::string, std::string> sources = {{"test.RqOptix", kOptixImplDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.RqOptix", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 702) << "fail code " << r
                      << " (700: OptiX AS build/record fell back to software — the "
                         "CUDA OptiX noun arm didn't take, or OptiX unavailable on-device)";
}

// M3 Phase 1 — the multi-impl noun: an OptiX-primary AS also retains the portable
// software-BVH FLOOR as a secondary representation, so implSet() reports BOTH
// (1<<Optix=4 | 1<<Software=1 = 5). implTag() still reports the single primary
// (Optix=2). The floor is what the M3 launch-time selector falls back to for an
// Unsupported-shape kernel. Build-only driver (no kernel launch needed).
const char* kImplSetDriver =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class RqSet {\n"
    "    @Kernel\n"
    "    public static void noop(KernelBuffer<uint32> b) {\n"
    "        uint32 i = KernelThread.globalIdX(); if (i == 0) { b[0] = 1; }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 np = 3;\n"
    "        float32[] boxes = heap float32[np * 6];\n"
    "        boxes[0]=-0.5f;  boxes[1]=-0.5f; boxes[2]=-0.5f;  boxes[3]=0.5f;  boxes[4]=0.5f; boxes[5]=0.5f;\n"
    "        boxes[6]=9.5f;   boxes[7]=-0.5f; boxes[8]=-0.5f;  boxes[9]=10.5f; boxes[10]=0.5f; boxes[11]=0.5f;\n"
    "        boxes[12]=19.5f; boxes[13]=-0.5f; boxes[14]=-0.5f; boxes[15]=20.5f; boxes[16]=0.5f; boxes[17]=0.5f;\n"
    "        AccelerationStructure scene = heap AccelerationStructure(boxes, np);\n"
    "        return scene.implSet();\n"   // bitmask: Optix(4) | Software-floor(1) = 5
    "    }\n"
    "}\n";


// M3 Phase 1 — implSet() on an AUTO (software-primary) AS reports software-only
// (bit 1), and implTag() still returns the single primary tag. Backward-compat:
// AUTO on CUDA stays software (the M2 4-C policy holds until M3 Phase 4 flips it).

// M3 Phase 2 — launch-time impl selection (the verb picks). ONE AccelerationStructure
// (built OptiX-primary + software floor under =optix) is consumed by TWO kernels in the
// same program: a SUPPORTED canonical AABB-count shape `countHits` (AS, qx, qy, qz, out,
// n) → emits an OptiX program set → runs on the RT cores via optixLaunch; and an
// UNSUPPORTED shape `countOne` (AS, qx, out, n — a signature no canonical shape matches)
// → no OptiX program → the software cubin, which the M3 launch hands the retained
// software FLOOR (not the OptixAs* primary, which would fault). Both must return the same
// neighbour counts {1,0,1,1}. This is the decisive proof that selection is per-launch and
// the floor fallback eliminates the M2 silent fault. The unsupported kernel bakes qy=qz=0
// so its single qx buffer reproduces the supported kernel's degenerate-ray RTNN query.
const char* kSelectDriver =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class RqSelect {\n"
    "    @Kernel\n"   // SUPPORTED: canonical AABB candidate-count -> OptiX RT cores
    "    public static void countHits(AccelerationStructure scene,\n"
    "                                 KernelBuffer<float32> qx, KernelBuffer<float32> qy,\n"
    "                                 KernelBuffer<float32> qz, KernelBuffer<uint32> out,\n"
    "                                 uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          qx[i], qy[i], qz[i], 0.0f,\n"
    "                          0.0f, 0.0f, 1.0f, 0.001f);\n"
    "            uint32 c = 0;\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 1) { c = c + 1; }\n"
    "            }\n"
    "            out[i] = c;\n"
    "        }\n"
    "    }\n"
    "    @Kernel\n"   // UNSUPPORTED shape (AS, 2 KernelBuffer, 1 scalar) -> software floor
    "    public static void countOne(AccelerationStructure scene,\n"
    "                                KernelBuffer<float32> qx, KernelBuffer<uint32> out,\n"
    "                                uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          qx[i], 0.0f, 0.0f, 0.0f,\n"
    "                          0.0f, 0.0f, 1.0f, 0.001f);\n"
    "            uint32 c = 0;\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 1) { c = c + 1; }\n"
    "            }\n"
    "            out[i] = c;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 np = 3;\n"
    "        float32[] boxes = heap float32[np * 6];\n"
    "        boxes[0]=-0.5f;  boxes[1]=-0.5f; boxes[2]=-0.5f;  boxes[3]=0.5f;  boxes[4]=0.5f; boxes[5]=0.5f;\n"
    "        boxes[6]=9.5f;   boxes[7]=-0.5f; boxes[8]=-0.5f;  boxes[9]=10.5f; boxes[10]=0.5f; boxes[11]=0.5f;\n"
    "        boxes[12]=19.5f; boxes[13]=-0.5f; boxes[14]=-0.5f; boxes[15]=20.5f; boxes[16]=0.5f; boxes[17]=0.5f;\n"
    "        AccelerationStructure scene = heap AccelerationStructure(boxes, np);\n"
    "        uint32 n = 4;\n"
    "        float32[] hqx = heap float32[n]; float32[] hqy = heap float32[n]; float32[] hqz = heap float32[n];\n"
    "        uint32[] hout = heap uint32[n];\n"
    "        hqx[0]=0.0f;  hqy[0]=0.0f; hqz[0]=0.0f;\n"
    "        hqx[1]=5.0f;  hqy[1]=0.0f; hqz[1]=0.0f;\n"
    "        hqx[2]=10.0f; hqy[2]=0.0f; hqz[2]=0.0f;\n"
    "        hqx[3]=19.7f; hqy[3]=0.0f; hqz[3]=0.0f;\n"
    "        hout[0]=9; hout[1]=9; hout[2]=9; hout[3]=9;\n"
    "        KernelBuffer<float32> qx = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<float32> qy = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<float32> qz = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<uint32> out = heap KernelBuffer<uint32>(n);\n"
    "        qx.upload(hqx); qy.upload(hqy); qz.upload(hqz); out.upload(hout);\n"
    "        KernelStream s #= KernelStream.current();\n"
    // 1) supported shape -> OptiX RT cores
    "        countHits.launch(s, grid: [1], block: [64])(scene, qx, qy, qz, out, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        if (hout[0] != 1) { return 100; }\n"
    "        if (hout[1] != 0) { return 101; }\n"
    "        if (hout[2] != 1) { return 102; }\n"
    "        if (hout[3] != 1) { return 103; }\n"
    // 2) unsupported shape, SAME AS -> software floor fallback (no fault)
    "        hout[0]=9; hout[1]=9; hout[2]=9; hout[3]=9; out.upload(hout);\n"
    "        countOne.launch(s, grid: [1], block: [64])(scene, qx, out, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        if (hout[0] != 1) { return 200; }\n"
    "        if (hout[1] != 0) { return 201; }\n"
    "        if (hout[2] != 1) { return 202; }\n"
    "        if (hout[3] != 1) { return 203; }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

// 2a — the decisive launch-time-selection test: one AS, a supported kernel on the RT
// cores and an Unsupported-shape kernel on the software floor, both correct (777).

// 2b — forced =optix with an UNSUPPORTED shape must NOT fault on the OptixAs* primary:
// it falls back to the retained software floor and returns the correct counts. Driver
// runs ONLY the unsupported kernel, in isolation, against an OptiX-forced AS.
const char* kUnsupOptixDriver =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class RqUnsup {\n"
    "    @Kernel\n"   // (AS, 2 KernelBuffer, 1 scalar) -> Unsupported -> software floor
    "    public static void countOne(AccelerationStructure scene,\n"
    "                                KernelBuffer<float32> qx, KernelBuffer<uint32> out,\n"
    "                                uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          qx[i], 0.0f, 0.0f, 0.0f,\n"
    "                          0.0f, 0.0f, 1.0f, 0.001f);\n"
    "            uint32 c = 0;\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 1) { c = c + 1; }\n"
    "            }\n"
    "            out[i] = c;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 np = 3;\n"
    "        float32[] boxes = heap float32[np * 6];\n"
    "        boxes[0]=-0.5f;  boxes[1]=-0.5f; boxes[2]=-0.5f;  boxes[3]=0.5f;  boxes[4]=0.5f; boxes[5]=0.5f;\n"
    "        boxes[6]=9.5f;   boxes[7]=-0.5f; boxes[8]=-0.5f;  boxes[9]=10.5f; boxes[10]=0.5f; boxes[11]=0.5f;\n"
    "        boxes[12]=19.5f; boxes[13]=-0.5f; boxes[14]=-0.5f; boxes[15]=20.5f; boxes[16]=0.5f; boxes[17]=0.5f;\n"
    "        AccelerationStructure scene = heap AccelerationStructure(boxes, np);\n"
    "        uint32 n = 4;\n"
    "        float32[] hqx = heap float32[n]; uint32[] hout = heap uint32[n];\n"
    "        hqx[0]=0.0f; hqx[1]=5.0f; hqx[2]=10.0f; hqx[3]=19.7f;\n"
    "        hout[0]=9; hout[1]=9; hout[2]=9; hout[3]=9;\n"
    "        KernelBuffer<float32> qx = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<uint32> out = heap KernelBuffer<uint32>(n);\n"
    "        qx.upload(hqx); out.upload(hout);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        countOne.launch(s, grid: [1], block: [64])(scene, qx, out, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        if (hout[0] != 1) { return 100; }\n"
    "        if (hout[1] != 0) { return 101; }\n"
    "        if (hout[2] != 1) { return 102; }\n"
    "        if (hout[3] != 1) { return 103; }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

TEST(XpuRayQueryDeviceTests, forcedOptixUnsupportedShapeFallsBackToSoftware) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    AsImplEnvGuard forceOptix("optix");
    std::map<std::string, std::string> sources = {{"test.RqUnsup", kUnsupOptixDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.RqUnsup", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (forced =optix + Unsupported shape must fall back to the "
                         "software floor — a fault/garbage here is the M2 silent-fault "
                         "regression the M3 floor swap exists to eliminate)";
}

// M2 Phase 4-C policy — on CUDA, AUTO (no CAJETA_GPU_AS_IMPL) records SOFTWARE, even
// when OptiX is available. The portable software BVH is the v1 default floor; OptiX
// RT cores are strictly OPT-IN (=optix), because the AS impl is resolved before the
// consumer kernel is known and only 3 canonical ray-query shapes are OptiX-supported —
// auto-routing any other shape onto an OptiX AS would fault the software cubin that
// reads it. The SAME kOptixImplDriver as optixRecordsImplOnNvptxDevice, no env: 700
// (software), NOT 702. Guards against an accidental AUTO→OptiX flip. See the OptiX
// AUTO-policy note + the M2 codegen plan (4-C).

// ── M3 Phase 3 — lazy / elidable native build ───────────────────────────────
// Under AUTO the build records the software-BVH floor as the primary and does NOT build
// the OptiX rep; that is deferred to the first SUPPORTED-shape launch (R4 cost control —
// an AS consumed only by software kernels never pays for OptiX). implSet() observes the
// transition: software-only (1) right after build, software|OptiX (5) after a supported
// launch lazily builds the OptiX rep. The drop-software-floor hint (3c) is below:
// AsImpl.NativeNoFloor omits the floor (implSet()==4).

// 3a — a software-only consumer never triggers the lazy OptiX build. Under AUTO, build an
// AABB AS and launch an UNSUPPORTED-shape kernel (AS, 2 KernelBuffer, 1 scalar → software cubin
// over the floor); implSet() must stay software-only (1) afterwards. run() returns
// 700 + implSet() → expect 701.
const char* kLazySoftOnlyDriver =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class RqLazySoft {\n"
    "    @Kernel\n"   // Unsupported shape -> software cubin, no OptiX program
    "    public static void countOne(AccelerationStructure scene,\n"
    "                                KernelBuffer<float32> qx, KernelBuffer<uint32> out,\n"
    "                                uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          qx[i], 0.0f, 0.0f, 0.0f,\n"
    "                          0.0f, 0.0f, 1.0f, 0.001f);\n"
    "            uint32 c = 0;\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 1) { c = c + 1; }\n"
    "            }\n"
    "            out[i] = c;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 np = 3;\n"
    "        float32[] boxes = heap float32[np * 6];\n"
    "        boxes[0]=-0.5f;  boxes[1]=-0.5f; boxes[2]=-0.5f;  boxes[3]=0.5f;  boxes[4]=0.5f; boxes[5]=0.5f;\n"
    "        boxes[6]=9.5f;   boxes[7]=-0.5f; boxes[8]=-0.5f;  boxes[9]=10.5f; boxes[10]=0.5f; boxes[11]=0.5f;\n"
    "        boxes[12]=19.5f; boxes[13]=-0.5f; boxes[14]=-0.5f; boxes[15]=20.5f; boxes[16]=0.5f; boxes[17]=0.5f;\n"
    "        AccelerationStructure scene = heap AccelerationStructure(boxes, np);\n"
    "        uint32 n = 4;\n"
    "        float32[] hqx = heap float32[n]; uint32[] hout = heap uint32[n];\n"
    "        hqx[0]=0.0f; hqx[1]=5.0f; hqx[2]=10.0f; hqx[3]=19.7f;\n"
    "        hout[0]=9; hout[1]=9; hout[2]=9; hout[3]=9;\n"
    "        KernelBuffer<float32> qx = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<uint32> out = heap KernelBuffer<uint32>(n);\n"
    "        qx.upload(hqx); out.upload(hout);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        countOne.launch(s, grid: [1], block: [64])(scene, qx, out, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        if (hout[0] != 1) { return 100; }\n"
    "        if (hout[1] != 0) { return 101; }\n"
    "        if (hout[2] != 1) { return 102; }\n"
    "        if (hout[3] != 1) { return 103; }\n"
    "        return 700 + scene.implSet();\n"   // expect 701: OptiX NEVER built
    "    }\n"
    "}\n";


// 3b — the OptiX rep is absent right after build (implSet()==1) and present after the
// first SUPPORTED-shape launch (implSet()==5): the lazy build fires on demand. Under
// AUTO; run() checks the pre-launch set, launches the canonical AABB-count shape, then
// returns 700 + post-launch implSet() (705 = lazy OptiX built; 701 = OptiX engine absent
// on this box → skip).
const char* kLazyBuildDriver =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class RqLazyBuild {\n"
    "    @Kernel\n"   // SUPPORTED canonical AABB candidate-count -> lazy OptiX at launch
    "    public static void countHits(AccelerationStructure scene,\n"
    "                                 KernelBuffer<float32> qx, KernelBuffer<float32> qy,\n"
    "                                 KernelBuffer<float32> qz, KernelBuffer<uint32> out,\n"
    "                                 uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          qx[i], qy[i], qz[i], 0.0f,\n"
    "                          0.0f, 0.0f, 1.0f, 0.001f);\n"
    "            uint32 c = 0;\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 1) { c = c + 1; }\n"
    "            }\n"
    "            out[i] = c;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 np = 3;\n"
    "        float32[] boxes = heap float32[np * 6];\n"
    "        boxes[0]=-0.5f;  boxes[1]=-0.5f; boxes[2]=-0.5f;  boxes[3]=0.5f;  boxes[4]=0.5f; boxes[5]=0.5f;\n"
    "        boxes[6]=9.5f;   boxes[7]=-0.5f; boxes[8]=-0.5f;  boxes[9]=10.5f; boxes[10]=0.5f; boxes[11]=0.5f;\n"
    "        boxes[12]=19.5f; boxes[13]=-0.5f; boxes[14]=-0.5f; boxes[15]=20.5f; boxes[16]=0.5f; boxes[17]=0.5f;\n"
    "        AccelerationStructure scene = heap AccelerationStructure(boxes, np);\n"
    "        int32 pre = scene.implSet();\n"          // expect 1 (software-only, no OptiX yet)
    "        uint32 n = 4;\n"
    "        float32[] hqx = heap float32[n]; float32[] hqy = heap float32[n]; float32[] hqz = heap float32[n];\n"
    "        uint32[] hout = heap uint32[n];\n"
    "        hqx[0]=0.0f;  hqy[0]=0.0f; hqz[0]=0.0f;\n"
    "        hqx[1]=5.0f;  hqy[1]=0.0f; hqz[1]=0.0f;\n"
    "        hqx[2]=10.0f; hqy[2]=0.0f; hqz[2]=0.0f;\n"
    "        hqx[3]=19.7f; hqy[3]=0.0f; hqz[3]=0.0f;\n"
    "        hout[0]=9; hout[1]=9; hout[2]=9; hout[3]=9;\n"
    "        KernelBuffer<float32> qx = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<float32> qy = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<float32> qz = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<uint32> out = heap KernelBuffer<uint32>(n);\n"
    "        qx.upload(hqx); qy.upload(hqy); qz.upload(hqz); out.upload(hout);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        countHits.launch(s, grid: [1], block: [64])(scene, qx, qy, qz, out, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        if (hout[0] != 1) { return 100; }\n"
    "        if (hout[1] != 0) { return 101; }\n"
    "        if (hout[2] != 1) { return 102; }\n"
    "        if (hout[3] != 1) { return 103; }\n"
    "        if (pre != 1) { return 110; }\n"          // OptiX must NOT exist before launch
    "        return 700 + scene.implSet();\n"          // 705 = OptiX built lazily; 701 = engine absent
    "    }\n"
    "}\n";


// 3c — the drop-software-floor hint. AsImpl.NativeNoFloor asserts all consumers are
// supported native shapes, so the build omits the software floor: implSet() reports
// OptiX-only (4, no software bit 1), and a supported-shape kernel still runs on the RT
// cores. The caller trades the Unsupported-shape safety net for the floor's memory.
// run() returns 700 + implSet() → 704 (floor dropped) on the 4090; 701 if the OptiX
// engine is absent (NativeNoFloor then degenerates to the software BVH as the only rep).
const char* kNoFloorDriver =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.AsImpl;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class RqNoFloor {\n"
    "    @Kernel\n"   // SUPPORTED canonical AABB candidate-count -> RT cores, no floor needed
    "    public static void countHits(AccelerationStructure scene,\n"
    "                                 KernelBuffer<float32> qx, KernelBuffer<float32> qy,\n"
    "                                 KernelBuffer<float32> qz, KernelBuffer<uint32> out,\n"
    "                                 uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          qx[i], qy[i], qz[i], 0.0f,\n"
    "                          0.0f, 0.0f, 1.0f, 0.001f);\n"
    "            uint32 c = 0;\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 1) { c = c + 1; }\n"
    "            }\n"
    "            out[i] = c;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 np = 3;\n"
    "        float32[] boxes = heap float32[np * 6];\n"
    "        boxes[0]=-0.5f;  boxes[1]=-0.5f; boxes[2]=-0.5f;  boxes[3]=0.5f;  boxes[4]=0.5f; boxes[5]=0.5f;\n"
    "        boxes[6]=9.5f;   boxes[7]=-0.5f; boxes[8]=-0.5f;  boxes[9]=10.5f; boxes[10]=0.5f; boxes[11]=0.5f;\n"
    "        boxes[12]=19.5f; boxes[13]=-0.5f; boxes[14]=-0.5f; boxes[15]=20.5f; boxes[16]=0.5f; boxes[17]=0.5f;\n"
    "        AccelerationStructure scene #= AccelerationStructure.of(boxes, np, AsImpl.NativeNoFloor);\n"
    "        uint32 n = 4;\n"
    "        float32[] hqx = heap float32[n]; float32[] hqy = heap float32[n]; float32[] hqz = heap float32[n];\n"
    "        uint32[] hout = heap uint32[n];\n"
    "        hqx[0]=0.0f;  hqy[0]=0.0f; hqz[0]=0.0f;\n"
    "        hqx[1]=5.0f;  hqy[1]=0.0f; hqz[1]=0.0f;\n"
    "        hqx[2]=10.0f; hqy[2]=0.0f; hqz[2]=0.0f;\n"
    "        hqx[3]=19.7f; hqy[3]=0.0f; hqz[3]=0.0f;\n"
    "        hout[0]=9; hout[1]=9; hout[2]=9; hout[3]=9;\n"
    "        KernelBuffer<float32> qx = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<float32> qy = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<float32> qz = heap KernelBuffer<float32>(n);\n"
    "        KernelBuffer<uint32> out = heap KernelBuffer<uint32>(n);\n"
    "        qx.upload(hqx); qy.upload(hqy); qz.upload(hqz); out.upload(hout);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        countHits.launch(s, grid: [1], block: [64])(scene, qx, qy, qz, out, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        if (hout[0] != 1) { return 100; }\n"
    "        if (hout[1] != 0) { return 101; }\n"
    "        if (hout[2] != 1) { return 102; }\n"
    "        if (hout[3] != 1) { return 103; }\n"
    "        return 700 + scene.implSet();\n"   // 704 = OptiX-only (floor dropped); 701 = engine absent
    "    }\n"
    "}\n";


// ── M3 Phase 4 — safe AUTO on CUDA prefers RT cores ──────────────────────────
// Phase 3 delivered the AUTO→OptiX flip via lazy build; this confirms it for the
// TRIANGLE-geometry lazy branch (caj_cuda_as_resolve_optix's build_triangles path,
// kind=1 — 3b only exercised the AABB kind=0 branch) and proves RT-core dispatch under
// AUTO the strong way: the OptiX rep is built (implSet 1→5) so the launch took
// optixLaunch (not the software cubin), AND the nearest-hit result is correct. The four
// canonical shapes otherwise run on RT cores under forced =optix (the device suite); the
// AUTO Unsupported-shape software fallback is covered by softwareOnlyConsumerSkipsOptixBuild.
const char* kAutoTriDriver =
    "package test;\n"
    "import cajeta.xpu.AccelerationStructure;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.RayQuery;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class NearRqAuto {\n"
    "    @Kernel\n"   // SUPPORTED triangle nearest-hit (NearestTri) -> lazy OptiX at launch
    "    public static void nearest(AccelerationStructure scene,\n"
    "                               KernelBuffer<float32> outT, KernelBuffer<uint32> outI) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i == 0) {\n"
    "            RayQuery rq;\n"
    "            rq.initialize(scene, 0, 255,\n"
    "                          0.25f, 0.25f, 10.0f, 0.0f,\n"
    "                          0.0f, 0.0f, -1.0f, 100.0f);\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 0) { rq.confirmIntersection(); }\n"
    "            }\n"
    "            outT[0] = rq.committedDistance();\n"
    "            outI[0] = rq.committedType();\n"
    "            outI[1] = rq.committedPrimitiveIndex();\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        float32[] verts = heap float32[18];\n"
    "        verts[0]=0.0f;  verts[1]=0.0f;  verts[2]=2.0f;\n"
    "        verts[3]=1.0f;  verts[4]=0.0f;  verts[5]=2.0f;\n"
    "        verts[6]=0.0f;  verts[7]=1.0f;  verts[8]=2.0f;\n"
    "        verts[9]=0.0f;  verts[10]=0.0f; verts[11]=4.0f;\n"
    "        verts[12]=1.0f; verts[13]=0.0f; verts[14]=4.0f;\n"
    "        verts[15]=0.0f; verts[16]=1.0f; verts[17]=4.0f;\n"
    "        AccelerationStructure mesh = heap AccelerationStructure(verts, 2, 3);\n"
    "        int32 pre = mesh.implSet();\n"            // expect 1 (software-only, no OptiX yet)
    "        float32[] ht = heap float32[1]; ht[0]=-9.0f;\n"
    "        uint32[] hi = heap uint32[2]; hi[0]=9; hi[1]=9;\n"
    "        KernelBuffer<float32> outT = heap KernelBuffer<float32>(1);\n"
    "        KernelBuffer<uint32> outI = heap KernelBuffer<uint32>(2);\n"
    "        outT.upload(ht); outI.upload(hi);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        nearest.launch(s, grid: [1], block: [64])(mesh, outT, outI);\n"
    "        s.sync();\n"
    "        outT.download(ht); outI.download(hi);\n"
    "        if (hi[0] != 1) { return 100; }\n"        // committed type = triangle
    "        float32 dt = ht[0] - 6.0f;\n"
    "        if (dt * dt > 0.001f) { return 101; }\n"  // nearest distance = 6
    "        if (hi[1] != 1) { return 102; }\n"        // nearest prim = z=4 (idx 1)
    "        if (pre != 1) { return 110; }\n"          // OptiX must NOT exist before launch
    "        return 700 + mesh.implSet();\n"           // 705 = lazy OptiX built (RT cores); 701 = engine absent
    "    }\n"
    "}\n";


// M2 Phase 3-D — the OptiX RT-core VERB end to end through the full compiler. The
// SAME kRqMinDriver as the software/native legs, but with CAJETA_GPU_AS_IMPL=optix:
// the AS builds on the OptiX tier, NvptxRegistration emits the kernel's OptiX program
// set (raygen/intersection/anyhit/miss PTX) alongside its software cubin, and the
// CUDA launch path dispatches countHits.launch to optixLaunch (RT cores) instead of
// cuLaunchKernel. Its 777 must equal the AUTO-software (minimalRayQueryOnNvptxSoftwareBvh),
// native (minimalRayQueryOnDevice), and CPU (minimalRayQueryOnCpuSoftwareBvh) legs —
// the verb following the noun onto a fifth path. On a box without the OptiX runtime
// the AS records software and this still returns 777 via the software cubin (the
// launch's impl branch falls through); on the 4090 it runs on the RT cores.
// The same five walk scenarios on the OptiX RT-core verb (M2 Phases 4/4-B/4-E),
// CAJETA_GPU_AS_IMPL=optix: the AABB scene dispatches via the launch's impl
// branch; the triangle scenarios build the AS on the OptiX tier and
// NvptxRegistration emits the per-shape program sets — nearest-hit (raygen with
// the baked ray + closesthit committing T/type/prim + miss), candidate getters
// (anyhit reading the candidate's tmax + optixGetTriangleBarycentrics into
// out[0..2] then optixIgnoreIntersection), and the CommittedTri per-launch
// dynamic ray (ray components resolved from the initialize() args as
// const-or-buffer[i] loads, closesthit writing hit-flag / front-face 1/2) —
// all dispatched to cajeta_xpu_optix_launch_tri. Each scenario's 777 must
// equal the software/CPU legs. A 5xxx (front-face) failure showing the
// back-hit code means OptiX front-face winding differs from cajeta's det>0
// convention — negate in emitOptixCommittedTriModule.

// ===========================================================================
// AMD (AMDGPU → hsaco, HipDriver) software-BVH ray query — the symmetric twin of
// the NVPTX arm. AMD has no cajeta native inline ray-query seam either, so
// AmdgpuTarget.accelImpl() == SoftwareBvh: the AccelerationStructure builds the
// portable software BVH, the HIP noun provider uploads it into a device buffer, and
// the kernel runs the SoftwareRayQuery walk under its base name. The SAME driver
// sources as the CPU/NVPTX legs, so each AMD 777 must equal the CPU 777 — the verb
// following the noun across the fourth backend. Skips cleanly without a ROCm/HIP
// device.
// ---------------------------------------------------------------------------

