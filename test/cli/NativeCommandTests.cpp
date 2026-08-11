// `cajeta fetch` / `cajeta vendor` — cli/NativeCommands.cpp, the thin CLI
// over NativeProvision (whose core has its own in-process suite). Local
// files serve as sources; the cache root follows HOME, so everything runs
// hermetically in a temp world.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

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

struct NativeWorld {
    fs::path root;
    NativeWorld() {
        static std::mt19937_64 rng(std::random_device{}());
        root = fs::temp_directory_path()
             / ("cajeta_native_" + std::to_string(rng()));
        fs::create_directories(root / "home");
        fs::create_directories(root / "proj");
    }
    ~NativeWorld() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    fs::path outLog() const { return root / "out.log"; }

    fs::path writeArtifact(const std::string& name,
                           const std::string& body) const {
        fs::path p = root / name;
        std::ofstream(p, std::ios::binary) << body;
        return p;
    }

    int run(const std::string& args) {
        std::string cmd = "cd " + (root / "proj").string()
            + " && HOME=" + (root / "home").string()
            + " " + compilerBinary() + " " + args
            + " > " + outLog().string() + " 2>&1";
        return exitCodeOf(std::system(cmd.c_str()));
    }
    std::string output() const {
        std::ifstream in(outLog());
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    fs::path cacheRoot() const { return root / "home" / ".cajeta" / "native"; }
};

// sha256 of a small file via the system tool (available wherever the
// battery runs; sha256sum prints "<hex>  <file>").
std::string sha256Of(const fs::path& p) {
    std::string cmd = "sha256sum " + p.string();
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return "";
    char buf[128] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    pclose(f);
    std::string s(buf, n);
    auto sp = s.find(' ');
    return sp == std::string::npos ? "" : s.substr(0, sp);
}

} // namespace

TEST(NativeCommandTests, fetchUsageWhenArgsMissing) {
    NativeWorld w;
    EXPECT_EQ(w.run("fetch"), 2);
    EXPECT_NE(w.output().find("usage: cajeta fetch"), std::string::npos)
        << w.output();
}

TEST(NativeCommandTests, fetchCachesLocalArtifactUnderHome) {
    NativeWorld w;
    fs::path art = w.writeArtifact("libdemo.a", "DEMO-BYTES");
    EXPECT_EQ(w.run("fetch demo 1.0.0 linux-x64 " + art.string()), 0)
        << w.output();
    EXPECT_NE(w.output().find("fetched demo 1.0.0"), std::string::npos)
        << w.output();
    EXPECT_TRUE(fs::exists(
        w.cacheRoot() / "demo" / "1.0.0" / "linux-x64" / "libdemo.a"))
        << w.output();
}

TEST(NativeCommandTests, fetchVerifiesSha256BothWays) {
    NativeWorld w;
    fs::path art = w.writeArtifact("libsha.a", "SHA-CHECKED");
    std::string good = sha256Of(art);
    ASSERT_FALSE(good.empty());

    EXPECT_EQ(w.run("fetch shalib 2.0.0 linux-x64 " + art.string() + " "
                    + good), 0)
        << w.output();

    std::string bad(64, '0');
    EXPECT_EQ(w.run("fetch shalib 2.0.1 linux-x64 " + art.string() + " "
                    + bad), 1);
    EXPECT_NE(w.output().find("cajeta fetch:"), std::string::npos)
        << w.output();
}

TEST(NativeCommandTests, fetchMissingSourceReported) {
    NativeWorld w;
    EXPECT_EQ(w.run("fetch ghost 1.0.0 linux-x64 /no/such/file.a"), 1);
    EXPECT_NE(w.output().find("source not found"), std::string::npos)
        << w.output();
}

TEST(NativeCommandTests, vendorUsageWhenArgsMissing) {
    NativeWorld w;
    EXPECT_EQ(w.run("vendor"), 2);
    EXPECT_NE(w.output().find("usage: cajeta vendor"), std::string::npos)
        << w.output();
}

TEST(NativeCommandTests, vendorMaterializesIntoProjectNativeDir) {
    NativeWorld w;
    fs::path art = w.writeArtifact("libvend.a", "VENDORED");
    EXPECT_EQ(w.run("vendor linux-x64 " + art.string()), 0) << w.output();
    EXPECT_NE(w.output().find("vendored ->"), std::string::npos)
        << w.output();
    EXPECT_TRUE(fs::exists(
        w.root / "proj" / "native" / "linux-x64" / "libvend.a"))
        << w.output();

    // Explicit project-native-dir override.
    EXPECT_EQ(w.run("vendor linux-x64 " + art.string() + " thirdparty"), 0)
        << w.output();
    EXPECT_TRUE(fs::exists(
        w.root / "proj" / "thirdparty" / "linux-x64" / "libvend.a"))
        << w.output();
}
