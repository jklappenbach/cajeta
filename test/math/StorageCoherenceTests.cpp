//
// StorageCoherenceTests — cajeta-llama plan Unit 3 (3.1.1–3.1.5): storage
// coherence and host-mirror release (spec §2.3, §2.4, §4.10).
//
// The failure this unit forecloses: an op (or a caller) reading `Storage`'s
// host mirror while the live data sits on the device — stale bytes returned
// quietly — and the memory cost of keeping both mirrors alive for the life of
// every device-resident tensor (a device-resident model costing full host RAM).
//
// Device placement uses the portable CPU XPU backend in-process, the same
// discipline as PlacementDispatchTests: `gpu()` allocates a real KernelBuffer
// and uploads, so mirror-release and re-materialization are exercised for
// real, just without silicon.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

#ifdef __linux__
#include <fstream>
#endif

using cajeta_test::CajetaJit;

namespace {

int32_t runI32Xpu(const std::string& src) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(src, "test.D", o);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

#ifdef __linux__
// Resident set size in KiB, from /proc/self/status. -1 if unreadable.
long rssKb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            return std::atol(line.c_str() + 6);
        }
    }
    return -1;
}
#endif

const char* PRE =
    "package test;\n"
    "import cajeta.math.Tensor;\n";

} // namespace

// 3.1.1 — reading a device-resident tensor through a host accessor faults with
// a diagnostic instead of returning stale bytes (spec 2.3). Writes are guarded
// too: a host store to a device-resident tensor would be silently overwritten
// by the next download — the same quiet corruption in the other direction.
//
// RED until 3.2.1: Storage.get/set read `host` unconditionally today
// (Storage.cajeta:41,46).
TEST(StorageCoherenceTests, deviceResidentHostAccessFaults) {
    std::string src = std::string(PRE) +
        "import cajeta.math.PlacementException;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] d = [ 3.0f, 4.0f, 5.0f, 6.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 4;\n"
        "        Tensor<float32> t #= Tensor.of<float32>(d, s);\n"
        "        t.gpu();\n"
        "        boolean threwGet = false;\n"
        "        try {\n"
        "            float32 v = t.get1(0);\n"
        "        } catch (PlacementException ex) {\n"
        "            threwGet = true;\n"
        "        }\n"
        "        if (!threwGet) { return -1; }\n"
        "        boolean threwFlat = false;\n"
        "        try {\n"
        "            float32 v2 = t.flatGet(2);\n"
        "        } catch (PlacementException ex) {\n"
        "            threwFlat = true;\n"
        "        }\n"
        "        if (!threwFlat) { return -2; }\n"
        "        boolean threwAt = false;\n"
        "        int64[] idx = heap int64[1]; idx[0] = 1;\n"
        "        try {\n"
        "            float32 v3 = t.getAt(idx);\n"
        "        } catch (PlacementException ex) {\n"
        "            threwAt = true;\n"
        "        }\n"
        "        if (!threwAt) { return -3; }\n"
        "        boolean threwSet = false;\n"
        "        try {\n"
        "            t.set1(0, 9.0f);\n"
        "        } catch (PlacementException ex) {\n"
        "            threwSet = true;\n"
        "        }\n"
        "        if (!threwSet) { return -4; }\n"
        // After cpu(), host access works again and the data is intact.
        "        t.cpu();\n"
        "        if (t.get1(0) != 3.0f) { return -5; }\n"
        "        if (t.get1(3) != 6.0f) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 3.1.2 — moving a tensor to the device releases the host mirror and
// measurably lowers host RSS (spec 2.4). The tensor is 64 MiB (16M f32), well
// past the glibc mmap threshold, so the free is a real munmap and RSS
// accounting sees it.
//
// Delta discipline: on the CPU XPU backend the "device" buffer is itself host
// memory, so the move allocates 64 MiB (device) and — once 3.2.2 lands —
// frees 64 MiB (host mirror): net ≈ 0. Today nothing is freed, so the move
// costs a net +64 MiB. The 32 MiB threshold cleanly discriminates.
//
// RED until 3.2.2.
TEST(StorageCoherenceTests, deviceMoveReleasesHostMirror) {
#ifndef __linux__
    GTEST_SKIP() << "RSS probe reads /proc/self/status (Linux only)";
#else
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    static Tensor<float32> t;\n"
        "    public static int32 alloc() {\n"
        "        int64[] s = heap int64[1]; s[0] = 16777216;\n"   // 64 MiB of f32
        "        t #= Tensor.ones<float32>(s);\n"
        "        if (t.get1(12345) != 1.0f) { return -1; }\n"
        "        return 1;\n"
        "    }\n"
        "    public static int32 move() {\n"
        "        t.gpu();\n"
        "        if (!t.isOnGpu()) { return -1; }\n"
        "        return 1;\n"
        "    }\n"
        "    public static int32 back() {\n"
        "        t.cpu();\n"
        "        if (t.get1(12345) != 1.0f) { return -1; }\n"     // survived the round trip
        "        if (t.get1(16777215) != 1.0f) { return -2; }\n"  // last element too
        "        return 1;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(src, "test.D", o);
    auto alloc = jit->lookup<int32_t (*)()>("alloc");
    auto move = jit->lookup<int32_t (*)()>("move");
    auto back = jit->lookup<int32_t (*)()>("back");

    ASSERT_EQ(alloc(), 1);
    long afterAlloc = rssKb();
    ASSERT_GT(afterAlloc, 0);

    ASSERT_EQ(move(), 1);
    long afterMove = rssKb();

    // Device copy (+64 MiB) minus released host mirror (−64 MiB) ≈ 0. Without
    // the release the delta is ≈ +64 MiB.
    EXPECT_LT(afterMove - afterAlloc, 32 * 1024)
        << "moving to the device kept the host mirror resident";

    // The data must survive release + re-materialization.
    EXPECT_EQ(back(), 1);
#endif
}

// 3.1.3 — toHost() after device mutation returns the mutated data (spec 2.3's
// flip side: the download must re-materialize a RELEASED mirror, not just
// overwrite a kept one). The mutation is a real kernel: both operands
// device-resident, the elementwise add runs on the XPU path and its result is
// device-resident (spec 2.1, pinned by PlacementDispatchTests); cpu() must
// then reallocate the host buffer lazily and download into it.
//
// Green before 3.2.2 (download into the kept mirror), and MUST stay green
// after it (download into a lazily reallocated one) — the regression guard for
// the release.
TEST(StorageCoherenceTests, toHostAfterDeviceMutationReturnsMutatedData) {
    std::string src = std::string(PRE) +
        "import cajeta.math.Ewise;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] da = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
        "        float32[] db = [ 10.0f, 20.0f, 30.0f, 40.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 4;\n"
        "        Tensor<float32> a #= Tensor.of<float32>(da, s);\n"
        "        Tensor<float32> b #= Tensor.of<float32>(db, s);\n"
        "        a.gpu();\n"
        "        b.gpu();\n"
        "        Tensor<float32> c #= Ewise.arithF32Op(a, b, 0);\n"  // add, on device
        "        if (!c.isOnGpu()) { return -1; }\n"
        "        c.cpu();\n"
        "        if (c.get1(0) != 11.0f) { return -2; }\n"
        "        if (c.get1(3) != 44.0f) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 3.1.4 — a view and its base share residency: moving either moves both. A
// view shares its base's Storage instance, so this holds structurally — the
// test pins it so a residency redesign cannot quietly give views their own
// placement state.
TEST(StorageCoherenceTests, viewAndBaseShareResidency) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] d = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 4;\n"
        "        Tensor<float32> t #= Tensor.of<float32>(d, s);\n"
        "        Tensor<float32> v #= t.alias();\n"
        "        if (!v.isView()) { return -1; }\n"
        "        v.gpu();\n"                                  // move the VIEW
        "        if (!t.isOnGpu()) { return -2; }\n"          // base moved with it
        "        t.cpu();\n"                                  // move the BASE back
        "        if (v.isOnGpu()) { return -3; }\n"           // view came back too
        "        if (v.get1(2) != 3.0f) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 3.1.5 — a borrowed Storage slice aliases its parent's elements with no copy,
// and dropping the slice does not free the parent's buffer (spec 4.10; the
// host-side mirror of KernelBuffer.slice's owned=false contract). The debug
// allocator poisons freed memory with 0xDB, so if the slice's drop freed the
// shared buffer, the parent's post-drop reads return poison, not data.
//
// RED until 3.2.4: Storage has exactly one constructor today and it always
// allocates (Storage.cajeta:29-34), so the borrowing form does not compile.
TEST(StorageCoherenceTests, borrowedStorageSliceAliasesWithoutCopy) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Storage;\n"
        "public final class D {\n"
        "    static int32 probeSlice(Storage<float32> p) {\n"
        "        Storage<float32> s = heap Storage<float32>(p, 50, 25);\n"
        "        if (s.size() != 25) { return -11; }\n"
        "        if (s.get(0) != 50.0f) { return -12; }\n"     // aliases parent[50]
        "        if (s.get(24) != 74.0f) { return -13; }\n"    // aliases parent[74]
        "        s.set(1, 999.0f);\n"                          // write-through
        "        return 1;\n"
        "    }\n"                       // slice drops here; parent must survive
        "    public static int32 run() {\n"
        "        Storage<float32> p = heap Storage<float32>(100);\n"
        "        int64 i = 0;\n"
        "        while (i < 100) {\n"
        "            p.set(i, (float32) i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        int32 r = probeSlice(p);\n"
        "        if (r != 1) { return r; }\n"
        "        if (p.get(51) != 999.0f) { return -1; }\n"    // slice write visible
        "        if (p.get(50) != 50.0f) { return -2; }\n"
        "        if (p.get(99) != 99.0f) { return -3; }\n"     // buffer intact post-drop
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
