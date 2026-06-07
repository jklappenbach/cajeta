#include "gtest/gtest.h"

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
    return RUN_ALL_TESTS();
}
