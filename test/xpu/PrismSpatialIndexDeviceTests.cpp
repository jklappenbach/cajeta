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
    "        float32[] pts = new float32[np * 3];\n"
    "        pts[0] = 0.0f;  pts[1] = 0.0f; pts[2] = 0.0f;\n"
    "        pts[3] = 10.0f; pts[4] = 0.0f; pts[5] = 0.0f;\n"
    "        pts[6] = 20.0f; pts[7] = 0.0f; pts[8] = 0.0f;\n"
    "        SpatialIndex idx = heap SpatialIndex(pts, np, 0.5f);\n"
    "        uint32 n = 4;\n"
    "        float32[] hqx = new float32[n];\n"
    "        float32[] hqy = new float32[n];\n"
    "        float32[] hqz = new float32[n];\n"
    "        uint32[] hout = new uint32[n];\n"
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
