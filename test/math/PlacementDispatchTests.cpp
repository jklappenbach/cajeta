//
// PlacementDispatchTests — cajeta-llama plan Unit 1 (1.1.1–1.1.6): placement
// dispatch across the Tensor op surface.
//
// Today `Ewise.cajeta` carries the dispatch shape in 14 hand-copied
// `isOnGpu()` guards and is reachable from exactly two files (itself and
// Tensor.cajeta) — spec 2.1. These tests state the contract the surface must
// meet once that shape is extracted and the elementwise, reduction and matmul
// families are routed through it.
//
// Written test-first: 1.1.3, 1.1.4 and 1.1.6 are expected RED until Unit 1.2
// lands. Each is red for a specific missing behaviour, noted per test, not for
// a harness or syntax problem.
//
// Device tests use the portable CPU XPU backend in-process, the same discipline
// as TensorTests' Phase 6 cases: the seam routes on placement, and the kernel
// goes through the real launch FFI either way. On-device Vulkan/CUDA validation
// rides separate device gates.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32Xpu(const std::string& src) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(src, "test.D", o);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* PRE =
    "package test;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.math.Ewise;\n";

} // namespace

// 1.1.1 — every op with a device path agrees with the host path. Integer ops are
// bitwise; float ops are within tolerance, because the device path reduces in a
// different order than the host triple-loop. Covers one op per family routed in
// 1.2.2 (elementwise, reduction, matmul); the sweep widens as ops are routed.
TEST(PlacementDispatchTests, deviceAgreesWithHostPerFamily) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] da = [ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f ];\n"
        "        float32[] db = [ 10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f, 70.0f, 80.0f ];\n"
        "        int64[] shp = heap int64[1];\n"
        "        shp[0] = 8;\n"
        // elementwise, host
        "        Tensor<float32> a = Tensor.of<float32>(da, shp);\n"
        "        Tensor<float32> b = Tensor.of<float32>(db, shp);\n"
        "        Tensor<float32> addHost = Ewise.arithF32Op(a, b, 0);\n"
        "        float32 sumHost = Ewise.sumF32(a);\n"
        "        float32 maxHost = Ewise.maxF32(a);\n"
        // same three on device
        "        Tensor<float32> a2 = Tensor.of<float32>(da, shp);\n"
        "        Tensor<float32> b2 = Tensor.of<float32>(db, shp);\n"
        "        a2.gpu();\n"
        "        b2.gpu();\n"
        "        Tensor<float32> addDev = Ewise.arithF32Op(a2, b2, 0);\n"
        "        float32 sumDev = Ewise.sumF32(a2);\n"
        "        float32 maxDev = Ewise.maxF32(a2);\n"
        "        addDev.cpu();\n"
        "        int64 i = 0;\n"
        "        while (i < 8) {\n"
        "            float32 d = addHost.get1(i) - addDev.get1(i);\n"
        "            if (d < 0.0f) { d = 0.0f - d; }\n"
        "            if (d > 0.0001f) { return -1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        float32 ds = sumHost - sumDev;\n"
        "        if (ds < 0.0f) { ds = 0.0f - ds; }\n"
        "        if (ds > 0.001f) { return -2; }\n"
        "        if (maxHost != maxDev) { return -3; }\n"   // max is exact, not a reduction sum
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 1.1.2 — an op whose operands are all device-resident returns a device-resident
// result and performs no download. Asserted by reading device() on the RESULT
// before any explicit cpu(): a path that quietly downloaded to compute on the
// host would land the result on the host.
TEST(PlacementDispatchTests, allDeviceOperandsStayResident) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] da = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
        "        float32[] db = [ 5.0f, 6.0f, 7.0f, 8.0f ];\n"
        "        int64[] shp = heap int64[1];\n"
        "        shp[0] = 4;\n"
        "        Tensor<float32> a = Tensor.of<float32>(da, shp);\n"
        "        Tensor<float32> b = Tensor.of<float32>(db, shp);\n"
        "        a.gpu();\n"
        "        b.gpu();\n"
        "        Tensor<float32> c = Ewise.arithF32Op(a, b, 0);\n"
        "        if (!c.isOnGpu()) { return -1; }\n"      // result is device-resident
        "        if (c.device() != 1) { return -2; }\n"
        "        if (!a.isOnGpu()) { return -3; }\n"      // operands were not migrated back
        "        if (!b.isOnGpu()) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 1.1.3 — operands split across host and device raise a located error naming
// both operands and their residency. It neither migrates nor downloads
// (spec 2.2, 13.1: mixed residency REJECTS, reversing the filed recommendation
// to migrate — torch-facade-spec §3.1.2-3.1.3 already made device type-level).
//
// RED until 1.2.2: `cajeta.math.PlacementException` does not exist yet, and the
// current guards fall through to the CPU path on a mixed pair rather than
// rejecting — a silent download.
TEST(PlacementDispatchTests, mixedResidencyRejects) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.Ewise;\n"
        "import cajeta.math.PlacementException;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] da = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
        "        float32[] db = [ 5.0f, 6.0f, 7.0f, 8.0f ];\n"
        "        int64[] shp = heap int64[1];\n"
        "        shp[0] = 4;\n"
        "        Tensor<float32> a = Tensor.of<float32>(da, shp);\n"
        "        Tensor<float32> b = Tensor.of<float32>(db, shp);\n"
        "        b.gpu();\n"                                  // a on host, b on device
        "        boolean threw = false;\n"
        "        try {\n"
        "            Tensor<float32> c = Ewise.arithF32Op(a, b, 0);\n"
        "        } catch (PlacementException ex) {\n"
        "            threw = true;\n"
        "        }\n"
        "        if (!threw) { return -1; }\n"
        "        if (a.isOnGpu()) { return -2; }\n"           // a was not migrated up
        "        if (!b.isOnGpu()) { return -3; }\n"          // b was not downloaded
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 1.1.4 — GEMM with m/n/k not multiples of 16 produces correct results
// (spec 2.6). Ewise.cajeta:355 documents the GPU path as requiring multiples of
// the 16x16 tile; the CPU floor has no such restriction.
//
// RED until 1.2.3 adds cooperative-matrix tail handling. 17x17x17 is chosen so
// every dimension has a ragged tail.
TEST(PlacementDispatchTests, gemmHandlesNonMultipleOf16) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] shp = heap int64[2];\n"
        "        shp[0] = 17;\n"
        "        shp[1] = 17;\n"
        "        Tensor<float32> a = Tensor.zeros<float32>(shp);\n"
        "        Tensor<float32> b = Tensor.zeros<float32>(shp);\n"
        "        int64 r = 0;\n"
        "        while (r < 17) {\n"
        "            int64 c = 0;\n"
        "            while (c < 17) {\n"
        "                a.set2(r, c, (float32) ((r + c) % 5));\n"
        "                b.set2(r, c, (float32) ((r * c) % 3));\n"
        "                c = c + 1;\n"
        "            }\n"
        "            r = r + 1;\n"
        "        }\n"
        "        Tensor<float32> host = Ewise.matmulF32Op(a, b);\n"   // CPU floor
        "        a.gpu();\n"
        "        b.gpu();\n"
        "        Tensor<float32> dev = Ewise.matmulF32Op(a, b);\n"    // tiled GEMM, ragged tail
        "        dev.cpu();\n"
        "        int64 i = 0;\n"
        "        while (i < 17) {\n"
        "            int64 j = 0;\n"
        "            while (j < 17) {\n"
        "                float32 d = host.get2(i, j) - dev.get2(i, j);\n"
        "                if (d < 0.0f) { d = 0.0f - d; }\n"
        "                if (d > 0.001f) { return -1; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 1.1.5 — an op with no kernel for a dtype/backend pair emits the tiering note
// and runs on CPU (spec 2.7), mirroring the existing `note: [mma-tiering]`
// convention emitted from KernelLowering.cpp:3373. The note is a severity below
// warning, so it must not fail the build; the op must still produce the right
// answer on the host floor.
//
// RED until 1.2.5 emits `note: [op-tiering]`. The result assertion below should
// pass today (the CPU floor already runs); the note capture is the new part.
TEST(PlacementDispatchTests, missingKernelTiersToCpuWithNote) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] shp = heap int64[2];\n"
        "        shp[0] = 16;\n"
        "        shp[1] = 16;\n"                              // matmul needs 2-D; 16 = one clean tile
        "        Tensor<float64> a = Tensor.zeros<float64>(shp);\n"
        "        Tensor<float64> b = Tensor.zeros<float64>(shp);\n"
        "        a.gpu();\n"
        "        b.gpu();\n"
        "        Tensor<float64> c = Ewise.matmulF64Op(a, b);\n"   // f64 has no native MMA
        "        c.cpu();\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    testing::internal::CaptureStderr();
    int32_t rc = runI32Xpu(src);
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 1);
    EXPECT_NE(err.find("[op-tiering]"), std::string::npos)
        << "expected a `note: [op-tiering]` naming op, dtype and backend; stderr was:\n" << err;
}

// 1.1.6 — axis-wise argmax/argmin match the host reference (spec 2.8).
// `Tensor.argmax<E>(t)` exists today but returns only the C-order FLATTENED
// index; numpy's `a.argmax(axis=)` has no counterpart.
//
// RED until 1.2.4: the axis overload does not exist.
TEST(PlacementDispatchTests, argmaxArgminAlongAxis) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // [[1,9,3],[7,2,8]] — argmax axis 1 = [1,2]; argmin axis 1 = [0,1]
        "        float32[] d = [ 1.0f, 9.0f, 3.0f, 7.0f, 2.0f, 8.0f ];\n"
        "        int64[] shp = heap int64[2];\n"
        "        shp[0] = 2;\n"
        "        shp[1] = 3;\n"
        "        Tensor<float32> t = Tensor.of<float32>(d, shp);\n"
        "        Tensor<int64> mx = Tensor.argmax<float32>(t, 1);\n"
        "        Tensor<int64> mn = Tensor.argmin<float32>(t, 1);\n"
        "        if (mx.get1(0) != 1) { return -1; }\n"
        "        if (mx.get1(1) != 2) { return -2; }\n"
        "        if (mn.get1(0) != 0) { return -3; }\n"
        "        if (mn.get1(1) != 1) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}
