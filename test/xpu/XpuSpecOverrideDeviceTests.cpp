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

using cajeta_test::CajetaJit;
using cajeta::xpu::vulkan::VulkanDriver;

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
