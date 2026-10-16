//
// ReadySetJitTests — request-dispatch plan 1.2.3: the cajeta surface over the
// readiness set (`cajeta.io.net.reactor.ReadySet`) and the carrier count
// (`cajeta.concurrent.Carriers.count()`), driven under the real scheduler
// from JIT-compiled cajeta source. The native engine itself is pinned by
// test/net/ReadySetTests.cpp.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#if !defined(_WIN32)
#  include <sys/socket.h>
#  include <unistd.h>
#endif

using cajeta_test::CajetaJit;

extern "C" {
    int32_t __cajeta_carriers_count(void);
}

// Carriers.count() is the runtime's number, positive, and stable.
TEST(ReadySetJitTests, carriersCountIsTheRuntimesNumber) {
    auto src =
        "package test;\n"
        "import cajeta.concurrent.Carriers;\n"
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        return Carriers.count();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    int32_t n = jit->lookup<int32_t (*)()>("run")();
    EXPECT_GT(n, 0);
    EXPECT_EQ(n, __cajeta_carriers_count());
    EXPECT_EQ(n, jit->lookup<int32_t (*)()>("run")());
}

#if !defined(_WIN32)
// A fiber waits on a ReadySet holding one socket; the test thread writes to
// its peer after the wait has begun; the fiber reports that fd and the
// bytes are readable afterwards. A second wait with the byte consumed times
// out with 0.
TEST(ReadySetJitTests, fiberWaitsOnASetAndWakesWithTheReadyFd) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.reactor.ReadySet;\n"
        "public final class T {\n"
        "    public static int32 worker(int32 fd, int32 timeoutMs) {\n"
        "        ReadySet s = heap ReadySet();\n"
        "        if (!s.add(fd, ReadySet.READ)) { return -2; }\n"
        "        int32[] fds = heap int32[4];\n"
        "        int32[] evs = heap int32[4];\n"
        "        int32 n = s.wait(fds, evs, 4, timeoutMs);\n"
        "        int32 r = -3;\n"
        "        if (n == 0) { r = 0; }\n"
        "        if (n == 1) { r = fds[0]; }\n"
        "        s.close();\n"
        "        return r;\n"
        "    }\n"
        "    public static int32 run(int32 fd, int32 timeoutMs) {\n"
        "        Task<int32> t = spawn worker(fd, timeoutMs);\n"
        "        return await t;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    auto run = jit->lookup<int32_t (*)(int32_t, int32_t)>("run");

    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    // Writer: lets the wait begin, then wakes it.
    std::thread writer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        char b = 'z';
        (void) !write(sv[1], &b, 1);
    });
    int32_t got = run(sv[0], 5000);
    writer.join();
    EXPECT_EQ(got, sv[0]) << "the wait must report the readable member";

    char c = 0;
    EXPECT_EQ(1, (int) read(sv[0], &c, 1));
    EXPECT_EQ(c, 'z');

    // Quiet now: a bounded wait reports 0.
    EXPECT_EQ(0, run(sv[0], 80));

    close(sv[0]);
    close(sv[1]);
}
#endif
