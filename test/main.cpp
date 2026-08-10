#include "gtest/gtest.h"

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#  include <cstdlib>
#  include <string>

// The build-tool task/exec fixtures invoke POSIX coreutils (echo, test, true)
// and shebang scripts that resolve through the MSYS2 shells. Those ship in the
// toolchain's usr\bin, which is intentionally kept off PATH for the cmd test
// runner (its MSYS coreutils would shadow Windows find/sort there). Inside the
// test process it is safe and necessary: APPEND it (so native/mingw tools keep
// priority) to give those commands a resolution path with no native equivalent.
static void appendMsysCoreutilsToPath() {
    const char* root = std::getenv("MSYS2_ROOT");
    std::string usrbin =
        std::string(root && *root ? root : "C:\\msys64") + "\\usr\\bin";
    const char* cur = std::getenv("PATH");
    std::string p = cur ? cur : "";
    if (!p.empty() && p.back() != ';') p += ';';
    p += usrbin;
    _putenv_s("PATH", p.c_str());
}
#endif

#if defined(__GNUC__) && !defined(_WIN32)
// Weak so uninstrumented builds resolve it to null — see the coverage note at
// the _Exit below.
extern "C" __attribute__((weak)) void __gcov_dump(void);
#endif

#if !defined(_WIN32)
// The fork-per-test prime server (ForkPerTest.cpp): prime once, fork a COW
// child per test. Dispatched by CAJETA_FORK_PER_TEST=1.
int cajetaForkPerTestMain();
#endif

int main(int argc, char **argv) {
#if defined(_WIN32)
    appendMsysCoreutilsToPath();
#endif
    ::testing::InitGoogleTest(&argc, argv);
#if !defined(_WIN32)
    if (std::getenv("CAJETA_FORK_PER_TEST")) {
        const int frc = cajetaForkPerTestMain();
        std::fflush(nullptr);
        std::_Exit(frc);
    }
#endif
    const int rc = RUN_ALL_TESTS();

    // Bypass the C/C++ exit-handler chain (atexit/static dtors) and terminate
    // immediately. cajeta_test statically links the fork LLVM, while a GPU
    // backend (HIP comgr / RADV) dlopens its OWN LLVM at device init. At process
    // exit the two copies' global `cl::opt` ManagedStatic registries collide and
    // an LLVM option global (e.g. AsmMacroMaxNestingDepth) is freed with a
    // corrupted chunk size -> "double free or corruption" -> SIGABRT, AFTER all
    // tests have already passed. gtest has fully printed/flushed its results by
    // the time RUN_ALL_TESTS returns, so skipping teardown is observationally
    // safe and lets ctest see the real pass/fail exit code. CPU-only runs (no 2nd
    // LLVM) were unaffected either way.
    std::fflush(nullptr);
#if defined(__GNUC__) && !defined(_WIN32)
    // _Exit below skips the atexit chain (see above) — which also skips the
    // gcov counter dump in CAJETA_COVERAGE builds, silently producing zero
    // .gcda. Dump explicitly first; null in uninstrumented builds.
    if (__gcov_dump) __gcov_dump();
#endif
    std::_Exit(rc);
}
