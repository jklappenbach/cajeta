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
// "Loudly" is the whole point. Without the guard, a Windows caller gets
// the generic "not a pollable descriptor" from the underlying probe,
// which reads as "your fd is wrong" when the truth is "this platform
// cannot do this at all". The message must name the platform AND the
// reason, so the test asserts on the message text rather than merely
// that something threw.
//
// This test is PLATFORM-CONDITIONAL by construction and is why the two
// awaitReadable suites belong in test/release_filter.txt: they lower
// differently per OS, so the Windows arm is only ever exercised by the
// release matrix. A local Linux sweep cannot see it — exactly how
// v0.25.0 shipped a Windows-only compile error two days ago.
//
// Pins (plan ids in brackets):
//   [3.1.1] Windows -> throws, message names the platform and the reason
//   [3.1.2] POSIX   -> the guard changes nothing
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<int32_t (*)()>("run")();
}

// Returns: 1 = threw and the message named the platform + reason,
//          2 = threw but the message was unhelpful,
//          0 = did not throw.
const char* kGuardProbe =
    "package test;\n"
    "import cajeta.io.file.FileReader;\n"
    "import cajeta.lang.String;\n"
    "public final class D {\n"
    "    public static int32 run() {\n"
    "        int32 fd = Cajeta.eventfdCreate();\n"
    "        if (fd < 0) { return -1; }\n"
    "        Cajeta.eventfdSignal(fd);\n"
    "        FileReader r = heap FileReader(fd);\n"
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

} // namespace

#if defined(_WIN32)

// [3.1.1] Windows refuses, and says why. A bare "unsupported" would leave
// the caller guessing whether their fd or their platform was the problem.
TEST(FileReaderAwaitPlatformTests, windowsThrowsNamingPlatformAndReason) {
    int32_t rc = runI32(kGuardProbe);
    ASSERT_NE(0, rc) << "awaitReadable returned normally on Windows, where "
                        "Winsock select() cannot poll a console/pipe HANDLE";
    EXPECT_EQ(1, rc)
        << "threw, but the message named neither the platform nor the "
           "reason — a caller cannot tell a bad fd from an unsupported OS";
}

#else

// [3.1.2] On POSIX the guard is inert: a readable fd is still readable,
// and nothing throws. This is the arm that catches a guard written with
// its condition inverted, which would otherwise only surface on the
// platform nobody runs locally.
TEST(FileReaderAwaitPlatformTests, posixIsUnaffectedByTheGuard) {
    int32_t rc = runI32(kGuardProbe);
    ASSERT_NE(-1, rc) << "eventfd creation failed";
    EXPECT_EQ(0, rc)
        << "awaitReadable threw on POSIX (rc=" << rc
        << ") — the platform guard fired on the wrong platform";
}

#endif
