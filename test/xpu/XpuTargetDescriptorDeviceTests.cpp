// TargetDescriptor — the single source of hardware facts for the cooperative
// cajeta.xpu surface (xpu-cooperative-tile spec §5, Phase A Unit 1).
//
// The headline is TargetDescriptor.waveWidth(): the COOPERATIVE-GROUP width,
// read on the device hot path, folding to a per-target compile-time constant.
// It is deliberately NOT Wave.width():
//   - on a GPU it IS the wave/subgroup size (32 on gfx1151 wave32);
//   - on the CPU backend it is 1 — the cooperative unit there is one work-item,
//     and SIMD is exploited BELOW this abstraction (spec §3.5). Wave.width() on
//     CPU returns the host SIMD width (8/16), which is the wrong number for the
//     Group model, so the descriptor is a distinct per-target lowering.
//
// This is where geometry-is-literals starts being paid down: no consumer ever
// writes 32 again — it reads the descriptor (spec §5.3). Unit 1 only stands the
// descriptor up; Unit 2's Group is the first consumer.
//
// waveWidth() is verified on BOTH backends (AMD device + CPU). The not-yet-
// reportable host facts (cuCount) are pinned for SHAPE, not value (spec §5.2):
// a stable interface that gains accuracy later with no call-site change.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options amdOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    return o;
}

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

// A @Kernel writes TargetDescriptor.waveWidth() into out[0]; run() launches it
// and reads it back. Same source for every backend — the value differs because
// the descriptor folds per target, which is the whole point.
const char* kWidthSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.TargetDescriptor;\n"
    "public class Tw {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<int32> out) {\n"
    "        uint32 t = KernelThread.x();\n"
    "        out[t] = TargetDescriptor.waveWidth();\n"
    "    }\n"
    "    public static int32 run(int32 block) {\n"
    "        KernelBuffer<int32> out = heap KernelBuffer<int32>(0, 64);\n"
    "        out.allocate();\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [block])(out);\n"
    "        s.sync();\n"
    "        int32[] h = heap int32[64];\n"
    "        out.download(h);\n"
    "        out.free();\n"
    "        return h[0];\n"
    "    }\n"
    "}\n";

// A host function returning TargetDescriptor.cuCount() — the not-yet-reportable
// fact, exercised host-side (it is a HOST query, not a device op).
const char* kCuCountSource =
    "package test;\n"
    "import cajeta.xpu.TargetDescriptor;\n"
    "public class Tc {\n"
    "    public static int32 run() {\n"
    "        return TargetDescriptor.cuCount();\n"
    "    }\n"
    "}\n";

} // namespace

// 1.1.1 (AMD): the descriptor folds to the real wave width on gfx1151 wave32.
TEST(XpuTargetDescriptorDevice, waveWidthIs32OnAmd) {
    CAJETA_SKIP_IF_NO_HIP();
    auto jit = CajetaJit::compile(kWidthSource, "test.Tw", amdOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)(int)>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(32), 32)
        << "TargetDescriptor.waveWidth() must fold to the wave32 width on "
           "gfx1151; a wrong value here is the geometry-is-literals defect";
}

// 1.1.1 (CPU): the descriptor folds to 1 — one work-item per cooperative group
// (spec §3.5). This is the value that MUST differ from Wave.width() on CPU
// (which returns the host SIMD width), so a delegation-to-Wave regression fails
// here.
TEST(XpuTargetDescriptorDevice, waveWidthIs1OnCpu) {
    auto jit = CajetaJit::compile(kWidthSource, "test.Tw", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)(int)>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(1), 1)
        << "TargetDescriptor.waveWidth() must be 1 on the CPU backend (one "
           "work-item per group); the host SIMD width would be wrong for the "
           "Group model";
}

// 1.1.2: a not-yet-reportable fact returns a documented stub behind a STABLE
// interface. The test pins the SHAPE — the method exists, is host-callable, and
// returns a non-negative count — not the stubbed value (spec §5.2). A count is
// never negative; the assertion is that the interface is present and stable, so
// a later unit can wire real accuracy in with no call-site change.
TEST(XpuTargetDescriptorDevice, cuCountHasAStableStubInterface) {
    auto jit = CajetaJit::compile(kCuCountSource, "test.Tc", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_GE(fn(), 0)
        << "cuCount() must present a stable non-negative interface (0 = unknown "
           "on a device that cannot report it)";
}
