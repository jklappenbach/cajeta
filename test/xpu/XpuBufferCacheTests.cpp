//
// XpuBufferCacheTests — cajeta-ml-gpu 28.2.2: a caching device allocator under
// KernelBuffer on the pointer GPU backends (hip, cuda).
//
// Every ml device op that returned a fresh 16 MB tensor landed at 740-830 us on
// proton whatever its arithmetic (2026-11-08), against 83-234 us for torch: the
// floor was hipMalloc + hipFree per output. A freed device-kind block is now kept
// in a per-process cache and handed to the next allocation of its size class;
// pinned and unified kinds, and the cpu and vulkan backends, still go to the
// backend every time. `Device.deviceAllocationCount` keeps counting logical
// allocations, so "this op allocates nothing" assertions keep their meaning;
// `Device.deviceRawAllocationCount` counts the backend's.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* PRE =
    "package test;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.MemoryKind;\n"
    "public class C {\n"
    "    @Kernel\n"
    "    public static void fill(KernelBuffer<float32> x, float32 v) {\n"
    "        uint32 t = KernelThread.globalIdX();\n"
    "        x[t] = v;\n"
    "    }\n";

int32_t runOnHip(const std::string& body) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    std::string src = std::string(PRE) +
        "    public static int32 run() {\n"
        "        Device.setDeviceCache(true);\n"
        "        Device.deviceCacheFlush();\n" + body +
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.C", o);
    if (!jit) return -1;
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn ? fn() : -2;
}

} // namespace

// A freed 16 MB block serves the next 16 MB allocation: two logical allocations,
// one backend allocation, and the second handle is the first's.
TEST(XpuBufferCache, aFreedBlockServesTheNextAllocationOfItsSize) {
    CAJETA_SKIP_IF_NO_HIP();
    EXPECT_EQ(runOnHip(
        "        int64 logical0 = Device.deviceAllocationCount();\n"
        "        int64 raw0 = Device.deviceRawAllocationCount();\n"
        "        KernelBuffer<float32> a = heap KernelBuffer<float32>((uint64) 4194304);\n"
        "        int64 ha = a.handle();\n"
        "        a.free();\n"
        "        KernelBuffer<float32> b = heap KernelBuffer<float32>((uint64) 4194304);\n"
        "        if (b.handle() != ha) { return 1; }\n"
        "        if (Device.deviceAllocationCount() - logical0 != 2) { return 2; }\n"
        "        if (Device.deviceRawAllocationCount() - raw0 != 1) { return 3; }\n"
        "        b.free();\n"
        "        return 0;\n"), 0);
}

// A reused block holds the new kernel's results, not the old ones: fill A with
// 1, free it, fill its successor B with 2, and read back only 2s.
TEST(XpuBufferCache, aReusedBlockHoldsTheNewKernelsResults) {
    CAJETA_SKIP_IF_NO_HIP();
    EXPECT_EQ(runOnHip(
        "        KernelStream s #= KernelStream.current();\n"
        "        KernelBuffer<float32> a = heap KernelBuffer<float32>((uint64) 65536);\n"
        "        fill.launch(s, grid: [256], block: [256])(a, 1.0f);\n"
        "        s.sync();\n"
        "        a.free();\n"
        "        KernelBuffer<float32> b = heap KernelBuffer<float32>((uint64) 65536);\n"
        "        fill.launch(s, grid: [256], block: [256])(b, 2.0f);\n"
        "        s.sync();\n"
        "        float32[] out = heap float32[65536];\n"
        "        b.download(out);\n"
        "        int32 i = 0;\n"
        "        while (i < 65536) {\n"
        "            if (out[i] != 2.0f) { return 10; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        b.free();\n"
        "        return 0;\n"), 0);
}

// Sizes round to classes (512 B below 1 MB, 2 MB multiples above): a request a
// little larger than a freed block of the same class is a hit; a request in
// another class is a miss.
TEST(XpuBufferCache, requestsRoundToSizeClasses) {
    CAJETA_SKIP_IF_NO_HIP();
    EXPECT_EQ(runOnHip(
        "        int64 raw0 = Device.deviceRawAllocationCount();\n"
        "        KernelBuffer<float32> a = heap KernelBuffer<float32>((uint64) 300000);\n"   // 1.2 MB -> 2 MB class
        "        a.free();\n"
        "        KernelBuffer<float32> b = heap KernelBuffer<float32>((uint64) 400000);\n"   // 1.6 MB -> same class
        "        if (Device.deviceRawAllocationCount() - raw0 != 1) { return 1; }\n"
        "        b.free();\n"
        "        KernelBuffer<float32> c = heap KernelBuffer<float32>((uint64) 600000);\n"   // 2.4 MB -> 4 MB class
        "        if (Device.deviceRawAllocationCount() - raw0 != 2) { return 2; }\n"
        "        c.free();\n"
        "        KernelBuffer<float32> d = heap KernelBuffer<float32>((uint64) 100);\n"       // 400 B -> 512 B class
        "        d.free();\n"
        "        KernelBuffer<float32> e = heap KernelBuffer<float32>((uint64) 128);\n"       // 512 B -> same class
        "        if (Device.deviceRawAllocationCount() - raw0 != 3) { return 3; }\n"
        "        e.free();\n"
        "        KernelBuffer<float32> f = heap KernelBuffer<float32>((uint64) 129);\n"       // 516 B -> 1024 B class
        "        if (Device.deviceRawAllocationCount() - raw0 != 4) { return 4; }\n"
        "        f.free();\n"
        "        return 0;\n"), 0);
}

// The cache reports what it holds, a flush returns it all to the backend, and
// the next allocation after a flush is a backend allocation again.
TEST(XpuBufferCache, flushReturnsCachedBlocksToTheBackend) {
    CAJETA_SKIP_IF_NO_HIP();
    EXPECT_EQ(runOnHip(
        "        KernelBuffer<float32> a = heap KernelBuffer<float32>((uint64) 4194304);\n"
        "        if (Device.deviceCacheBytes() != 0) { return 1; }\n"
        "        a.free();\n"
        "        if (Device.deviceCacheBytes() != 16777216) { return 2; }\n"
        "        Device.deviceCacheFlush();\n"
        "        if (Device.deviceCacheBytes() != 0) { return 3; }\n"
        "        int64 raw0 = Device.deviceRawAllocationCount();\n"
        "        KernelBuffer<float32> b = heap KernelBuffer<float32>((uint64) 4194304);\n"
        "        if (Device.deviceRawAllocationCount() - raw0 != 1) { return 4; }\n"
        "        b.free();\n"
        "        return 0;\n"), 0);
}

// With the cache off every allocation reaches the backend, and a free returns
// the block to it.
TEST(XpuBufferCache, theCacheCanBeTurnedOff) {
    CAJETA_SKIP_IF_NO_HIP();
    EXPECT_EQ(runOnHip(
        "        Device.setDeviceCache(false);\n"
        "        int64 raw0 = Device.deviceRawAllocationCount();\n"
        "        KernelBuffer<float32> a = heap KernelBuffer<float32>((uint64) 4194304);\n"
        "        a.free();\n"
        "        if (Device.deviceCacheBytes() != 0) { return 1; }\n"
        "        KernelBuffer<float32> b = heap KernelBuffer<float32>((uint64) 4194304);\n"
        "        if (Device.deviceRawAllocationCount() - raw0 != 2) { return 2; }\n"
        "        b.free();\n"
        "        Device.setDeviceCache(true);\n"
        "        return 0;\n"), 0);
}

// The cache holds at most its limit: a block freed over the limit goes back to
// the backend instead.
TEST(XpuBufferCache, theCacheKeepsNoMoreThanItsLimit) {
    CAJETA_SKIP_IF_NO_HIP();
    EXPECT_EQ(runOnHip(
        "        Device.setDeviceCacheLimit((int64) 20000000);\n"
        "        KernelBuffer<float32> a = heap KernelBuffer<float32>((uint64) 4194304);\n"
        "        KernelBuffer<float32> b = heap KernelBuffer<float32>((uint64) 4194304);\n"
        "        a.free();\n"
        "        b.free();\n"
        "        if (Device.deviceCacheBytes() != 16777216) { return 1; }\n"
        "        Device.setDeviceCacheLimit((int64) -1);\n"
        "        Device.deviceCacheFlush();\n"
        "        return 0;\n"), 0);
}

// Pinned and unified blocks are never cached: their frees reach the backend.
TEST(XpuBufferCache, pinnedAndUnifiedKindsBypassTheCache) {
    CAJETA_SKIP_IF_NO_HIP();
    EXPECT_EQ(runOnHip(
        "        int64 raw0 = Device.deviceRawAllocationCount();\n"
        "        KernelBuffer<float32> p = heap KernelBuffer<float32>(0, (uint64) 262144);\n"
        "        p.allocate(MemoryKind.Pinned);\n"
        "        p.free();\n"
        "        KernelBuffer<float32> q = heap KernelBuffer<float32>(0, (uint64) 262144);\n"
        "        q.allocate(MemoryKind.Pinned);\n"
        "        q.free();\n"
        "        if (Device.deviceRawAllocationCount() - raw0 != 2) { return 1; }\n"
        "        if (Device.deviceCacheBytes() != 0) { return 2; }\n"
        "        return 0;\n"), 0);
}
