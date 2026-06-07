//
// R9.4 — I/O reactor / netpoller. End-to-end smoke for the epoll-backed
// fiber park/wake on fd readiness. Linux-only; on macOS / Windows the
// underlying intrinsics return -1 and we skip the tests.
//
// Each test uses an eventfd as the readiness source: a fiber calls
// Cajeta.ioWait(fd, READ) which parks it on the reactor; the main thread
// signals the eventfd, the reactor's epoll_wait fires, and the fiber
// resumes. This exercises the full reactor → publish_ready → carrier
// dispatch loop without needing pipes or sockets in the language
// surface.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

#if defined(__linux__)

// Main-thread (non-fiber) path: ioWait on an already-signalled eventfd
// returns immediately via the direct epoll_wait fallback. Verifies the
// intrinsic surface + the non-fiber code path's correctness without
// exercising the reactor thread.
TEST(IoReactorTests, mainThreadIoWaitReturnsOnReadyEventfd) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 fd = Cajeta.eventfdCreate();\n"
        "        if (fd < 0) { return -1; }\n"
        "        Cajeta.eventfdSignal(fd);\n"
        "        int32 ready = Cajeta.ioWait(fd, 1);\n"
        "        Cajeta.fdClose(fd);\n"
        "        return ready;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Pre-signal + spawn → fiber path. The signal happens before the fiber
// runs, so by the time the fiber calls ioWait the eventfd is already
// ready. The reactor immediately fires; the parker wakes; await returns
// the inner result. Exercises the reactor's lazy-start + waiter
// registration + epoll round-trip.
TEST(IoReactorTests, fiberIoWaitWakesOnPreSignaledEventfd) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 waitForFd(int32 fd) {\n"
        "        return Cajeta.ioWait(fd, 1);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 fd = Cajeta.eventfdCreate();\n"
        "        if (fd < 0) { return -1; }\n"
        "        Cajeta.eventfdSignal(fd);\n"
        "        int32 r = await spawn waitForFd(fd);\n"
        "        Cajeta.fdClose(fd);\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

#else /* !__linux__ */

TEST(IoReactorTests, platformStubReturnsMinusOne) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Cajeta.eventfdCreate();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), -1);
}

#endif
