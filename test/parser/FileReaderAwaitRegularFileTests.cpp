//
// pollable-stdin unit 2 — the regular-file contract for
// `FileReader.awaitReadable` (spec 3.2.1-3.2.5).
//
// Why this contract exists: `serve < requests.jsonl` is a first-class way
// to drive a line-oriented server, so a redirected regular file must
// behave like a pipe from the caller's side. The platform disagrees about
// what that means — Linux `epoll_ctl` REJECTS a regular file with EPERM,
// while `select()` reports one as always ready — and the difference must
// not reach the caller.
//
// NOTE ON SCOPE (measured 2026-08-30): unit 1 shipped WITHOUT the Linux
// epoll arm the plan assumed (plan 1.2.2 deviation — the native await
// takes no timeout, and epoll would EPERM on exactly these fds). Every
// wait therefore runs the portable select() probe, under which a regular
// file is already always-ready. So these tests may pass with no new code
// at all. That is the finding, not a gap: unit 2's job becomes PINNING
// the contract so a future epoll fast-path cannot silently break it.
// Plan 2.1.5's "disable classification and watch 2.1.1 fail" control is
// not runnable while no classification exists — recorded, not faked.
//
// Pins (plan ids in brackets):
//   [2.1.1] a regular file is READY at once, never an EPERM-shaped error
//   [2.1.2] a regular file AT EOF is still READY (the read returns 0)
//   [2.1.3] a pipe still follows its data (no regression from unit 1)
//   [2.1.4] a terminal follows its data — SKIPPED, no pty here, stated
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<int32_t (*)()>("run")();
}

// Repo-local tmp, never /tmp.
std::string writeFixture(const std::string& name, const std::string& body) {
    std::string dir = "tmp/pollable-stdin";
    std::filesystem::create_directories(dir);
    std::string path = dir + "/" + name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
    out.close();
    return std::filesystem::absolute(path).string();
}

std::string prog(const std::string& body) {
    return
        "package test;\n"
        "import cajeta.io.file.File;\n"
        "import cajeta.io.file.FileReader;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        + body +
        "    }\n"
        "}\n";
}

} // namespace

#if defined(__linux__)

// [2.1.1] A regular file with bytes in it is readable at once. The failure
// this guards is an EPERM surfacing as an exception, which is what a naive
// epoll registration would do.
TEST(FileReaderAwaitRegularFileTests, regularFileIsReadyImmediately) {
    auto path = writeFixture("ready.txt", "{\"op\":\"ping\"}\n");
    auto src = prog(
        "        FileReader r #= File.openRead(\"" + path + "\");\n"
        "        boolean threw = false;\n"
        "        boolean ready = false;\n"
        "        try {\n"
        "            ready = r.awaitReadable(1000);\n"
        "        } catch (Throwable t) {\n"
        "            threw = true;\n"
        "        }\n"
        "        r.close();\n"
        "        if (threw) { return -2; }\n"
        "        if (ready) { return 1; }\n"
        "        return 0;\n");
    int32_t rc = runI32(src);
    ASSERT_NE(-2, rc) << "a regular file raised an error (EPERM leaked?)";
    EXPECT_EQ(1, rc) << "a regular file must report readable";
}

// [2.1.2] Still READY once drained. Readiness is not "has bytes" — it is
// "a read will not block", and at EOF a read returns 0 immediately. A
// serve loop learns it is done from that 0, so reporting NOT-ready here
// would hang it instead.
TEST(FileReaderAwaitRegularFileTests, regularFileAtEofIsStillReady) {
    auto path = writeFixture("eof.txt", "ab");
    auto src = prog(
        "        FileReader r #= File.openRead(\"" + path + "\");\n"
        "        int8[] buf = heap int8[8];\n"
        "        int32 n = r.read(buf, 8);\n"           // drain the 2 bytes
        "        int32 n2 = r.read(buf, 8);\n"          // now at EOF -> 0
        "        boolean ready = r.awaitReadable(1000);\n"
        "        r.close();\n"
        "        if (n2 != 0) { return -3; }\n"
        "        if (ready) { return 1; }\n"
        "        return 0;\n");
    int32_t rc = runI32(src);
    ASSERT_NE(-3, rc) << "fixture never reached EOF";
    EXPECT_EQ(1, rc)
        << "a drained regular file reported NOT ready — a serve loop "
           "would hang instead of seeing EOF";
}

// [2.1.3] The unit-1 behaviour is not regressed: a descriptor with no data
// still reports not-ready and still times out. Pairs with 2.1.1 — together
// they show readiness is fd-KIND-aware rather than uniformly "yes".
TEST(FileReaderAwaitRegularFileTests, aPipeStillFollowsItsData) {
    auto src =
        "package test;\n"
        "import cajeta.io.file.FileReader;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 fd = Cajeta.eventfdCreate();\n"
        "        if (fd < 0) { return -1; }\n"
        "        FileReader r = heap FileReader(fd);\n"
        "        boolean idle = r.awaitReadable(50);\n"
        "        Cajeta.eventfdSignal(fd);\n"
        "        boolean live = r.awaitReadable(50);\n"
        "        if (idle) { return -2; }\n"
        "        if (!live) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    int32_t rc = runI32(src);
    ASSERT_NE(-2, rc) << "an idle fd reported readable — readiness is "
                         "uniformly true, so 2.1.1 proves nothing";
    ASSERT_NE(-3, rc) << "a signalled fd reported not readable";
    EXPECT_EQ(1, rc);
}

// [2.1.4] A terminal follows its data. Not runnable here: the test binary
// has no controlling pty, and forging one (openpty) would test the pty
// rather than the contract. Skipped LOUDLY per the plan — never silently
// passed.
TEST(FileReaderAwaitRegularFileTests, DISABLED_terminalFollowsItsData) {
    GTEST_SKIP() << "no pty in the test environment; plan 2.1.4 records "
                    "this as skipped-with-reason rather than passed";
}

#endif // __linux__
