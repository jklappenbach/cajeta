//
// Custom test entry point for the debugger TDD harness.
//
// Why not gtest_main: each end-to-end test spins up a full LLJIT (LLVM global
// state + the embedded Cajeta runtime, including its carrier-thread + static
// state). After several in-process JIT runs, normal process teardown races
// those static destructors and the process can exit non-zero *after* every
// test has already passed — which would make the harness's exit code
// untrustworthy (observed: "[ PASSED ] N tests" followed by exit=1). gtest's
// own verdict is the source of truth, so capture it and _exit immediately,
// skipping the fragile static-destructor teardown. The OS reclaims everything.
//
#include <gtest/gtest.h>

#if defined(_WIN32)
#  include <process.h>   // _exit
#else
#  include <unistd.h>    // _exit
#endif

#if defined(__GNUC__) && !defined(_WIN32)
// _exit below skips the atexit chain, and that also skips libgcov's counter
// dump — so a CAJETA_COVERAGE build of this target produced NO .gcda for its
// own 281 tests, and the whole dbg/ family read as uncovered while being
// exercised every run. Dump explicitly first. Resolved at runtime so an
// uninstrumented build gets null (an ELF-style weak declaration would not
// link on Mach-O — see test/ForkPerTest.cpp). Mirrors test/main.cpp.
#include <dlfcn.h>
static void (*debugGcovDumpFn())(void) {
    static void (*fn)(void) = reinterpret_cast<void (*)(void)>(
        dlsym(RTLD_DEFAULT, "__gcov_dump"));
    return fn;
}
#endif

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    int status = RUN_ALL_TESTS();
    fflush(nullptr);
#if defined(__GNUC__) && !defined(_WIN32)
    if (debugGcovDumpFn()) debugGcovDumpFn()();
#endif
    _exit(status);   // skip static-destructor teardown (see header)
}
