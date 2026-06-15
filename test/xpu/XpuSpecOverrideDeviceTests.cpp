//
// Cajeta XPU — host-side specialization-constant override (Stage 11/12).
//
// `Spec.geti(slot, default)` reads a user spec constant; the launch can OVERRIDE
// its value with a new `spec:[v0, v1, …]` config arg (entry i → slot i). The
// frontend lowers a `spec:`-bearing launch to __cajeta_xpu_launch_v3, which
// threads the values to the backend: Vulkan binds them as genuine OpSpecConstants
// at pipeline creation; CPU reads them at runtime (Phase B); AMD/NVPTX bake them
// (Phase C). See plans/gpu/xpu/stage12-spec-override-plan.md.
//
// Phase A: Vulkan/RADV on-device. The override changes the observed result vs the
// default-reading baseline (specConstantDefaultOnDevice), proving the value rode
// the launch FFI to the pipeline. Skips cleanly without a Vulkan compute device.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"
#include "cajeta/xpu/amd/HipDriver.h"

using cajeta_test::CajetaJit;
using cajeta::xpu::vulkan::VulkanDriver;
using cajeta::xpu::amd::HipDriver;

namespace {

CajetaJit::Options vulkanOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    return o;
}

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

CajetaJit::Options amdOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    return o;
}

// One spec slot: out[i] = Spec.geti(0, 7). With spec:[V] every element == V
// (override); the kernel's compile-time default (7) is shadowed.
const char* kOneSpecFill =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class SO {\n"
    "    @Kernel\n"
    "    public static void fill(Buffer<int32> out, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) { out[i] = Spec.geti(0, 7); }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 n = 64;\n"
    "        int32[] hout = heap int32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1; }\n"
    "        Buffer<int32> out = heap Buffer<int32>(0, n);\n"
    "        out.allocate();\n"
    "        out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
    "        fill.launch(s, grid: [1], block: [64], spec: [1234])(out, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        out.free();\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            if (hout[i] != 1234) { return (int32)(100 + i); }\n"
    "        }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

// Two spec slots, override only slot 0: out[2i] = Spec.geti(0, 11) (overridden to
// V), out[2i+1] = Spec.geti(1, 22) (unset → default 22). Proves slot-indexing and
// that an unset trailing slot keeps its compile-time default.
const char* kPartialSpecFill =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class SOP {\n"
    "    @Kernel\n"
    "    public static void fill(Buffer<int32> out, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) {\n"
    "            out[i*2] = Spec.geti(0, 11);\n"
    "            out[i*2 + 1] = Spec.geti(1, 22);\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 n = 32;\n"
    "        int32[] hout = heap int32[n*2];\n"
    "        for (uint32 i = 0; i < n*2; i = i + 1) { hout[i] = -1; }\n"
    "        Buffer<int32> out = heap Buffer<int32>(0, n*2);\n"
    "        out.allocate();\n"
    "        out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
    "        fill.launch(s, grid: [1], block: [64], spec: [555])(out, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        out.free();\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            if (hout[i*2] != 555) { return (int32)(100 + i); }\n"      // slot 0 overridden
    "            if (hout[i*2 + 1] != 22) { return (int32)(200 + i); }\n"   // slot 1 default
    "        }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

// f32 override: out[i] = Spec.getf(0, 2.5). With spec:[7.5] every element == 7.5
// (the float bit-pattern rides the i32 transport word, reinterpreted by the
// consumer). All values are small integers-over-2 → float-exact, so == is safe.
const char* kFloatSpecFill =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class SF {\n"
    "    @Kernel\n"
    "    public static void fill(Buffer<float32> out, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) { out[i] = Spec.getf(0, 2.5f); }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 n = 64;\n"
    "        float32[] hout = heap float32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
    "        Buffer<float32> out = heap Buffer<float32>(0, n);\n"
    "        out.allocate();\n"
    "        out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
    "        fill.launch(s, grid: [1], block: [64], spec: [7.5f])(out, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        out.free();\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            float32 d = hout[i] - 7.5f;\n"
    "            if (d < -0.001f || d > 0.001f) { return (int32)(100 + i); }\n"
    "        }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

} // namespace

// Phase A: a host spec override rides the launch FFI to a genuine OpSpecConstant
// on Vulkan — every element reads the overridden 1234, not the kernel default 7.
TEST(XpuSpecOverrideDeviceTests, specOverrideRoutesToVulkanOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    auto jit = CajetaJit::compile(kOneSpecFill, "test.SO", vulkanOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != overridden 1234)";
}

// Phase A: slot-indexed + sparse — overriding slot 0 leaves slot 1 at its default.
TEST(XpuSpecOverrideDeviceTests, specOverridePartialReadsDefaultForUnsetSlotsOnVulkan) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    auto jit = CajetaJit::compile(kPartialSpecFill, "test.SOP", vulkanOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777)
        << "fail code " << r
        << " (100+i: slot0 != overridden 555; 200+i: slot1 != default 22)";
}

// Phase B: CPU honors the override by reading it at runtime — the oracle now
// reproduces the device result (uniform observable semantics). The SAME source
// the Vulkan test runs, here on CPU: every element reads the overridden 1234.
TEST(XpuSpecOverrideTests, specOverrideMatchesDeviceOnCpu) {
    auto jit = CajetaJit::compile(kOneSpecFill, "test.SO", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != overridden 1234)";
}

// Phase B: CPU slot-indexing + sparse tail — overriding slot 0 leaves slot 1 at
// its compile-time default (the runtime helper returns the default for unset slots).
TEST(XpuSpecOverrideTests, specOverridePartialReadsDefaultForUnsetSlotsOnCpu) {
    auto jit = CajetaJit::compile(kPartialSpecFill, "test.SOP", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777)
        << "fail code " << r
        << " (100+i: slot0 != overridden 555; 200+i: slot1 != default 22)";
}

// Phase C on-device (AMD/gfx1151): the constant-memory override path runs on a
// real HIP device — the runtime sets __cajeta_xpu_spec_count/_values per launch
// and the kernel reads the override (1234, not the default 7). This validates
// the cu/hipModuleGetGlobal + copy path the emit gate could only prove was wired.
// (Unblocked by the --exclude-libs comgr-interposition fix; skips without HIP.)
TEST(XpuSpecOverrideDeviceTests, specOverrideRoutesToAmdOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no AMD HIP device available";
    }
    auto jit = CajetaJit::compile(kOneSpecFill, "test.SO", amdOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != overridden 1234)";
}

// Phase C on-device (AMD): slot-indexed + sparse — slot 0 overridden, slot 1 default.
TEST(XpuSpecOverrideDeviceTests, specOverridePartialReadsDefaultForUnsetSlotsOnAmd) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no AMD HIP device available";
    }
    auto jit = CajetaJit::compile(kPartialSpecFill, "test.SOP", amdOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777)
        << "fail code " << r
        << " (100+i: slot0 != overridden 555; 200+i: slot1 != default 22)";
}

// Phase C on-device (AMD): no `spec:` → reads the compile-time default (the
// zero-init safe-default path; runtime writes count 0).
TEST(XpuSpecOverrideDeviceTests, noOverrideReadsDefaultOnAmd) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no AMD HIP device available";
    }
    const char* src =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class SDA {\n"
        "    @Kernel\n"
        "    public static void fill(Buffer<int32> out, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) { out[i] = Spec.geti(0, 99); }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 64;\n"
        "        int32[] hout = heap int32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1; }\n"
        "        Buffer<int32> out = heap Buffer<int32>(0, n);\n"
        "        out.allocate();\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        fill.launch(s, grid: [1], block: [64])(out, n);\n"   // no spec:
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        out.free();\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != 99) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.SDA", amdOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != default 99)";
}

// f32 override on Vulkan — Spec.getf(0, 2.5) overridden to 7.5 via a float
// OpSpecConstant; proves the raw-word transport reinterprets correctly on device.
TEST(XpuSpecOverrideDeviceTests, floatSpecOverrideRoutesToVulkanOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    auto jit = CajetaJit::compile(kFloatSpecFill, "test.SF", vulkanOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != overridden 7.5)";
}

// f32 override on CPU — the oracle reproduces it (the runtime helper reinterprets
// the transport word as float). Matches the Vulkan result (uniform semantics).
TEST(XpuSpecOverrideTests, floatSpecOverrideMatchesDeviceOnCpu) {
    auto jit = CajetaJit::compile(kFloatSpecFill, "test.SF", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != overridden 7.5)";
}

// Phase B: no `spec:` → CPU reads the compile-time default (unchanged behavior;
// the override path is inert when nothing is supplied).
TEST(XpuSpecOverrideTests, noOverrideReadsDefaultOnCpu) {
    const char* src =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class SD {\n"
        "    @Kernel\n"
        "    public static void fill(Buffer<int32> out, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) { out[i] = Spec.geti(0, 99); }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 64;\n"
        "        int32[] hout = heap int32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1; }\n"
        "        Buffer<int32> out = heap Buffer<int32>(0, n);\n"
        "        out.allocate();\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        fill.launch(s, grid: [1], block: [64])(out, n);\n"   // no spec:
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        out.free();\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != 99) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.SD", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != default 99)";
}
