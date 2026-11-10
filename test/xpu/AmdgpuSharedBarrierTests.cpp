//
// AmdgpuSharedBarrierTests — Barrier.shared on amdgpu: the rendezvous whose fences are
// restricted to the local address space, so a global load issued before it stays in
// flight across it (cajeta-ml-gpu 23.2.2). Compile-level: ISA emission needs no device.
//

#include <gtest/gtest.h>
#include "KernelLoweringProbe.h"

#include <string>

using cajeta::xpu::probe::countOf;
using cajeta::xpu::probe::lowerForAmdgpu;

namespace {

// A register prefetch across a barrier: the global load's value is consumed only after it.
std::string prefetchKernel(const char* barrier) {
    return std::string() +
        "package test;\n"
        "import cajeta.xpu.Barrier;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Shared;\n"
        "public final class M {\n"
        "    @Kernel\n"
        "    public static void k(KernelBuffer<float32> out, KernelBuffer<float32> x, uint32 n) {\n"
        "        Shared<float32> tile = shared float32[256];\n"
        "        uint32 t = KernelThread.x();\n"
        "        float32 cur = x[t];\n"
        "        tile[t] = cur;\n"
        "        Barrier." + barrier + "();\n"
        "        float32 acc = tile[(t + 1) % 256];\n"
        "        uint32 i = 256;\n"
        "        while (i < n) {\n"
        "            float32 next = x[i + t];\n"           // the prefetch, in flight across the barrier
        "            acc = acc + tile[(t + 7) % 256];\n"
        "            Barrier." + barrier + "();\n"
        "            tile[t] = next;\n"
        "            Barrier." + barrier + "();\n"
        "            i = i + 256;\n"
        "        }\n"
        "        out[t] = acc;\n"
        "    }\n"
        "}\n";
}

// True when EVERY global load is followed by a vmcnt(0) drain before the next s_barrier;
// false when some load is still in flight at the barrier after it.
bool everyLoadDrainsBeforeBarrier(const std::string& isa) {
    std::size_t loads = 0;
    for (std::size_t load = isa.find("global_load"); load != std::string::npos;
         load = isa.find("global_load", load + 1)) {
        std::size_t bar = isa.find("s_barrier", load);
        if (bar == std::string::npos) break;
        ++loads;
        if (isa.substr(load, bar - load).find("vmcnt(0)") == std::string::npos) return false;
    }
    EXPECT_GE(loads, 2u) << isa;
    return true;
}

TEST(AmdgpuSharedBarrierTests, workgroupBarrierDrainsGlobalLoads) {
    auto L = lowerForAmdgpu(prefetchKernel("workgroup"), "k");
    ASSERT_TRUE(L.ok) << L.why;
    EXPECT_TRUE(everyLoadDrainsBeforeBarrier(L.ptx)) << L.ptx;
    EXPECT_EQ(countOf(L.ir, "!mmra"), 0u);
}

TEST(AmdgpuSharedBarrierTests, sharedBarrierKeepsGlobalLoadsInFlight) {
    auto L = lowerForAmdgpu(prefetchKernel("shared"), "k");
    ASSERT_TRUE(L.ok) << L.why;
    EXPECT_FALSE(everyLoadDrainsBeforeBarrier(L.ptx)) << L.ptx;
    EXPECT_GE(countOf(L.ir, "!mmra"), 6u) << L.ir;
    EXPECT_EQ(countOf(L.ir, "amdgpu-synchronize-as"), 1u);
    EXPECT_GE(countOf(L.ptx, "s_barrier"), 3u);
}

} // namespace
