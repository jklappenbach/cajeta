// `heap KernelBuffer<T>(n, MemoryKind.X)` allocates n elements of that kind, as
// MemoryKind's own documentation shows (enum-overload-ranking Unit 2). Through
// v0.54.2 there was no such constructor: the call bound the package-private
// (int64 deviceHandle, uint64 elementCount) form, so the buffer wrapped address n
// with a count of the kind's ordinal, and a kernel touching it faulted with CUDA 700
// (found writing kernel-atomic-scope's pinned-flag test). Adding the constructor
// alone did not help until overload resolution ranked the exact enum first.

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// 777 when a buffer built with (n, kind) holds n elements the host writes with
// hostStore, a kernel increments, and the host reads back with hostLoad; 50 when
// length() is not n; 100+i when element i is wrong.
std::string kindCtorSource(const std::string& make) {
    return std::string(
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.MemoryKind;\n"
        "public class KindCtor {\n"
        "    @Kernel\n"
        "    public static void inc(KernelBuffer<int32> b, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) { b[i] = b[i] + 1; }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 64;\n"
        "        int32[] h = heap int32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { h[i] = (int32) i; }\n"
        "        ") + make + "\n"
        "        if (u.length() != (uint64) n) { return 50; }\n"
        "        u.hostStore(h);\n"
        "        KernelStream s #= KernelStream.current();\n"
        "        inc.launch(s, grid: [1], block: [64])(u, n);\n"
        "        s.sync();\n"
        "        int32[] out = heap int32[n];\n"
        "        u.hostLoad(out);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (out[i] != (int32)(i + 1)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
}

// The constructor under test, and the older idiom it must not capture.
std::string viaCtor(const char* kind) {
    return std::string("KernelBuffer<int32> u = heap KernelBuffer<int32>(n, ") + kind + ");";
}
std::string viaAllocate(const char* kind) {
    return std::string("KernelBuffer<int32> u = heap KernelBuffer<int32>(0, 64);\n"
                       "        u.allocate(") + kind + ");";
}

int runOn(cajeta::xpu::Backend be, const std::string& make) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    auto jit = CajetaJit::compile(kindCtorSource(make), "test.KindCtor", o);
    if (!jit) return -1;
    auto fn = jit->lookup<int (*)()>("run");
    return fn ? fn() : -2;
}

} // namespace

// On cpu every kind is host memory, so hostStore and hostLoad reach the kernel's
// data for each of them.
TEST(XpuKernelBufferKindCtor, everyKindAllocatesItsElementsOnCpu) {
    for (const char* k : {"MemoryKind.Device", "MemoryKind.Pinned", "MemoryKind.Unified"})
        EXPECT_EQ(777, runOn(cajeta::xpu::Backend::Cpu, viaCtor(k))) << k;
}

// `KernelBuffer(0, 64)` with two integer literals keeps meaning (handle 0, count
// 64): the kind parameter is typed MemoryKind, so an integer cannot bind it.
TEST(XpuKernelBufferKindCtor, theHandleAndCountFormStillBindsIntegerArguments) {
    for (const char* k : {"MemoryKind.Device", "MemoryKind.Pinned", "MemoryKind.Unified"})
        EXPECT_EQ(777, runOn(cajeta::xpu::Backend::Cpu, viaAllocate(k))) << k;
}

TEST(XpuKernelBufferKindCtorDeviceTests, pinnedAndUnifiedAllocateTheirKindOnCuda) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    for (const char* k : {"MemoryKind.Pinned", "MemoryKind.Unified"})
        EXPECT_EQ(777, runOn(cajeta::xpu::Backend::Nvptx, viaCtor(k))) << k;
}

TEST(XpuKernelBufferKindCtorDeviceTests, pinnedAndUnifiedAllocateTheirKindOnHip) {
    if (!cajeta::xpu::test::hipAvailable()) GTEST_SKIP() << "no HIP device";
    for (const char* k : {"MemoryKind.Pinned", "MemoryKind.Unified"})
        EXPECT_EQ(777, runOn(cajeta::xpu::Backend::Amdgpu, viaCtor(k))) << k;
}
