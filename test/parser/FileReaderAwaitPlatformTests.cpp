//
// pollable-stdin unit 3 — the platform guard on
// `FileReader.awaitReadable` (spec 4.2.3, 4.2.4).
//
// Windows has no correct implementation of this wait today: Winsock
// `select()` accepts only SOCKETs, and a console or pipe HANDLE is not
// one. A real Win32 path (`PeekNamedPipe` / `WaitForSingleObject`) is a
// native subsystem in its own right — the same reason the reactor's
// kqueue and IOCP engines are still deferred. So v1 refuses, loudly.
//
// "Loudly" is the point. Without the guard a Windows caller gets the
// generic "not a pollable descriptor" from the underlying probe, which
// reads as "your fd is wrong" when the truth is "this platform cannot do
// this at all". So the test asserts the MESSAGE TEXT, not merely that
// something threw.
//
// PORTABILITY, learned the hard way (2026-08-30): this suite is in
// test/release_filter.txt so the Windows arm actually runs, which means
// it is compiled and executed on ALL FOUR targets. The first version used
// `Cajeta.eventfdCreate()` for its fixture — eventfd is LINUX-ONLY, so
// the darwin and mingw legs of the release dry-run both failed at "Run
// release tests". A test that must run everywhere cannot use a
// Linux-only fixture.
//
// The fixtures below are per-platform and minimal:
//   * Windows — the guard throws BEFORE the fd is touched, so any int
//     will do; fd 0 is never dereferenced.
//   * POSIX   — a real `pipe()` read end, created in C++ and passed as a
//     literal (the same trick FileReaderAwaitEofTests uses).
//
// Pins (plan ids in brackets):
//   [3.1.1] Windows -> throws, message names the platform and the reason
//   [3.1.2] POSIX   -> the guard changes nothing
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#if !defined(_WIN32)
#  include <unistd.h>
#endif

using cajeta_test::CajetaJit;

namespace {

// Returns: 1 = threw and the message named the platform + reason,
//          2 = threw but the message was unhelpful,
//          0 = did not throw.
std::string guardProbe(int fd) {
    return
        "package test;\n"
        "import cajeta.io.file.FileReader;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        FileReader r = heap FileReader(" + std::to_string(fd) + ");\n"
        "        try {\n"
        "            r.awaitReadable(0);\n"
        "        } catch (Throwable t) {\n"
        "            String m = t.getMessage();\n"
        "            if (m.contains(\"Windows\") && m.contains(\"select\")) {\n"
        "                return 1;\n"
        "            }\n"
        "            return 2;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
}

int32_t runProbe(int fd) {
    auto jit = CajetaJit::compile(guardProbe(fd), "test.D");
    return jit->lookup<int32_t (*)()>("run")();
}

} // namespace

#if defined(_WIN32)

// [3.1.1] Windows refuses, and says why. A bare "unsupported" would leave
// the caller guessing whether their fd or their platform was the problem.
// fd 0 is a placeholder: the guard fires before anything touches it.
TEST(FileReaderAwaitPlatformTests, windowsThrowsNamingPlatformAndReason) {
    int32_t rc = runProbe(0);
    ASSERT_NE(0, rc) << "awaitReadable returned normally on Windows, where "
                        "Winsock select() cannot poll a console/pipe HANDLE";
    EXPECT_EQ(1, rc)
        << "threw, but the message named neither the platform nor the "
           "reason — a caller cannot tell a bad fd from an unsupported OS";
}

#else

// [3.1.2] On POSIX the guard is inert: a readable fd is still readable and
// nothing throws. This is the arm that catches a guard written with its
// condition inverted, which would otherwise only surface on the platform
// nobody runs locally.
//
// A pipe, not an eventfd: this suite runs on darwin too, and eventfd is
// Linux-only.
TEST(FileReaderAwaitPlatformTests, posixIsUnaffectedByTheGuard) {
    int fds[2];
    ASSERT_EQ(0, ::pipe(fds)) << "pipe() failed";
    ssize_t w = ::write(fds[1], "x", 1);      // make the read end ready
    ASSERT_EQ(1, w);

    int32_t rc = runProbe(fds[0]);
    ::close(fds[0]);
    ::close(fds[1]);

    EXPECT_EQ(0, rc)
        << "awaitReadable threw on POSIX (rc=" << rc
        << ") — the platform guard fired on the wrong platform";
}

#endif
