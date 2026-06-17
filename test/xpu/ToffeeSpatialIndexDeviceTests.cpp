//
// Toffee P1.0 — SpatialIndex exec test (cajeta-gpu Part C inc 3c).
//
// Toffee is a consumer of the cajeta-gpu foundation; its first real code is the
// RT-as-compute SpatialIndex primitive (`cajeta-toffee/src/toffee/spatial/
// SpatialIndex.cajeta`). It has no standalone build/test harness yet, so we
// exec-verify it here, through the cajeta compiler's JIT: compile the authoritative
// SpatialIndex source (read from the sibling cajeta-toffee repo) alongside a driver
// program via the multi-source overload, and run a fixed-radius neighbour count on
// a real ray-query device.
//
// Gated twice: on a ray-query-capable Vulkan device, and on the cajeta-toffee
// source being present next to this checkout. Either missing -> SKIP.
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

namespace {

// The authoritative SpatialIndex.cajeta lives in the sibling cajeta-toffee repo.
// Resolve it relative to CAJETA_SOURCE_ROOT (the cajeta checkout) — its parent is
// the cpp/ workspace dir, with cajeta-toffee alongside. Returns "" if unreadable.
std::string readSpatialIndexSource() {
    const char* root = std::getenv("CAJETA_SOURCE_ROOT");
    if (!root || !*root) return "";
    std::string path = std::string(root) +
        "/../cajeta-toffee/src/toffee/spatial/SpatialIndex.cajeta";
    std::ifstream f(path);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Driver: index three points spread along x at 0, 10, 20 with radius 0.5, then
// count neighbours for four query points. Inside a datum's box -> 1; between
// boxes -> 0. countWithin is the public SpatialIndex verb; the ray-query walk is
// hidden inside it.
const char* kDriver =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import toffee.spatial.SpatialIndex;\n"
    "public class ToffeeRQ {\n"
    "    public static int32 run() {\n"
    "        uint32 np = 3;\n"
    "        float32[] pts = heap float32[np * 3];\n"
    "        pts[0] = 0.0f;  pts[1] = 0.0f; pts[2] = 0.0f;\n"
    "        pts[3] = 10.0f; pts[4] = 0.0f; pts[5] = 0.0f;\n"
    "        pts[6] = 20.0f; pts[7] = 0.0f; pts[8] = 0.0f;\n"
    "        SpatialIndex idx = heap SpatialIndex(pts, np, 0.5f);\n"
    "        uint32 n = 4;\n"
    "        float32[] hqx = heap float32[n];\n"
    "        float32[] hqy = heap float32[n];\n"
    "        float32[] hqz = heap float32[n];\n"
    "        uint32[] hout = heap uint32[n];\n"
    "        hqx[0] = 0.0f;  hqy[0] = 0.0f; hqz[0] = 0.0f;\n"   // on point 0 -> 1
    "        hqx[1] = 5.0f;  hqy[1] = 0.0f; hqz[1] = 0.0f;\n"   // between      -> 0
    "        hqx[2] = 10.0f; hqy[2] = 0.0f; hqz[2] = 0.0f;\n"   // on point 1 -> 1
    "        hqx[3] = 19.7f; hqy[3] = 0.0f; hqz[3] = 0.0f;\n"   // within 0.5 of pt2 -> 1
    "        hout[0] = 99; hout[1] = 99; hout[2] = 99; hout[3] = 99;\n"
    "        Buffer<float32> qx = heap Buffer<float32>(0, n);\n"
    "        Buffer<float32> qy = heap Buffer<float32>(0, n);\n"
    "        Buffer<float32> qz = heap Buffer<float32>(0, n);\n"
    "        Buffer<uint32> out = heap Buffer<uint32>(0, n);\n"
    "        qx.allocate(); qy.allocate(); qz.allocate(); out.allocate();\n"
    "        qx.upload(hqx); qy.upload(hqy); qz.upload(hqz); out.upload(hout);\n"
    "        idx.countWithin(qx, qy, qz, out, n);\n"
    "        out.download(hout);\n"
    "        qx.free(); qy.free(); qz.free(); out.free();\n"
    "        if (hout[0] != 1) { return 100; }\n"
    "        if (hout[1] != 0) { return 101; }\n"
    "        if (hout[2] != 1) { return 102; }\n"
    "        if (hout[3] != 1) { return 103; }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

// Exact-L2 driver (P1.1): 3 points; the box (L-inf, half-extent 1.0) approximation
// reports all 3 as neighbours of the origin, but only P0 is within Euclidean 0.7 —
// radiusExact uses the candidate primitive index to refine to the true distance.
const char* kExactDriver =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import toffee.spatial.SpatialIndex;\n"
    "public class ToffeeExact {\n"
    "    public static int32 run() {\n"
    "        uint32 np = 3;\n"
    "        float32[] pts = heap float32[np * 3];\n"
    "        pts[0]=0.0f; pts[1]=0.0f; pts[2]=0.0f;\n"   // P0 origin
    "        pts[3]=0.9f; pts[4]=0.0f; pts[5]=0.0f;\n"   // P1 L2=0.9
    "        pts[6]=0.6f; pts[7]=0.6f; pts[8]=0.0f;\n"   // P2 L2~0.849
    "        SpatialIndex idx = heap SpatialIndex(pts, np, 1.0f);\n"  // build radius 1.0
    "        float32[] hdx = heap float32[np]; float32[] hdy = heap float32[np]; float32[] hdz = heap float32[np];\n"
    "        hdx[0]=0.0f; hdy[0]=0.0f; hdz[0]=0.0f;\n"
    "        hdx[1]=0.9f; hdy[1]=0.0f; hdz[1]=0.0f;\n"
    "        hdx[2]=0.6f; hdy[2]=0.6f; hdz[2]=0.0f;\n"
    "        Buffer<float32> dx = heap Buffer<float32>(0, np);\n"
    "        Buffer<float32> dy = heap Buffer<float32>(0, np);\n"
    "        Buffer<float32> dz = heap Buffer<float32>(0, np);\n"
    "        dx.allocate(); dy.allocate(); dz.allocate();\n"
    "        dx.upload(hdx); dy.upload(hdy); dz.upload(hdz);\n"
    "        uint32 n = 1;\n"
    "        float32[] hq = heap float32[n]; hq[0]=0.0f;\n"
    "        Buffer<float32> qx = heap Buffer<float32>(0, n);\n"
    "        Buffer<float32> qy = heap Buffer<float32>(0, n);\n"
    "        Buffer<float32> qz = heap Buffer<float32>(0, n);\n"
    "        qx.allocate(); qy.allocate(); qz.allocate();\n"
    "        qx.upload(hq); qy.upload(hq); qz.upload(hq);\n"
    "        uint32[] hb = heap uint32[n]; hb[0]=99;\n"
    "        Buffer<uint32> oExact = heap Buffer<uint32>(0, n);\n"
    "        Buffer<uint32> oBox = heap Buffer<uint32>(0, n);\n"
    "        oExact.allocate(); oBox.allocate();\n"
    "        oExact.upload(hb); oBox.upload(hb);\n"
    "        idx.radiusExact(dx, dy, dz, qx, qy, qz, oExact, n, 0.7f);\n"
    "        idx.countWithin(qx, qy, qz, oBox, n);\n"
    "        uint32[] hexact = heap uint32[n]; uint32[] hbox = heap uint32[n];\n"
    "        oExact.download(hexact); oBox.download(hbox);\n"
    "        dx.free(); dy.free(); dz.free(); qx.free(); qy.free(); qz.free();\n"
    "        oExact.free(); oBox.free();\n"
    "        if (hbox[0] != 3) { return 300 + (int32) hbox[0]; }\n"   // box approx over-counts
    "        if (hexact[0] != 1) { return 200 + (int32) hexact[0]; }\n" // exact L2 0.7 -> only P0
    "        return 888;\n"
    "    }\n"
    "}\n";

} // namespace

// The Toffee SpatialIndex primitive, end to end on a real RT device: build a BVH
// over points, run a fixed-radius neighbour count through the public verb. Proves
// the foundation's ray-query path (3a/3b) is consumable as a library abstraction
// (3c) — the user writes `idx.countWithin(...)`, never a ray.
TEST(ToffeeSpatialIndexDeviceTests, fixedRadiusCountOnDevice) {
    if (!VulkanDriver::rayQueryAvailable()) {
        GTEST_SKIP() << "no Vulkan ray-query (acceleration-structure) device";
    }
    std::string lib = readSpatialIndexSource();
    if (lib.empty()) {
        GTEST_SKIP() << "cajeta-toffee SpatialIndex.cajeta not found beside checkout";
    }
    std::map<std::string, std::string> sources = {
        {"toffee.spatial.SpatialIndex", lib},
        {"test.ToffeeRQ", kDriver},
    };
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(sources, "test.ToffeeRQ", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (10x: countWithin returned the wrong neighbour count)";
}

// P1.1 — exact-L2 refinement via the candidate primitive index (the new
// OpRayQueryGetIntersectionPrimitiveIndexKHR op). The box approximation over-counts
// (3 of 3 boxes contain the origin); radiusExact recovers each candidate's data
// point and keeps only the one within the true Euclidean radius (1).
TEST(ToffeeSpatialIndexDeviceTests, exactL2RefinementOnDevice) {
    if (!VulkanDriver::rayQueryAvailable()) {
        GTEST_SKIP() << "no Vulkan ray-query (acceleration-structure) device";
    }
    std::string lib = readSpatialIndexSource();
    if (lib.empty()) {
        GTEST_SKIP() << "cajeta-toffee SpatialIndex.cajeta not found beside checkout";
    }
    std::map<std::string, std::string> sources = {
        {"toffee.spatial.SpatialIndex", lib},
        {"test.ToffeeExact", kExactDriver},
    };
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(sources, "test.ToffeeExact", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 888) << "fail code " << r
                      << " (3xx: box approx != 3; 2xx: exact-L2 count != 1)";
}

// Minimal self-contained CPU ray-query exec (no SpatialIndex / cajeta-toffee
// dependency): build an AccelerationStructure over 3 AABBs and run a RayQuery walk
// in a kernel on the CPU software path. Directly exercises the ray-query-to-core
// integration (software BVH builder + SoftwareRayQuery walk) without the broader
// stdlib closure. Same 1/0/1/1 expectation as the Toffee fixed-radius scene.
const char* kRqMinDriver =
    "package test;\n"
    "import cajeta.gpu.core.AccelerationStructure;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.RayQuery;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class RqMin {\n"
    "    @Kernel\n"
    "    public static void countHits(AccelerationStructure scene,\n"
    "                                 Buffer<float32> qx, Buffer<float32> qy,\n"
    "                                 Buffer<float32> qz, Buffer<uint32> out,\n"
    "                                 uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
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
    "        Buffer<float32> qx = heap Buffer<float32>(n);\n"
    "        Buffer<float32> qy = heap Buffer<float32>(n);\n"
    "        Buffer<float32> qz = heap Buffer<float32>(n);\n"
    "        Buffer<uint32> out = heap Buffer<uint32>(n);\n"
    "        qx.upload(hqx); qy.upload(hqy); qz.upload(hqz); out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
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
    "import cajeta.gpu.core.AccelerationStructure;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.RayQuery;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class TriRq {\n"
    "    @Kernel\n"
    "    public static void countTri(AccelerationStructure scene,\n"
    "                                Buffer<float32> ox, Buffer<float32> oy,\n"
    "                                Buffer<uint32> out, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
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
    "        AccelerationStructure mesh = heap AccelerationStructure(verts, nt, 3u);\n"
    "        uint32 n = 5;\n"
    "        float32[] hox = heap float32[n]; float32[] hoy = heap float32[n];\n"
    "        uint32[] hout = heap uint32[n];\n"
    "        hox[0]=0.2f;  hoy[0]=0.2f;\n"   // through tri0 interior -> 1
    "        hox[1]=3.2f;  hoy[1]=0.2f;\n"   // through tri1 -> 1
    "        hox[2]=6.2f;  hoy[2]=0.2f;\n"   // through tri2 -> 1
    "        hox[3]=9.2f;  hoy[3]=0.2f;\n"   // through tri3 -> 1
    "        hox[4]=50.0f; hoy[4]=50.0f;\n"  // misses all -> 0
    "        hout[0]=9; hout[1]=9; hout[2]=9; hout[3]=9; hout[4]=9;\n"
    "        Buffer<float32> ox = heap Buffer<float32>(n);\n"
    "        Buffer<float32> oy = heap Buffer<float32>(n);\n"
    "        Buffer<uint32> out = heap Buffer<uint32>(n);\n"
    "        ox.upload(hox); oy.upload(hoy); out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
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
    "import cajeta.gpu.core.AccelerationStructure;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.RayQuery;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class BaryRq {\n"
    "    @Kernel\n"
    "    public static void getBary(AccelerationStructure scene, Buffer<float32> out) {\n"
    "        uint32 i = Thread.globalIdX();\n"
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
    "        Buffer<float32> out = heap Buffer<float32>(3);\n"
    "        out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
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
    "import cajeta.gpu.core.AccelerationStructure;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.RayQuery;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class NearRq {\n"
    "    @Kernel\n"
    "    public static void nearest(AccelerationStructure scene,\n"
    "                               Buffer<float32> outT, Buffer<uint32> outI) {\n"
    "        uint32 i = Thread.globalIdX();\n"
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
    "        Buffer<float32> outT = heap Buffer<float32>(1);\n"
    "        Buffer<uint32> outI = heap Buffer<uint32>(2);\n"
    "        outT.upload(ht); outI.upload(hi);\n"
    "        Stream s = Stream.current();\n"
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

TEST(ToffeeSpatialIndexDeviceTests, nearestHitOnCpuSoftwareBvh) {
    std::map<std::string, std::string> sources = {{"test.NearRq", kNearestDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(sources, "test.NearRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100: committed type; 101: nearest distance; 102: prim)";
}

TEST(ToffeeSpatialIndexDeviceTests, candidateGettersOnCpuSoftwareBvh) {
    std::map<std::string, std::string> sources = {{"test.BaryRq", kBaryDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(sources, "test.BaryRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100: distance; 101: barycentric u; 102: v)";
}

// Inc 3b: front-face getter. A CCW triangle in z=0 (geometric normal +z): a ray
// from above (+z, going down) hits the front (true); a ray from below hits the
// back (false). The cajeta convention (MT det > 0) is set to match Vulkan's
// default CCW front-face, so CPU and native agree.
const char* kFrontDriver =
    "package test;\n"
    "import cajeta.gpu.core.AccelerationStructure;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.RayQuery;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class FrontRq {\n"
    "    @Kernel\n"
    "    public static void getFront(AccelerationStructure scene,\n"
    "                                Buffer<float32> oz, Buffer<float32> dz,\n"
    "                                Buffer<uint32> out, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
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
    "        Buffer<float32> oz = heap Buffer<float32>(n);\n"
    "        Buffer<float32> dz = heap Buffer<float32>(n);\n"
    "        Buffer<uint32> out = heap Buffer<uint32>(n);\n"
    "        oz.upload(hoz); dz.upload(hdz); out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
    "        getFront.launch(s, grid: [1], block: [64])(tri, oz, dz, out, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        if (hout[0] != 1) { return 100; }\n"   // front hit -> true
    "        if (hout[1] != 2) { return 101; }\n"   // back hit  -> false
    "        return 777;\n"
    "    }\n"
    "}\n";

TEST(ToffeeSpatialIndexDeviceTests, frontFaceOnCpuSoftwareBvh) {
    std::map<std::string, std::string> sources = {{"test.FrontRq", kFrontDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(sources, "test.FrontRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100: front hit; 101: back hit)";
}

TEST(ToffeeSpatialIndexDeviceTests, frontFaceOnDevice) {
    if (!VulkanDriver::rayQueryAvailable()) {
        GTEST_SKIP() << "no Vulkan ray-query (acceleration-structure) device";
    }
    std::map<std::string, std::string> sources = {{"test.FrontRq", kFrontDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(sources, "test.FrontRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (Vulkan native front-face != software)";
}

// Inc 3b native cross-checks: the SAME getter / nearest-hit sources on the Vulkan
// NATIVE path (OpRayQueryGetIntersectionT / Barycentrics, Confirm/Generate via the
// new cajeta-llvm fork intrinsics) must match the CPU software results.
TEST(ToffeeSpatialIndexDeviceTests, candidateGettersOnDevice) {
    if (!VulkanDriver::rayQueryAvailable()) {
        GTEST_SKIP() << "no Vulkan ray-query (acceleration-structure) device";
    }
    std::map<std::string, std::string> sources = {{"test.BaryRq", kBaryDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(sources, "test.BaryRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (Vulkan native candidate getters != software)";
}

TEST(ToffeeSpatialIndexDeviceTests, nearestHitOnDevice) {
    if (!VulkanDriver::rayQueryAvailable()) {
        GTEST_SKIP() << "no Vulkan ray-query (acceleration-structure) device";
    }
    std::map<std::string, std::string> sources = {{"test.NearRq", kNearestDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(sources, "test.NearRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (Vulkan native nearest-hit (confirm) != software)";
}

TEST(ToffeeSpatialIndexDeviceTests, triangleRayQueryOnCpuSoftwareBvh) {
    std::map<std::string, std::string> sources = {{"test.TriRq", kTriDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(sources, "test.TriRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (CPU software triangle ray query: wrong hit count)";
}

// The mesh cross-check (inc-2 done bar): the SAME triangle source on the Vulkan
// NATIVE path (VK_GEOMETRY_TYPE_TRIANGLES_KHR + OpRayQuery, Möller-Trumbore in
// hardware) must produce the same hit counts as the CPU software walk above.
// Non-opaque geometry so triangle candidates enumerate in the proceed() loop.
TEST(ToffeeSpatialIndexDeviceTests, triangleRayQueryOnDevice) {
    if (!VulkanDriver::rayQueryAvailable()) {
        GTEST_SKIP() << "no Vulkan ray-query (acceleration-structure) device";
    }
    std::map<std::string, std::string> sources = {{"test.TriRq", kTriDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(sources, "test.TriRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (Vulkan native triangle ray query != software)";
}

TEST(ToffeeSpatialIndexDeviceTests, minimalRayQueryOnCpuSoftwareBvh) {
    std::map<std::string, std::string> sources = {{"test.RqMin", kRqMinDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(sources, "test.RqMin", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (CPU software ray query: wrong hit count)";
}

// ── Ray-query-to-core (inc 1): the SAME Toffee source on the CPU SOFTWARE path ──
// No ray-query-capable device required — the AccelerationStructure builds a
// portable software BVH (runtime/native/cajeta_bvh.c) and RayQuery lowers to the
// cajeta SoftwareRayQuery walk. The results must match the Vulkan native path
// above (777 / 888): that agreement is what makes inline ray query genuinely core.
// Each ctest case is a fresh process, so the CPU-only bundle selects the CPU
// backend (priority CUDA>HIP>Vulkan>CPU is moot when only CPU is bundled).

TEST(ToffeeSpatialIndexDeviceTests, fixedRadiusCountOnCpuSoftwareBvh) {
    std::string lib = readSpatialIndexSource();
    if (lib.empty()) {
        GTEST_SKIP() << "cajeta-toffee SpatialIndex.cajeta not found beside checkout";
    }
    std::map<std::string, std::string> sources = {
        {"toffee.spatial.SpatialIndex", lib},
        {"test.ToffeeRQ", kDriver},
    };
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(sources, "test.ToffeeRQ", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (CPU software BVH/RayQuery: wrong neighbour count)";
}

TEST(ToffeeSpatialIndexDeviceTests, exactL2RefinementOnCpuSoftwareBvh) {
    std::string lib = readSpatialIndexSource();
    if (lib.empty()) {
        GTEST_SKIP() << "cajeta-toffee SpatialIndex.cajeta not found beside checkout";
    }
    std::map<std::string, std::string> sources = {
        {"toffee.spatial.SpatialIndex", lib},
        {"test.ToffeeExact", kExactDriver},
    };
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(sources, "test.ToffeeExact", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 888) << "fail code " << r
                      << " (CPU software: 3xx box approx != 3; 2xx exact-L2 != 1)";
}

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
    "import cajeta.gpu.core.AccelerationStructure;\n"
    "import cajeta.gpu.core.AsImpl;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.RayQuery;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class RqMinSw {\n"
    "    @Kernel\n"
    "    public static void countHits(AccelerationStructure scene,\n"
    "                                 Buffer<float32> qx, Buffer<float32> qy,\n"
    "                                 Buffer<float32> qz, Buffer<uint32> out,\n"
    "                                 uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
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
    "        AccelerationStructure scene = AccelerationStructure.of(boxes, np, AsImpl.Software);\n"
    "        uint32 n = 4;\n"
    "        float32[] hqx = heap float32[n]; float32[] hqy = heap float32[n]; float32[] hqz = heap float32[n];\n"
    "        uint32[] hout = heap uint32[n];\n"
    "        hqx[0]=0.0f;  hqy[0]=0.0f; hqz[0]=0.0f;\n"
    "        hqx[1]=5.0f;  hqy[1]=0.0f; hqz[1]=0.0f;\n"
    "        hqx[2]=10.0f; hqy[2]=0.0f; hqz[2]=0.0f;\n"
    "        hqx[3]=19.7f; hqy[3]=0.0f; hqz[3]=0.0f;\n"
    "        hout[0]=9; hout[1]=9; hout[2]=9; hout[3]=9;\n"
    "        Buffer<float32> qx = heap Buffer<float32>(n);\n"
    "        Buffer<float32> qy = heap Buffer<float32>(n);\n"
    "        Buffer<float32> qz = heap Buffer<float32>(n);\n"
    "        Buffer<uint32> out = heap Buffer<uint32>(n);\n"
    "        qx.upload(hqx); qy.upload(hqy); qz.upload(hqz); out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
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
    "import cajeta.gpu.core.AccelerationStructure;\n"
    "import cajeta.gpu.core.AsImpl;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.RayQuery;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class RqMinNat {\n"
    "    @Kernel\n"
    "    public static void countHits(AccelerationStructure scene,\n"
    "                                 Buffer<float32> qx, Buffer<float32> qy,\n"
    "                                 Buffer<float32> qz, Buffer<uint32> out,\n"
    "                                 uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
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
    "        AccelerationStructure scene = AccelerationStructure.of(boxes, np, AsImpl.Native);\n"
    "        uint32 n = 4;\n"
    "        float32[] hqx = heap float32[n]; float32[] hqy = heap float32[n]; float32[] hqz = heap float32[n];\n"
    "        uint32[] hout = heap uint32[n];\n"
    "        hqx[0]=0.0f;  hqy[0]=0.0f; hqz[0]=0.0f;\n"
    "        hqx[1]=5.0f;  hqy[1]=0.0f; hqz[1]=0.0f;\n"
    "        hqx[2]=10.0f; hqy[2]=0.0f; hqz[2]=0.0f;\n"
    "        hqx[3]=19.7f; hqy[3]=0.0f; hqz[3]=0.0f;\n"
    "        hout[0]=9; hout[1]=9; hout[2]=9; hout[3]=9;\n"
    "        Buffer<float32> qx = heap Buffer<float32>(n);\n"
    "        Buffer<float32> qy = heap Buffer<float32>(n);\n"
    "        Buffer<float32> qz = heap Buffer<float32>(n);\n"
    "        Buffer<uint32> out = heap Buffer<uint32>(n);\n"
    "        qx.upload(hqx); qy.upload(hqy); qz.upload(hqz); out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
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
    "import cajeta.gpu.core.AccelerationStructure;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.RayQuery;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class RqImpl {\n"
    "    @Kernel\n"
    "    public static void countHits(AccelerationStructure scene,\n"
    "                                 Buffer<float32> qx, Buffer<float32> qy,\n"
    "                                 Buffer<float32> qz, Buffer<uint32> out,\n"
    "                                 uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
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
    "        Buffer<float32> qx = heap Buffer<float32>(n);\n"
    "        Buffer<float32> qy = heap Buffer<float32>(n);\n"
    "        Buffer<float32> qz = heap Buffer<float32>(n);\n"
    "        Buffer<uint32> out = heap Buffer<uint32>(n);\n"
    "        qx.upload(hqx); qy.upload(hqy); qz.upload(hqz); out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
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

// The honest proof that the AUTO *OnDevice native legs are genuinely native and
// not silently running the software fallback: the default (AUTO) ctor on a
// ray-query Vulkan device must RECORD native (implTag 1) -> 701. Before the Win32
// un-gate of caj_native_rayquery_available(), AUTO resolved to software (-> 700)
// on Windows even on the 4090, so this test guards the fix on-device.
TEST(ToffeeSpatialIndexDeviceTests, autoRecordsNativeImplOnDevice) {
    if (!VulkanDriver::rayQueryAvailable()) {
        GTEST_SKIP() << "no Vulkan ray-query (acceleration-structure) device";
    }
    std::map<std::string, std::string> sources = {{"test.RqImpl", kImplProbeDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(sources, "test.RqImpl", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 701) << "fail code " << r
                      << " (1xx: wrong counts; 700: AUTO recorded SOFTWARE, not "
                         "native — the Win32 gate is still downgrading on-device)";
}

// Native leg: kRqMinDriver (default Auto ctor) on a ray-query-capable Vulkan
// device — the native BLAS + OpRayQuery. The reference 777 the forced-software
// leg below must match.
TEST(ToffeeSpatialIndexDeviceTests, minimalRayQueryOnDevice) {
    if (!VulkanDriver::rayQueryAvailable()) {
        GTEST_SKIP() << "no Vulkan ray-query (acceleration-structure) device";
    }
    std::map<std::string, std::string> sources = {{"test.RqMin", kRqMinDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(sources, "test.RqMin", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (Vulkan native AABB ray query: wrong hit count)";
}

// Forced-software leg (the brick's proof): AsImpl.Software on the SAME Vulkan
// device builds a software BVH and runs the $sw variant; its 777 == the native leg
// above == the CPU leg (minimalRayQueryOnCpuSoftwareBvh). One backend, either
// impl, the verb following the noun.
TEST(ToffeeSpatialIndexDeviceTests, forcedSoftwareOfApiOnDevice) {
    if (!VulkanDriver::rayQueryAvailable()) {
        GTEST_SKIP() << "no Vulkan ray-query (acceleration-structure) device";
    }
    std::map<std::string, std::string> sources = {{"test.RqMinSw", kRqMinSoftwareDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(sources, "test.RqMinSw", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (Vulkan FORCED-SOFTWARE via AsImpl.Software != native/CPU)";
}

// Forced-NATIVE leg: AsImpl.Native on the SAME Vulkan device builds the native
// VK_KHR_acceleration_structure BLAS and traces via OpRayQuery (the RT-core path);
// its 777 == the AUTO native leg (minimalRayQueryOnDevice) == the forced-software
// leg == the CPU leg. The explicit override proves native is selectable on its
// own, not just as the AUTO default — so this stays green even if a future change
// alters the AUTO/caj_native_rayquery_available policy.
TEST(ToffeeSpatialIndexDeviceTests, forcedNativeOfApiOnDevice) {
    if (!VulkanDriver::rayQueryAvailable()) {
        GTEST_SKIP() << "no Vulkan ray-query (acceleration-structure) device";
    }
    std::map<std::string, std::string> sources = {{"test.RqMinNat", kRqMinNativeDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(sources, "test.RqMinNat", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (Vulkan FORCED-NATIVE via AsImpl.Native != AUTO-native/software/CPU)";
}

// ===========================================================================
// NVIDIA (NVPTX → cubin, CudaDriver) software-BVH ray query. NVIDIA has no cajeta
// native inline ray-query seam, so NvptxTarget.accelImpl() == SoftwareBvh: the
// AccelerationStructure noun builds the portable software BVH (the shared CPU
// builder), the CUDA noun provider uploads it into a device buffer, and the kernel
// runs the cajeta.gpu.core.SoftwareRayQuery walk under its base name (no $sw twin
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

TEST(ToffeeSpatialIndexDeviceTests, minimalRayQueryOnNvptxSoftwareBvh) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    AsImplEnvGuard forceSoftware("software");   // keep the NVPTX software-walk coverage
    std::map<std::string, std::string> sources = {{"test.RqMin", kRqMinDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.RqMin", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (NVPTX software AABB ray query: wrong hit count)";
}

TEST(ToffeeSpatialIndexDeviceTests, triangleRayQueryOnNvptxSoftwareBvh) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    AsImplEnvGuard forceSoftware("software");   // keep the NVPTX software-walk coverage
    std::map<std::string, std::string> sources = {{"test.TriRq", kTriDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.TriRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (NVPTX software triangle ray query != CPU)";
}

TEST(ToffeeSpatialIndexDeviceTests, nearestHitOnNvptxSoftwareBvh) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    AsImplEnvGuard forceSoftware("software");   // keep the NVPTX software-walk coverage
    std::map<std::string, std::string> sources = {{"test.NearRq", kNearestDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.NearRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (NVPTX nearest-hit confirm/committed != CPU)";
}

TEST(ToffeeSpatialIndexDeviceTests, candidateGettersOnNvptxSoftwareBvh) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    AsImplEnvGuard forceSoftware("software");   // keep the NVPTX software-walk coverage
    std::map<std::string, std::string> sources = {{"test.BaryRq", kBaryDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.BaryRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (NVPTX candidate distance/barycentrics != CPU)";
}

TEST(ToffeeSpatialIndexDeviceTests, frontFaceOnNvptxSoftwareBvh) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    AsImplEnvGuard forceSoftware("software");   // keep the NVPTX software-walk coverage
    std::map<std::string, std::string> sources = {{"test.FrontRq", kFrontDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.FrontRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (NVPTX committed front-face != CPU)";
}

// ── NVIDIA OptiX RT-core AS tier (Milestone 1) ──────────────────────────────
// (AsImplEnvGuard is defined above, before the software-BVH section.)

// Build-only driver (no kernel launch): the OptiX *verb* (traversal via optixTrace)
// is M2, so M1 validates that the CUDA noun provider BUILDS + RECORDS the OptiX AS
// tier and frees it. run() builds an AABB AS and returns 700 + implTag(): on the
// 4090 with the OptiX SDK and CAJETA_GPU_AS_IMPL=optix that is 702 (CAJ_AS_IMPL_OPTIX);
// 700 would mean it fell back to the software floor (OptiX unavailable at runtime).
const char* kOptixImplDriver =
    "package test;\n"
    "import cajeta.gpu.core.AccelerationStructure;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class RqOptix {\n"
    // A @Kernel (even unlaunched) makes the bundle register the CUDA backend at
    // module init, so it is the active backend when the AS builds below — without
    // it the build wouldn't route to the CUDA noun provider's OptiX arm.
    "    @Kernel\n"
    "    public static void noop(Buffer<uint32> b) {\n"
    "        uint32 i = Thread.globalIdX(); if (i == 0) { b[0] = 1; }\n"
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

TEST(ToffeeSpatialIndexDeviceTests, optixRecordsImplOnNvptxDevice) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
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
    "import cajeta.gpu.core.AccelerationStructure;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class RqSet {\n"
    "    @Kernel\n"
    "    public static void noop(Buffer<uint32> b) {\n"
    "        uint32 i = Thread.globalIdX(); if (i == 0) { b[0] = 1; }\n"
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

TEST(ToffeeSpatialIndexDeviceTests, multiImplAsRecordsSoftwareAndOptix) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    AsImplEnvGuard forceOptix("optix");
    std::map<std::string, std::string> sources = {{"test.RqSet", kImplSetDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.RqSet", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int set = fn();
    // OptiX bit (1<<2 = 4) present only if OptiX actually built on-device; skip if not.
    if (!(set & 4)) {
        GTEST_SKIP() << "CUDA device present but OptiX AS not built (engine absent); "
                        "set=" << set;
    }
    EXPECT_TRUE(set & 1) << "OptiX-primary AS did not retain the software-BVH floor "
                            "(implSet=" << set << ", expected the Software bit 1 set)";
    EXPECT_EQ(set, 5) << "implSet=" << set << " (expected Optix|Software = 5)";
}

// M3 Phase 1 — implSet() on an AUTO (software-primary) AS reports software-only
// (bit 1), and implTag() still returns the single primary tag. Backward-compat:
// AUTO on CUDA stays software (the M2 4-C policy holds until M3 Phase 4 flips it).
TEST(ToffeeSpatialIndexDeviceTests, multiImplAsSoftwareOnlyUnderAuto) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    unsetenv("CAJETA_GPU_AS_IMPL");   // AUTO
    std::map<std::string, std::string> sources = {{"test.RqSet", kImplSetDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.RqSet", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int set = fn();
    EXPECT_EQ(set, 1) << "implSet=" << set
                      << " (AUTO on CUDA must be software-only [bit 1] until the M3 flip)";
}

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
    "import cajeta.gpu.core.AccelerationStructure;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.RayQuery;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class RqSelect {\n"
    "    @Kernel\n"   // SUPPORTED: canonical AABB candidate-count -> OptiX RT cores
    "    public static void countHits(AccelerationStructure scene,\n"
    "                                 Buffer<float32> qx, Buffer<float32> qy,\n"
    "                                 Buffer<float32> qz, Buffer<uint32> out,\n"
    "                                 uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
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
    "    @Kernel\n"   // UNSUPPORTED shape (AS, 2 Buffer, 1 scalar) -> software floor
    "    public static void countOne(AccelerationStructure scene,\n"
    "                                Buffer<float32> qx, Buffer<uint32> out,\n"
    "                                uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
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
    "        Buffer<float32> qx = heap Buffer<float32>(n);\n"
    "        Buffer<float32> qy = heap Buffer<float32>(n);\n"
    "        Buffer<float32> qz = heap Buffer<float32>(n);\n"
    "        Buffer<uint32> out = heap Buffer<uint32>(n);\n"
    "        qx.upload(hqx); qy.upload(hqy); qz.upload(hqz); out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
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
TEST(ToffeeSpatialIndexDeviceTests, sameAsTwoKernelsSelectsPerLaunch) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    AsImplEnvGuard forceOptix("optix");
    std::map<std::string, std::string> sources = {{"test.RqSelect", kSelectDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.RqSelect", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (1xx: supported shape on OptiX wrong; 2xx: Unsupported shape "
                         "did not fall back to the software floor correctly — the M3 "
                         "launch-time selection / floor swap regressed)";
}

// 2b — forced =optix with an UNSUPPORTED shape must NOT fault on the OptixAs* primary:
// it falls back to the retained software floor and returns the correct counts. Driver
// runs ONLY the unsupported kernel, in isolation, against an OptiX-forced AS.
const char* kUnsupOptixDriver =
    "package test;\n"
    "import cajeta.gpu.core.AccelerationStructure;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.RayQuery;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class RqUnsup {\n"
    "    @Kernel\n"   // (AS, 2 Buffer, 1 scalar) -> Unsupported -> software floor
    "    public static void countOne(AccelerationStructure scene,\n"
    "                                Buffer<float32> qx, Buffer<uint32> out,\n"
    "                                uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
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
    "        Buffer<float32> qx = heap Buffer<float32>(n);\n"
    "        Buffer<uint32> out = heap Buffer<uint32>(n);\n"
    "        qx.upload(hqx); out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
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

TEST(ToffeeSpatialIndexDeviceTests, forcedOptixUnsupportedShapeFallsBackToSoftware) {
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
TEST(ToffeeSpatialIndexDeviceTests, autoRecordsSoftwareImplOnNvptxDevice) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    // No AsImplEnvGuard — exercise AUTO. (unset defensively in case the env leaked.)
    unsetenv("CAJETA_GPU_AS_IMPL");
    std::map<std::string, std::string> sources = {{"test.RqOptix", kOptixImplDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.RqOptix", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 700) << "fail code " << r
                      << " (702: AUTO BUILD recorded OptiX — under M3 the AUTO build still "
                         "records the software floor as the primary; OptiX is built LAZILY "
                         "at the first supported-shape launch, not at build time)";
}

// ── M3 Phase 3 — lazy / elidable native build ───────────────────────────────
// Under AUTO the build records the software-BVH floor as the primary and does NOT build
// the OptiX rep; that is deferred to the first SUPPORTED-shape launch (R4 cost control —
// an AS consumed only by software kernels never pays for OptiX). implSet() observes the
// transition: software-only (1) right after build, software|OptiX (5) after a supported
// launch lazily builds the OptiX rep. (The drop-software-floor hint of 3c is deferred —
// a spec-level MAY; the floor is always retained for now.)

// 3a — a software-only consumer never triggers the lazy OptiX build. Under AUTO, build an
// AABB AS and launch an UNSUPPORTED-shape kernel (AS, 2 Buffer, 1 scalar → software cubin
// over the floor); implSet() must stay software-only (1) afterwards. run() returns
// 700 + implSet() → expect 701.
const char* kLazySoftOnlyDriver =
    "package test;\n"
    "import cajeta.gpu.core.AccelerationStructure;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.RayQuery;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class RqLazySoft {\n"
    "    @Kernel\n"   // Unsupported shape -> software cubin, no OptiX program
    "    public static void countOne(AccelerationStructure scene,\n"
    "                                Buffer<float32> qx, Buffer<uint32> out,\n"
    "                                uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
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
    "        Buffer<float32> qx = heap Buffer<float32>(n);\n"
    "        Buffer<uint32> out = heap Buffer<uint32>(n);\n"
    "        qx.upload(hqx); out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
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

TEST(ToffeeSpatialIndexDeviceTests, softwareOnlyConsumerSkipsOptixBuild) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    unsetenv("CAJETA_GPU_AS_IMPL");   // AUTO
    std::map<std::string, std::string> sources = {{"test.RqLazySoft", kLazySoftOnlyDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.RqLazySoft", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 701) << "fail code " << r
                      << " (1xx: wrong counts; 705: OptiX was built for a software-only "
                         "consumer — the lazy build fired when it must not)";
}

// 3b — the OptiX rep is absent right after build (implSet()==1) and present after the
// first SUPPORTED-shape launch (implSet()==5): the lazy build fires on demand. Under
// AUTO; run() checks the pre-launch set, launches the canonical AABB-count shape, then
// returns 700 + post-launch implSet() (705 = lazy OptiX built; 701 = OptiX engine absent
// on this box → skip).
const char* kLazyBuildDriver =
    "package test;\n"
    "import cajeta.gpu.core.AccelerationStructure;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.RayQuery;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class RqLazyBuild {\n"
    "    @Kernel\n"   // SUPPORTED canonical AABB candidate-count -> lazy OptiX at launch
    "    public static void countHits(AccelerationStructure scene,\n"
    "                                 Buffer<float32> qx, Buffer<float32> qy,\n"
    "                                 Buffer<float32> qz, Buffer<uint32> out,\n"
    "                                 uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
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
    "        Buffer<float32> qx = heap Buffer<float32>(n);\n"
    "        Buffer<float32> qy = heap Buffer<float32>(n);\n"
    "        Buffer<float32> qz = heap Buffer<float32>(n);\n"
    "        Buffer<uint32> out = heap Buffer<uint32>(n);\n"
    "        qx.upload(hqx); qy.upload(hqy); qz.upload(hqz); out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
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

TEST(ToffeeSpatialIndexDeviceTests, lazyOptixBuiltOnFirstNativeLaunch) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    unsetenv("CAJETA_GPU_AS_IMPL");   // AUTO
    std::map<std::string, std::string> sources = {{"test.RqLazyBuild", kLazyBuildDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.RqLazyBuild", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    if (r == 701) {
        GTEST_SKIP() << "CUDA present but OptiX engine absent — lazy build cannot fire";
    }
    EXPECT_EQ(r, 705) << "fail code " << r
                      << " (1xx: wrong counts; 110: OptiX existed before launch (not "
                         "lazy); 705 expected = pre-launch software-only then OptiX built "
                         "on the first supported-shape launch)";
}

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
    "import cajeta.gpu.core.AccelerationStructure;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.RayQuery;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class NearRqAuto {\n"
    "    @Kernel\n"   // SUPPORTED triangle nearest-hit (NearestTri) -> lazy OptiX at launch
    "    public static void nearest(AccelerationStructure scene,\n"
    "                               Buffer<float32> outT, Buffer<uint32> outI) {\n"
    "        uint32 i = Thread.globalIdX();\n"
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
    "        Buffer<float32> outT = heap Buffer<float32>(1);\n"
    "        Buffer<uint32> outI = heap Buffer<uint32>(2);\n"
    "        outT.upload(ht); outI.upload(hi);\n"
    "        Stream s = Stream.current();\n"
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

TEST(ToffeeSpatialIndexDeviceTests, autoPrefersOptixForTriangleShape) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    unsetenv("CAJETA_GPU_AS_IMPL");   // AUTO — no opt-in
    std::map<std::string, std::string> sources = {{"test.NearRqAuto", kAutoTriDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.NearRqAuto", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    if (r == 701) {
        GTEST_SKIP() << "CUDA present but OptiX engine absent — lazy build cannot fire";
    }
    EXPECT_EQ(r, 705) << "fail code " << r
                      << " (1xx: wrong nearest-hit; 110: OptiX existed before launch; 705 "
                         "expected = AUTO lazily built the triangle OptiX rep and ran the "
                         "nearest-hit on RT cores with the correct result)";
}

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
TEST(ToffeeSpatialIndexDeviceTests, aabbCountRayQueryOnOptixDevice) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    AsImplEnvGuard forceOptix("optix");   // CUDA: optix -> the OptiX RT-core verb
    std::map<std::string, std::string> sources = {{"test.RqMin", kRqMinDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.RqMin", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (OptiX RT-core AABB ray query via the compiler != CPU)";
}

// M2 Phase 4 — the OptiX triangle nearest-hit verb through the full compiler. The
// SAME kNearestDriver as the software/CPU legs, with CAJETA_GPU_AS_IMPL=optix: the
// triangle AS builds on the OptiX tier, NvptxRegistration emits the nearest-hit
// program set (raygen with the baked ray + closesthit committing T/type/prim + miss),
// and the launch dispatches to cajeta_xpu_optix_launch_tri (built-in triangle
// traversal). Its 777 must equal the nearestHitOnNvptxSoftwareBvh / CPU legs.
TEST(ToffeeSpatialIndexDeviceTests, nearestHitRayQueryOnOptixDevice) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    AsImplEnvGuard forceOptix("optix");
    std::map<std::string, std::string> sources = {{"test.NearRq", kNearestDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.NearRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (OptiX RT-core triangle nearest-hit via the compiler != CPU)";
}

// M2 Phase 4-B — the OptiX triangle candidate-getter verb through the full compiler.
// The SAME kBaryDriver as the software/CPU legs, with CAJETA_GPU_AS_IMPL=optix: the
// triangle AS builds on the OptiX tier, NvptxRegistration emits the candidate-getter
// program set (raygen with the baked ray + anyhit reading the candidate's tmax +
// optixGetTriangleBarycentrics into out[0..2] then optixIgnoreIntersection + miss),
// and the launch dispatches to cajeta_xpu_optix_launch_tri (anyhit hitgroup). Its 777
// (distance=5, u=v=0.25) must equal the candidateGettersOnNvptxSoftwareBvh / CPU legs.
TEST(ToffeeSpatialIndexDeviceTests, candidateGettersRayQueryOnOptixDevice) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    AsImplEnvGuard forceOptix("optix");
    std::map<std::string, std::string> sources = {{"test.BaryRq", kBaryDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.BaryRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (OptiX RT-core candidate getters via the compiler != CPU)";
}

// M2 Phase 4-E — the OptiX committed-triangle per-launch verb (CommittedTri shape).
// The SAME kTriDriver / kFrontDriver as the software/CPU legs, with
// CAJETA_GPU_AS_IMPL=optix: a per-launch dynamic ray (ray components resolved from the
// initialize() args as const-or-buffer[i] loads), built-in triangle traversal,
// closesthit writes out[i] (hit-flag for the count kernel; front-face 1/2 for the
// front-face kernel). Each 777 must equal the *OnNvptxSoftwareBvh / CPU legs.
TEST(ToffeeSpatialIndexDeviceTests, triangleCountRayQueryOnOptixDevice) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    AsImplEnvGuard forceOptix("optix");
    std::map<std::string, std::string> sources = {{"test.TriRq", kTriDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.TriRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (OptiX RT-core triangle hit-count via the compiler != CPU)";
}

TEST(ToffeeSpatialIndexDeviceTests, frontFaceRayQueryOnOptixDevice) {
    if (!cajeta::xpu::nvidia::CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    AsImplEnvGuard forceOptix("optix");
    std::map<std::string, std::string> sources = {{"test.FrontRq", kFrontDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(sources, "test.FrontRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (OptiX RT-core front-face via the compiler != CPU; if it's "
                         "the back-hit code, OptiX front-face winding differs from cajeta's "
                         "det>0 convention — negate in emitOptixCommittedTriModule)";
}

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

TEST(ToffeeSpatialIndexDeviceTests, minimalRayQueryOnAmdSoftwareBvh) {
    if (!cajeta::xpu::amd::HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    std::map<std::string, std::string> sources = {{"test.RqMin", kRqMinDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(sources, "test.RqMin", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (AMD software AABB ray query: wrong hit count)";
}

TEST(ToffeeSpatialIndexDeviceTests, triangleRayQueryOnAmdSoftwareBvh) {
    if (!cajeta::xpu::amd::HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    std::map<std::string, std::string> sources = {{"test.TriRq", kTriDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(sources, "test.TriRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (AMD software triangle ray query != CPU)";
}

TEST(ToffeeSpatialIndexDeviceTests, nearestHitOnAmdSoftwareBvh) {
    if (!cajeta::xpu::amd::HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    std::map<std::string, std::string> sources = {{"test.NearRq", kNearestDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(sources, "test.NearRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (AMD nearest-hit confirm/committed != CPU)";
}

TEST(ToffeeSpatialIndexDeviceTests, candidateGettersOnAmdSoftwareBvh) {
    if (!cajeta::xpu::amd::HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    std::map<std::string, std::string> sources = {{"test.BaryRq", kBaryDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(sources, "test.BaryRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (AMD candidate distance/barycentrics != CPU)";
}

TEST(ToffeeSpatialIndexDeviceTests, frontFaceOnAmdSoftwareBvh) {
    if (!cajeta::xpu::amd::HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    std::map<std::string, std::string> sources = {{"test.FrontRq", kFrontDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(sources, "test.FrontRq", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (AMD committed front-face != CPU)";
}
