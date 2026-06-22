//
// GfxSwapchainConfigTests — cajeta-gfx plan §4.c (slice 1): the swapchain WSI
// SELECTION POLICY (cajeta.gpu.gfx.SwapchainConfig).
//
// A swapchain is built from a description, and the description is derived by
// choosing — from what the driver reports for a Surface — the surface
// (format, colorSpace) pair, the present mode, the image count, and the extent.
// That choosing is pure, backend-independent logic: it is the verifiable-here
// core of §4.c (the live VK_KHR_swapchain acquire→present loop needs a device
// and is exercised on the device machine, like the §4.b SPIR-V binaries).
//
// Ordinals are the stable native contract (as for TextureFormat): the policy
// operates on the VK_* enum ordinals the runtime reports, so these analytic
// tests pass plain integers. Each test JITs a tiny program returning 0 on
// success or a negative failure code.
//
// GPU-free (pure host JIT; no device).
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& imports, const std::string& body) {
    std::string src =
        "package test;\n"
        + imports +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + body +
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* IMP = "import cajeta.gpu.gfx.SwapchainConfig;\n";

} // namespace

// 4c — present-mode policy: the preferred mode wins when the driver reports it;
// otherwise FIFO (ordinal 2 = VK_PRESENT_MODE_FIFO_KHR) is chosen, the one mode
// the Vulkan spec guarantees is always available.
TEST(GfxSwapchainConfigTests, presentModePrefersThenFallsBackToFifo) {
    EXPECT_EQ(runI32(IMP,
        "        int32[] avail = heap int32[3];\n"
        "        avail[0] = 0;\n"   // Immediate
        "        avail[1] = 2;\n"   // Fifo
        "        avail[2] = 3;\n"   // FifoRelaxed  (no Mailbox in this driver)
        "        int32 m = SwapchainConfig.choosePresentMode(avail, 3, 1);\n"  // prefer Mailbox(1)
        "        if (m != 2) { return -1; }\n"                                  // absent -> Fifo
        "        int32 m2 = SwapchainConfig.choosePresentMode(avail, 3, 0);\n" // prefer Immediate(0)
        "        if (m2 != 0) { return -2; }\n"                                 // present -> chosen
        "        return 0;\n"), 0);
}

// 4c — surface-format policy: an exact (format, colorSpace) match returns its
// index; failing that, the first pair whose colorSpace matches the preference;
// failing that, index 0 (the spec guarantees at least one pair). A zero-count
// available set returns -1.
TEST(GfxSwapchainConfigTests, surfaceFormatExactThenColorSpaceThenFirst) {
    EXPECT_EQ(runI32(IMP,
        "        int32[] fmt = heap int32[3];\n"
        "        int32[] cs  = heap int32[3];\n"
        "        fmt[0] = 44; cs[0] = 0;\n"   // BGRA8_UNORM, SRGB_NONLINEAR
        "        fmt[1] = 50; cs[1] = 0;\n"   // BGRA8_SRGB,  SRGB_NONLINEAR
        "        fmt[2] = 37; cs[2] = 0;\n"   // RGBA8_SRGB,  SRGB_NONLINEAR
        "        int32 a = SwapchainConfig.chooseSurfaceFormat(fmt, cs, 3, 50, 0);\n"
        "        if (a != 1) { return -1; }\n"                                  // exact pair -> idx 1
        "        int32 b = SwapchainConfig.chooseSurfaceFormat(fmt, cs, 3, 99, 0);\n"
        "        if (b != 0) { return -2; }\n"                                  // fmt absent, cs match -> idx 0
        "        int32 c = SwapchainConfig.chooseSurfaceFormat(fmt, cs, 3, 99, 7);\n"
        "        if (c != 0) { return -3; }\n"                                  // neither -> idx 0
        "        int32 z = SwapchainConfig.chooseSurfaceFormat(fmt, cs, 0, 50, 0);\n"
        "        if (z != -1) { return -4; }\n"                                 // empty -> -1
        "        return 0;\n"), 0);
}

// 4c — image-count policy: the desired count clamped into the surface's
// [minImageCount, maxImageCount], where maxImageCount == 0 means "no upper
// bound" (the Vulkan convention).
TEST(GfxSwapchainConfigTests, imageCountClampsIntoRange) {
    EXPECT_EQ(runI32(IMP,
        "        if (SwapchainConfig.chooseImageCount(2, 8, 3) != 3) { return -1; }\n"  // in range
        "        if (SwapchainConfig.chooseImageCount(4, 8, 3) != 4) { return -2; }\n"  // clamp up to min
        "        if (SwapchainConfig.chooseImageCount(2, 3, 5) != 3) { return -3; }\n"  // clamp down to max
        "        if (SwapchainConfig.chooseImageCount(2, 0, 9) != 9) { return -4; }\n"  // max 0 = no bound
        "        return 0;\n"), 0);
}

// 4c — extent policy: when the driver reports a concrete currentExtent it is
// mandatory and returned verbatim; when it reports the special 0xFFFFFFFF
// ("size driven by the swapchain" — Wayland/some X11) the caller's desired
// framebuffer size is used, clamped to [minExtent, maxExtent].
TEST(GfxSwapchainConfigTests, extentUsesCurrentOrClampsDesired) {
    EXPECT_EQ(runI32(IMP,
        "        if (SwapchainConfig.chooseExtentWidth(1280, 800, 1, 4096) != 1280) { return -1; }\n"   // current mandatory
        "        if (SwapchainConfig.chooseExtentHeight(720, 600, 1, 4096) != 720) { return -2; }\n"
        "        uint32 undef = 4294967295;\n"
        "        if (SwapchainConfig.chooseExtentWidth(undef, 800, 1, 4096) != 800) { return -3; }\n"     // desired in range
        "        if (SwapchainConfig.chooseExtentWidth(undef, 8000, 1, 4096) != 4096) { return -4; }\n"   // clamp to max
        "        if (SwapchainConfig.chooseExtentHeight(undef, 0, 16, 4096) != 16) { return -5; }\n"      // clamp to min
        "        return 0;\n"), 0);
}
