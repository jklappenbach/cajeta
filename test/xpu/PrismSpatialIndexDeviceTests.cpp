//
// Prism P1.0 — SpatialIndex exec test (cajeta-gpu Part C inc 3c).
//
// Prism is a consumer of the cajeta-gpu foundation; its first real code is the
// RT-as-compute SpatialIndex primitive (`cajeta-prism/src/prism/spatial/
// SpatialIndex.cajeta`). It has no standalone build/test harness yet, so we
// exec-verify it here, through the cajeta compiler's JIT: compile the authoritative
// SpatialIndex source (read from the sibling cajeta-prism repo) alongside a driver
// program via the multi-source overload, and run a fixed-radius neighbour count on
// a real ray-query device.
//
// Gated twice: on a ray-query-capable Vulkan device, and on the cajeta-prism
// source being present next to this checkout. Either missing -> SKIP.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using cajeta_test::CajetaJit;
using cajeta::xpu::vulkan::VulkanDriver;

namespace {

// The authoritative SpatialIndex.cajeta lives in the sibling cajeta-prism repo.
// Resolve it relative to CAJETA_SOURCE_ROOT (the cajeta checkout) — its parent is
// the cpp/ workspace dir, with cajeta-prism alongside. Returns "" if unreadable.
std::string readSpatialIndexSource() {
    const char* root = std::getenv("CAJETA_SOURCE_ROOT");
    if (!root || !*root) return "";
    std::string path = std::string(root) +
        "/../cajeta-prism/src/prism/spatial/SpatialIndex.cajeta";
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
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Stream;\n"
    "import prism.spatial.SpatialIndex;\n"
    "public class PrismRQ {\n"
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
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Stream;\n"
    "import prism.spatial.SpatialIndex;\n"
    "public class PrismExact {\n"
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

// The Prism SpatialIndex primitive, end to end on a real RT device: build a BVH
// over points, run a fixed-radius neighbour count through the public verb. Proves
// the foundation's ray-query path (3a/3b) is consumable as a library abstraction
// (3c) — the user writes `idx.countWithin(...)`, never a ray.
TEST(PrismSpatialIndexDeviceTests, fixedRadiusCountOnDevice) {
    if (!VulkanDriver::rayQueryAvailable()) {
        GTEST_SKIP() << "no Vulkan ray-query (acceleration-structure) device";
    }
    std::string lib = readSpatialIndexSource();
    if (lib.empty()) {
        GTEST_SKIP() << "cajeta-prism SpatialIndex.cajeta not found beside checkout";
    }
    std::map<std::string, std::string> sources = {
        {"prism.spatial.SpatialIndex", lib},
        {"test.PrismRQ", kDriver},
    };
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(sources, "test.PrismRQ", o);
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
TEST(PrismSpatialIndexDeviceTests, exactL2RefinementOnDevice) {
    if (!VulkanDriver::rayQueryAvailable()) {
        GTEST_SKIP() << "no Vulkan ray-query (acceleration-structure) device";
    }
    std::string lib = readSpatialIndexSource();
    if (lib.empty()) {
        GTEST_SKIP() << "cajeta-prism SpatialIndex.cajeta not found beside checkout";
    }
    std::map<std::string, std::string> sources = {
        {"prism.spatial.SpatialIndex", lib},
        {"test.PrismExact", kExactDriver},
    };
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(sources, "test.PrismExact", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 888) << "fail code " << r
                      << " (3xx: box approx != 3; 2xx: exact-L2 count != 1)";
}

// Minimal self-contained CPU ray-query exec (no SpatialIndex / cajeta-prism
// dependency): build an AccelerationStructure over 3 AABBs and run a RayQuery walk
// in a kernel on the CPU software path. Directly exercises the ray-query-to-core
// integration (software BVH builder + SoftwareRayQuery walk) without the broader
// stdlib closure. Same 1/0/1/1 expectation as the Prism fixed-radius scene.
const char* kRqMinDriver =
    "package test;\n"
    "import cajeta.xpu.core.AccelerationStructure;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.RayQuery;\n"
    "import cajeta.xpu.core.Stream;\n"
    "import cajeta.xpu.core.Thread;\n"
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

// Triangle ray query on the CPU software path (inc 2): build a triangle BVH and
// cast rays that hit / miss each triangle, counting triangle candidates
// (candidateType() == 0). Exercises the Möller-Trumbore leaf test end to end.
const char* kTriDriver =
    "package test;\n"
    "import cajeta.xpu.core.AccelerationStructure;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.RayQuery;\n"
    "import cajeta.xpu.core.Stream;\n"
    "import cajeta.xpu.core.Thread;\n"
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
    "            uint32 c = 0;\n"
    "            while (rq.proceed()) {\n"
    "                if (rq.candidateType() == 0) { c = c + 1; }\n"  // triangle hit
    "            }\n"
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
    "import cajeta.xpu.core.AccelerationStructure;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.RayQuery;\n"
    "import cajeta.xpu.core.Stream;\n"
    "import cajeta.xpu.core.Thread;\n"
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

TEST(PrismSpatialIndexDeviceTests, candidateGettersOnCpuSoftwareBvh) {
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

TEST(PrismSpatialIndexDeviceTests, triangleRayQueryOnCpuSoftwareBvh) {
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
TEST(PrismSpatialIndexDeviceTests, triangleRayQueryOnDevice) {
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

TEST(PrismSpatialIndexDeviceTests, minimalRayQueryOnCpuSoftwareBvh) {
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

// ── Ray-query-to-core (inc 1): the SAME Prism source on the CPU SOFTWARE path ──
// No ray-query-capable device required — the AccelerationStructure builds a
// portable software BVH (runtime/native/cajeta_bvh.c) and RayQuery lowers to the
// cajeta SoftwareRayQuery walk. The results must match the Vulkan native path
// above (777 / 888): that agreement is what makes inline ray query genuinely core.
// Each ctest case is a fresh process, so the CPU-only bundle selects the CPU
// backend (priority CUDA>HIP>Vulkan>CPU is moot when only CPU is bundled).

TEST(PrismSpatialIndexDeviceTests, fixedRadiusCountOnCpuSoftwareBvh) {
    std::string lib = readSpatialIndexSource();
    if (lib.empty()) {
        GTEST_SKIP() << "cajeta-prism SpatialIndex.cajeta not found beside checkout";
    }
    std::map<std::string, std::string> sources = {
        {"prism.spatial.SpatialIndex", lib},
        {"test.PrismRQ", kDriver},
    };
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(sources, "test.PrismRQ", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (CPU software BVH/RayQuery: wrong neighbour count)";
}

TEST(PrismSpatialIndexDeviceTests, exactL2RefinementOnCpuSoftwareBvh) {
    std::string lib = readSpatialIndexSource();
    if (lib.empty()) {
        GTEST_SKIP() << "cajeta-prism SpatialIndex.cajeta not found beside checkout";
    }
    std::map<std::string, std::string> sources = {
        {"prism.spatial.SpatialIndex", lib},
        {"test.PrismExact", kExactDriver},
    };
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(sources, "test.PrismExact", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 888) << "fail code " << r
                      << " (CPU software: 3xx box approx != 3; 2xx exact-L2 != 1)";
}
