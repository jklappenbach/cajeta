//
// The JIT harness's multi-source path lays each (fqClass, source) pair under a
// fresh temp source-root (plus a paired archive root) so a file's path-derived
// package matches its `package X;` declaration. Nothing ever removed them.
//
// A full suite run left ~7,800 of these behind (one `cajeta_multi_*` and one
// `cajeta_archive_*` per multi-source test), and they accumulated across runs.
// On this host `/tmp` is a tmpfs carrying a per-user quota that `df` does not
// report, so the debris eventually exhausted the quota and broke UNRELATED
// work: `cc1plus` failed mid-compile with "Disk quota exceeded", one build
// linked an object tree it had only partially written and produced a binary
// that SIGSEGV'd on startup, and a full suite run reported 89 failures that
// did not reproduce once the debris was cleared. See profiler plan 6.5.
//
// The contract asserted here: a multi-source compile leaves no temp roots
// behind once its CajetaJit is destroyed.
//

#include "gtest/gtest.h"
#include "../PortableEnv.h"   // setenv/unsetenv on MinGW
#include "JitTestHelper.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Entries the harness's multi-source path creates, by name.
size_t countHarnessTempDirs() {
    size_t n = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(
             std::filesystem::temp_directory_path(), ec)) {
        const auto name = e.path().filename().string();
        if (name.rfind("cajeta_multi_", 0) == 0 ||
            name.rfind("cajeta_archive_", 0) == 0) {
            ++n;
        }
    }
    return n;
}

// Point TMPDIR (what temp_directory_path() reads) at a directory only this
// test uses, and put it back on the way out. Without this the counts below
// pick up any other cajeta process sharing the ambient TMPDIR — the full
// suite running alongside this one moved the count by 2 in a 16-second
// window, which is exactly the delta under test.
class PrivateTmpDir {
public:
    PrivateTmpDir() {
        if (const char* prev = std::getenv("TMPDIR")) {
            had = true;
            saved = prev;
        }
#if defined(_WIN32)
        if (const char* t = std::getenv("TMP"))  { hadTmp = true;  savedTmp = t; }
        if (const char* e = std::getenv("TEMP")) { hadTemp = true; savedTemp = e; }
#endif
        dir = std::filesystem::temp_directory_path() / "cajeta_tmpdir_cleanup_test";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir);
        ::setenv("TMPDIR", dir.string().c_str(), /*overwrite=*/1);
#if defined(_WIN32)
        // std::filesystem::temp_directory_path() reads TMP/TEMP on Windows,
        // never TMPDIR, so the private-dir redirect needs those too (release
        // full sweep 2026-09-06).
        ::setenv("TMP", dir.string().c_str(), 1);
        ::setenv("TEMP", dir.string().c_str(), 1);
#endif
    }
    ~PrivateTmpDir() {
        if (had) ::setenv("TMPDIR", saved.c_str(), 1);
        else     ::unsetenv("TMPDIR");
#if defined(_WIN32)
        if (hadTmp)  ::setenv("TMP",  savedTmp.c_str(), 1);  else ::unsetenv("TMP");
        if (hadTemp) ::setenv("TEMP", savedTemp.c_str(), 1); else ::unsetenv("TEMP");
#endif
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    const std::filesystem::path& path() const { return dir; }

private:
    std::filesystem::path dir;
    std::string saved, savedTmp, savedTemp;
    bool had = false, hadTmp = false, hadTemp = false;
};

} // namespace

TEST(JitTempDirCleanupTests, multiSourceCompileLeavesNoTempRootsBehind) {
    PrivateTmpDir tmp;
    const size_t before = countHarnessTempDirs();
    ASSERT_EQ(before, 0u) << "private TMPDIR should start clean";

    {
        std::map<std::string, std::string> sources = {
            {"test.Helper",
             "package test;\n"
             "public final class Helper {\n"
             "    public static int32 five() { return 5; }\n"
             "}\n"},
            {"test.I",
             "package test;\n"
             "import test.Helper;\n"
             "public final class I {\n"
             "    public static int32 run() { return Helper.five(); }\n"
             "}\n"},
        };

        auto jit = CajetaJit::compile(sources, "test.I");
        ASSERT_NE(jit, nullptr);
        auto fn = jit->lookup<int32_t (*)()>("run");
        ASSERT_NE(fn, nullptr);
        EXPECT_EQ(fn(), 5);
    }

    EXPECT_EQ(countHarnessTempDirs(), before)
        << "multi-source compile left temp roots under " << tmp.path().string();
}
