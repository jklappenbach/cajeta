//
// GfxFrameSyncTests — cajeta-gfx plan §4.c (slice 2): the frames-in-flight
// SYNC POLICY (cajeta.gpu.gfx.FrameSync) and a parse/anchor check on the
// Swapchain noun.
//
// A swapchain present loop lets the CPU run a bounded number of frames ahead of
// the GPU: frame N records into ring slot (N % framesInFlight) and must wait on
// that slot's fence before reuse, and the CPU may only get `framesInFlight`
// frames ahead of what has presented. That bookkeeping is pure arithmetic — the
// verifiable-here core of TDD 4.d ("acquire→render→present with correct
// frames-in-flight sync"); the live VK_KHR_swapchain acquire/queue-present (and
// the headless offscreen ring) are device-machine work behind Swapchain's
// @Native primitives.
//
// One test also imports cajeta.gpu.gfx.Swapchain so the noun is parsed +
// prototype-generated (it consumes a cajeta.ifx.Surface) without touching its
// device @Native methods.
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

const char* IMP = "import cajeta.gpu.gfx.FrameSync;\n";

} // namespace

// 4c — ring-slot rotation: nextSlot advances and wraps at framesInFlight, so a
// double-buffered loop ping-pongs 0,1,0,1 and a triple-buffered one cycles
// 0,1,2,0.
TEST(GfxFrameSyncTests, nextSlotRotatesAndWraps) {
    EXPECT_EQ(runI32(IMP,
        "        if (FrameSync.nextSlot(0, 2) != 1) { return -1; }\n"
        "        if (FrameSync.nextSlot(1, 2) != 0) { return -2; }\n"
        "        if (FrameSync.nextSlot(0, 3) != 1) { return -3; }\n"
        "        if (FrameSync.nextSlot(1, 3) != 2) { return -4; }\n"
        "        if (FrameSync.nextSlot(2, 3) != 0) { return -5; }\n"
        "        return 0;\n"), 0);
}

// 4c — slotForFrame maps an absolute frame index onto its ring slot
// (frameIndex % framesInFlight): frames 0..4 at framesInFlight 2 give
// 0,1,0,1,0; at framesInFlight 3 give 0,1,2,0,1.
TEST(GfxFrameSyncTests, slotForFrameIsModuloRing) {
    EXPECT_EQ(runI32(IMP,
        "        if (FrameSync.slotForFrame(0, 2) != 0) { return -1; }\n"
        "        if (FrameSync.slotForFrame(1, 2) != 1) { return -2; }\n"
        "        if (FrameSync.slotForFrame(4, 2) != 0) { return -3; }\n"
        "        if (FrameSync.slotForFrame(2, 3) != 2) { return -4; }\n"
        "        if (FrameSync.slotForFrame(3, 3) != 0) { return -5; }\n"
        "        return 0;\n"), 0);
}

// 4c — the acquire gate: the CPU may be at most framesInFlight frames ahead of
// what has presented. With framesInFlight = 2 it may acquire while 0 or 1 frames
// are in flight, but must wait once 2 are outstanding.
TEST(GfxFrameSyncTests, canAcquireBoundsInFlight) {
    EXPECT_EQ(runI32(IMP,
        "        if (!FrameSync.canAcquire(0, 2)) { return -1; }\n"
        "        if (!FrameSync.canAcquire(1, 2)) { return -2; }\n"
        "        if (FrameSync.canAcquire(2, 2)) { return -3; }\n"   // saturated -> must wait
        "        if (FrameSync.canAcquire(3, 2)) { return -4; }\n"
        "        return 0;\n"), 0);
}

// 4c — the Swapchain noun builds from a (headless) Surface with a resolved
// description and reports it back; acquire/present plumb through the host floor.
// This exercises the noun + RAII + getters without a device (the live
// VK_KHR_swapchain create/acquire/present is device-machine work; the host C
// floor returns a constructed token, image 0, and no-op present/free).
TEST(GfxFrameSyncTests, headlessSwapchainConstructsAndReportsDescription) {
    EXPECT_EQ(runI32(
        "import cajeta.ifx.Surface;\n"
        "import cajeta.gpu.gfx.Swapchain;\n",
        "        Surface s = heap Surface(0, 5, 800, 600);\n"          // headless (handle 0)
        "        Swapchain sc = heap Swapchain(s, 50, 0, 2, 3);\n"     // fmt 50, srgb, FIFO, 3 imgs
        "        if (sc.extentWidth() != 800) { return -1; }\n"
        "        if (sc.extentHeight() != 600) { return -2; }\n"
        "        if (sc.bufferCount() != 3) { return -3; }\n"
        "        if (sc.colorFormat() != 50) { return -4; }\n"
        "        if (sc.presentModeTag() != 2) { return -5; }\n"
        "        uint32 img = sc.acquireNextImage();\n"
        "        if (img >= sc.bufferCount()) { return -6; }\n"        // a valid image index
        "        sc.present(img);\n"
        "        return 0;\n"), 0);
}
