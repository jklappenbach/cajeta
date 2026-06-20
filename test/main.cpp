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

int main(int argc, char **argv) {
#if defined(_WIN32)
    appendMsysCoreutilsToPath();
#endif
    ::testing::InitGoogleTest(&argc, argv);
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
    std::_Exit(rc);
}
