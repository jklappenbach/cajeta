// The whole-artifact cache key folds the native inputs a library build bakes
// (build-cache-native-key spec 2.1–2.4, 3.1, 3.2).
//
// Measured on cajeta-crypto 2026-10-18: a `.cja` built before its shim was
// provided stayed cached after `CAJETA_NATIVE_PATH` named the shim, and a
// shim rebuilt from edited C shipped stale, because the key saw flags, sources
// and skills only. Native archives are build inputs: the key digests every
// native file the bake could find, and the project's own `native/` is one of
// the places it looks.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>

#include "../PortableEnv.h"
#include "cajeta/compile/NativeLink.h"

#ifndef _WIN32
#  include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string compilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r;
    if (envRoot && *envRoot) r = envRoot;
    else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
    return r + "/build/src/cajeta";
}

int exitCodeOf(int systemStatus) {
#ifdef _WIN32
    return systemStatus;
#else
    return WIFEXITED(systemStatus) ? WEXITSTATUS(systemStatus) : -1;
#endif
}

std::string readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}

// A library project with one live @Native method against lib "tnat". Its
// home is a fresh directory, so ~/.cajeta/native on the box never leaks in.
struct NativeLibProject {
    fs::path root;
    std::shared_ptr<void> cleanup;

    static NativeLibProject create(const std::string& tag) {
        static std::mt19937_64 rng(std::random_device{}());
        auto dir = fs::temp_directory_path()
                 / ("cajeta_natkey_" + tag + "_" + std::to_string(rng()));
        fs::create_directories(dir);
        NativeLibProject p;
        p.root = dir;
        p.cleanup = std::shared_ptr<void>(nullptr, [dir](void*) {
            if (std::getenv("CAJETA_KEEP_TEMP")) return;
            std::error_code ec;
            fs::remove_all(dir, ec);
        });
        fs::create_directories(p.home());
        fs::path src = dir / "src" / "main" / "cajeta" / "t";
        fs::create_directories(src);
        std::ofstream(src / "Nat.cajeta")
            << "package t;\n"
               "public final class Nat {\n"
               "    @Native(symbol = \"t_nat_answer\", lib = \"tnat\")\n"
               "    private static int32 answer();\n"
               "    public static int32 get() { return Nat.answer(); }\n"
               "}\n";
        std::ofstream(dir / "cajeta.json")
            << "{\n"
               "  \"details\": { \"name\": \"t.nlib\", \"version\": \"0.1.0\",\n"
               "                 \"cajeta-lang-version\": \"1.0\" },\n"
               "  \"tasks\": { \"build\": { \"actions\": [\n"
               "      { \"action\": \"build\", \"flavor\": \"debug\", \"id\": \"art\" } ] } }\n"
               "}\n";
        return p;
    }

    fs::path home() const { return root / "home"; }
    fs::path envDir() const { return root / "natdir"; }
    fs::path projectNative() const { return root / "native"; }
    fs::path cja() const { return root / "build" / "archive" / "t.nlib-0.1.0.cja"; }
    fs::path outLog() const { return root / "out.log"; }

    // A static archive the bake can copy: the bake stores bytes, so a valid
    // ar header over a text payload is enough to tell versions apart.
    static void writeArchive(const fs::path& dir, const std::string& platform,
                             const std::string& payload) {
        fs::create_directories(dir / platform);
        std::ofstream a(dir / platform / "libtnat.a", std::ios::binary);
        a << "!<arch>\n" << payload;
    }

    static void removeArchive(const fs::path& dir, const std::string& platform) {
        std::error_code ec;
        fs::remove_all(dir / platform, ec);
    }

    // `cajeta build` with HOME at the project's fresh home and
    // CAJETA_NATIVE_PATH set to `nativePath` (unset when empty).
    int build(const std::string& nativePath = "") const {
        setenv("HOME", home().string().c_str(), 1);
        if (nativePath.empty()) unsetenv("CAJETA_NATIVE_PATH");
        else setenv("CAJETA_NATIVE_PATH", nativePath.c_str(), 1);
        std::string cmd = CAJETA_PORTABLE_CD + root.string() + " && "
            + compilerBinary() + " build > " + outLog().string() + " 2>&1";
        return exitCodeOf(std::system(cmd.c_str()));
    }

    std::string buildOutput() const { return readAll(outLog()); }
    bool hit() const { return contains(buildOutput(), "[cache] hit"); }

    // The archive baked for `platform`, read back out of the .cja; empty when
    // the .cja carries none.
    std::string baked(const std::string& platform) const {
        static std::mt19937_64 rng(std::random_device{}());
        fs::path out = root / ("extract_" + std::to_string(rng()));
        fs::create_directories(out);
        std::string cmd = compilerBinary() + " archive extract " + cja().string()
            + " -C " + out.string() + " > " CAJETA_PORTABLE_DEVNULL " 2>&1";
        std::system(cmd.c_str());
        std::error_code ec;
        for (auto& e : fs::recursive_directory_iterator(out, ec)) {
            if (!e.is_regular_file()) continue;
            if (e.path().filename() != "libtnat.a") continue;
            if (e.path().parent_path().filename() != platform) continue;
            return readAll(e.path());
        }
        return "";
    }
};

std::string hostPlatform() { return cajeta::hostNativePlatform(); }

} // namespace

// 1.1.1: an edited archive re-keys; the rebuilt .cja carries the new bytes;
// the original bytes land back on the first slot.
TEST(NativeCacheKey, anEditedArchiveRekeysAndIsBaked) {
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "compiler binary unavailable";
    auto p = NativeLibProject::create("edit");
    const std::string plat = hostPlatform();
    const std::string path = p.envDir().string();

    NativeLibProject::writeArchive(p.envDir(), plat, "shim version A");
    ASSERT_EQ(p.build(path), 0) << p.buildOutput();
    EXPECT_FALSE(p.hit()) << p.buildOutput();
    EXPECT_EQ(p.baked(plat), "!<arch>\nshim version A");

    ASSERT_EQ(p.build(path), 0) << p.buildOutput();
    EXPECT_TRUE(p.hit()) << "nothing changed, so the second build must hit:\n" << p.buildOutput();

    NativeLibProject::writeArchive(p.envDir(), plat, "shim version B, rebuilt");
    ASSERT_EQ(p.build(path), 0) << p.buildOutput();
    EXPECT_FALSE(p.hit()) << "a rebuilt shim must re-key the artifact:\n" << p.buildOutput();
    EXPECT_EQ(p.baked(plat), "!<arch>\nshim version B, rebuilt");

    ASSERT_EQ(p.build(path), 0) << p.buildOutput();
    EXPECT_TRUE(p.hit()) << p.buildOutput();

    NativeLibProject::writeArchive(p.envDir(), plat, "shim version A");
    ASSERT_EQ(p.build(path), 0) << p.buildOutput();
    EXPECT_TRUE(p.hit()) << "the original bytes should land on the first slot:\n" << p.buildOutput();
    EXPECT_EQ(p.baked(plat), "!<arch>\nshim version A");
}

// 1.1.2: the measured defect. A build with no path bakes nothing; the same
// build with the path misses and bakes the shim; unset again, it hits the
// shim-less slot.
TEST(NativeCacheKey, providingTheShimAfterACachedBuildMisses) {
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "compiler binary unavailable";
    auto p = NativeLibProject::create("provide");
    const std::string plat = hostPlatform();
    NativeLibProject::writeArchive(p.envDir(), plat, "the shim");

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_FALSE(p.hit()) << p.buildOutput();
    EXPECT_TRUE(contains(p.buildOutput(), "not bundled")) << p.buildOutput();
    EXPECT_EQ(p.baked(plat), "");

    ASSERT_EQ(p.build(p.envDir().string()), 0) << p.buildOutput();
    EXPECT_FALSE(p.hit()) << "a shim provided after the cached build must re-key:\n" << p.buildOutput();
    EXPECT_EQ(p.baked(plat), "!<arch>\nthe shim");

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_TRUE(p.hit()) << p.buildOutput();
    EXPECT_EQ(p.baked(plat), "");
}

// 1.1.3: a platform added under the search dir re-keys; removed, the earlier
// slot hits again.
TEST(NativeCacheKey, anAddedPlatformRekeys) {
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "compiler binary unavailable";
    auto p = NativeLibProject::create("platform");
    const std::string plat = hostPlatform();
    const std::string path = p.envDir().string();
    NativeLibProject::writeArchive(p.envDir(), plat, "host shim");

    ASSERT_EQ(p.build(path), 0) << p.buildOutput();
    ASSERT_EQ(p.build(path), 0) << p.buildOutput();
    ASSERT_TRUE(p.hit()) << p.buildOutput();

    NativeLibProject::writeArchive(p.envDir(), "other-arm64", "other shim");
    ASSERT_EQ(p.build(path), 0) << p.buildOutput();
    EXPECT_FALSE(p.hit()) << "a new platform must re-key:\n" << p.buildOutput();
    EXPECT_EQ(p.baked("other-arm64"), "!<arch>\nother shim");
    EXPECT_EQ(p.baked(plat), "!<arch>\nhost shim");

    NativeLibProject::removeArchive(p.envDir(), "other-arm64");
    ASSERT_EQ(p.build(path), 0) << p.buildOutput();
    EXPECT_TRUE(p.hit()) << p.buildOutput();
    EXPECT_EQ(p.baked("other-arm64"), "");
}

// 1.1.5 (spec 2.4, 3.2): the project's own native/ is searched with no
// environment variable set, and an edit there re-keys.
TEST(NativeCacheKey, theProjectNativeDirIsSearchedAndKeyed) {
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "compiler binary unavailable";
    auto p = NativeLibProject::create("projdir");
    const std::string plat = hostPlatform();
    NativeLibProject::writeArchive(p.projectNative(), plat, "project shim");

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_FALSE(contains(p.buildOutput(), "not bundled")) << p.buildOutput();
    EXPECT_EQ(p.baked(plat), "!<arch>\nproject shim");

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_TRUE(p.hit()) << p.buildOutput();

    NativeLibProject::writeArchive(p.projectNative(), plat, "project shim, rebuilt");
    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_FALSE(p.hit()) << "an edit under native/ must re-key:\n" << p.buildOutput();
    EXPECT_EQ(p.baked(plat), "!<arch>\nproject shim, rebuilt");
}

// 1.1.5, the other half: the bare compiler CLI, with no manifest and no
// --native-dir, does not search the project's native/; with --native-dir it
// does.
TEST(NativeCacheKey, theBareCompilerSearchesNativeDirOnlyWhenTold) {
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "compiler binary unavailable";
    auto p = NativeLibProject::create("bare");
    const std::string plat = hostPlatform();
    NativeLibProject::writeArchive(p.projectNative(), plat, "project shim");
    setenv("HOME", p.home().string().c_str(), 1);
    unsetenv("CAJETA_NATIVE_PATH");

    auto run = [&](const std::string& extra, const std::string& outDir) {
        fs::create_directories(p.root / outDir);
        std::string cmd = CAJETA_PORTABLE_CD + p.root.string() + " && "
            + compilerBinary() + " " + extra + " --emit=cja t.Nat::get src/main/cajeta "
            + outDir + " > " + (p.root / (outDir + ".log")).string() + " 2>&1";
        std::system(cmd.c_str());
        return readAll(p.root / (outDir + ".log"));
    };
    std::string without = run("", "bare-out");
    EXPECT_TRUE(contains(without, "not bundled"))
        << "without --native-dir the project native/ must not be searched:\n" << without;
    std::string with = run("--native-dir=" + p.projectNative().string(), "told-out");
    EXPECT_FALSE(contains(with, "not bundled")) << with;
    EXPECT_FALSE(contains(with, "nknown")) << "the flag must be accepted:\n" << with;
    EXPECT_TRUE(contains(with, "bundled") || contains(with, "native lib"))
        << "the bake should report the bundled lib:\n" << with;
}
