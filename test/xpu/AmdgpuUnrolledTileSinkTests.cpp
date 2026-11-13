//
// The unrolled 128 GEMM tile on gfx1151: MachineSink moved its FMAs past the loop barrier and it spilled (cajeta-ml-gpu 28.2.8).
//

#include "gtest/gtest.h"

#include "KernelLoweringProbe.h"

#include "llvm/Support/CommandLine.h"

#include <sstream>
#include <string>

using namespace cajeta::xpu::probe;

namespace {

// The unrolled tile's wide path: 8x8 accumulators a thread, 16 K steps at constant shared
// offsets, then a barrier and the staging of the prefetched slab under `if (more)`.
std::string unrolledTileSource() {
    std::ostringstream s;
    s << "package test;\n"
         "import cajeta.xpu.Barrier;\nimport cajeta.xpu.KernelBuffer;\nimport cajeta.xpu.KernelThread;\n"
         "import cajeta.xpu.Shared;\nimport cajeta.xpu.Workgroup;\nimport cajeta.lang.Math;\n"
         "public class M {\n"
         "    @Kernel\n    @Occupancy(maxThreads = 256, minResident = 2)\n"
         "    public static void tile(KernelBuffer<float32> c, KernelBuffer<float32> a, KernelBuffer<float32> b, uint32 n, uint32 k) {\n"
         "        Shared<float32> sa = shared float32[2112];\n        Shared<float32> sb = shared float32[2112];\n"
         "        uint32 t = KernelThread.x();\n        uint32 tx = t % 16;\n        uint32 ty = t / 16;\n"
         "        uint32 ntiles = n / 128;\n        uint32 wid = Workgroup.x();\n        uint32 mi = wid / ntiles;\n"
         "        uint32 m0 = mi * 128;\n        uint32 n0 = (wid - mi * ntiles) * 128;\n"
         "        uint32 ar0 = t / 4;\n        uint32 ac0 = (t - ar0 * 4) * 4;\n        uint32 ar1 = ar0 + 64;\n";
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j) s << "        float32 c" << i << j << " = 0.0f;\n";
    const char* lanes = "xyzw";
    auto stage = [&](const char* ind) {
        for (int v = 0; v < 4; ++v) {
            const char* arr = v < 2 ? "sa" : "sb";
            const char* src = v == 0 ? "pa0" : v == 1 ? "pa1" : v == 2 ? "pb0" : "pb1";
            const char* row = (v % 2) ? "ar1" : "ar0";
            for (int l = 0; l < 4; ++l)
                s << ind << arr << "[(ac0 + " << l << ") * 132 + " << row << "] = " << src << "." << lanes[l] << ";\n";
        }
    };
    s << "        Vector<float32,4> pa0 = a.vload<4>((m0 + ar0) * k + ac0, align: 16);\n"
         "        Vector<float32,4> pa1 = a.vload<4>((m0 + ar1) * k + ac0, align: 16);\n"
         "        Vector<float32,4> pb0 = b.vload<4>((n0 + ar0) * k + ac0, align: 16);\n"
         "        Vector<float32,4> pb1 = b.vload<4>((n0 + ar1) * k + ac0, align: 16);\n";
    stage("        ");
    s << "        Barrier.workgroup();\n"
         "        Vector<float32,4> av0;\n        Vector<float32,4> av1;\n        Vector<float32,4> bv0;\n        Vector<float32,4> bv1;\n"
         "        uint32 k0 = 0;\n"
         "        while (k0 < k) {\n"
         "            uint32 next = k0 + 16;\n            boolean more = next < k;\n"
         "            if (more) {\n"
         "                pa0 = a.vload<4>((m0 + ar0) * k + next + ac0, align: 16);\n"
         "                pa1 = a.vload<4>((m0 + ar1) * k + next + ac0, align: 16);\n"
         "                pb0 = b.vload<4>((n0 + ar0) * k + next + ac0, align: 16);\n"
         "                pb1 = b.vload<4>((n0 + ar1) * k + next + ac0, align: 16);\n"
         "            }\n";
    for (int step = 0; step < 16; ++step) {
        s << "            av0 = sa.vload<4>(" << step * 132 << " + ty * 4);\n"
          << "            av1 = sa.vload<4>(" << step * 132 + 64 << " + ty * 4);\n"
          << "            bv0 = sb.vload<4>(" << step * 132 << " + tx * 4);\n"
          << "            bv1 = sb.vload<4>(" << step * 132 + 64 << " + tx * 4);\n";
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 8; ++j)
                s << "            c" << i << j << " = Math.fma(" << (i < 4 ? "av0." : "av1.") << lanes[i % 4] << ", "
                  << (j < 4 ? "bv0." : "bv1.") << lanes[j % 4] << ", c" << i << j << ");\n";
    }
    s << "            Barrier.workgroup();\n            if (more) {\n";
    stage("                ");
    s << "                Barrier.workgroup();\n            }\n            k0 = next;\n        }\n";
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j)
            s << "        c[(m0 + " << (i < 4 ? 0 : 64) << " + ty * 4 + " << i % 4 << ") * n + n0 + "
              << (j < 4 ? 0 : 64) << " + tx * 4 + " << j % 4 << "] = c" << i << j << ";\n";
    s << "    }\n}\n";
    return s.str();
}

// The integer after `key` in the ISA's metadata, or -1 when it is absent.
long isaNumber(const std::string& isa, const std::string& key) {
    auto at = isa.find(key);
    if (at == std::string::npos) return -1;
    return std::strtol(isa.c_str() + at + key.size(), nullptr, 10);
}

bool machineSinkDisabled() {
    auto& opts = llvm::cl::getRegisteredOptions();
    auto it = opts.find("disable-machine-sink");
    return it != opts.end() && static_cast<llvm::cl::opt<bool>*>(it->second)->getValue();
}

} // namespace

// The tile keeps its FMAs ahead of the barrier, so it fits gfx1151's registers without scratch.
TEST(AmdgpuUnrolledTileSinkTests, theUnrolledTileFitsWithoutSpilling) {
    auto low = lowerForAmdgpu(unrolledTileSource(), "tile", "test.M", "gfx1151", "unrolled_sink");
    ASSERT_TRUE(low.ok) << low.why;
    EXPECT_EQ(isaNumber(low.ptx, ".vgpr_spill_count:"), 0);
    EXPECT_EQ(isaNumber(low.ptx, ".private_segment_fixed_size:"), 0);
    EXPECT_GT(isaNumber(low.ptx, ".vgpr_count:"), 0);
}

// The switch is process-global and only amdgpu codegen sets it, so the host and other backends keep sinking.
TEST(AmdgpuUnrolledTileSinkTests, theSinkSwitchIsRestoredAfterAmdgpuCodegen) {
    ASSERT_FALSE(machineSinkDisabled());
    auto low = lowerForAmdgpu(unrolledTileSource(), "tile", "test.M", "gfx1151", "unrolled_sink_restore");
    ASSERT_TRUE(low.ok) << low.why;
    EXPECT_FALSE(machineSinkDisabled());
}
