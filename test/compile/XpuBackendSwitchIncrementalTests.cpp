// Switching device backends must RECOMPILE the @Kernel-bearing source.
//
// xpu-cache-discriminator-spec §3.1-§3.3, §5.2. Unit 1 made the key separate
// backends; this asserts the cache ACTS on it. The distinction is the whole
// point of §5.2: a hash test alone passes against a cache that computes a
// correct key and then ignores it, which is exactly the shape of the original
// defect — `[incremental] skip kernelprofile/KernelProfile.cajeta` on a build
// that had asked for a different accelerator.
//
// No device is needed (§2.1.3): `cpu` and `nvptx` device code both COMPILE
// without the hardware. The nvptx half needs ptxas on PATH or under CUDA_PATH;
// absent it, the kernel registration is skipped with a note and the manifest
// behaviour under test is unchanged, so the test still means what it says.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string switchCompilerBinary() {
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

int switchExit(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

void writeFileAt(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}

std::string readAllAt(const fs::path& p) {
    std::ifstream in(p);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// One source holding a @Kernel, plus the manifest machinery to build it twice.
struct KernelWorld {
    fs::path root, srcRoot, build, cacheDir, manifestPath, outLog;

    KernelWorld() {
        static std::mt19937_64 rng(std::random_device{}());
        root = fs::temp_directory_path()
             / ("cajeta_xpuswitch_" + std::to_string(rng()));
        srcRoot = root / "src";
        build = root / "build";
        cacheDir = root / "cache";
        manifestPath = root / "manifest.json";
        outLog = root / "out.log";
        fs::create_directories(build);
        fs::create_directories(cacheDir);
        writeFileAt(srcRoot / "kern" / "Kern.cajeta",
            "package kern;\n"
            "import cajeta.xpu.KernelBuffer;\n"
            "import cajeta.xpu.KernelThread;\n"
            "public final class Kern {\n"
            "    @Kernel\n"
            "    public static void scale(KernelBuffer<float32> y, float32 k,\n"
            "                             uint32 n) {\n"
            "        uint32 i = KernelThread.globalIdX();\n"
            "        if (i < n) { y[i] = y[i] * k; }\n"
            "    }\n"
            "    public static int32 run() { return 0; }\n"
            "}\n");
    }
    ~KernelWorld() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    static constexpr const char* kRel = "kern/Kern.cajeta";

    fs::path bcSlot() const { return cacheDir / "Kern.bc"; }
    fs::path obligationsSlot() const { return cacheDir / "Kern.obligations"; }

    void writeManifest(const std::string& discriminator, bool clean) const {
        std::stringstream m;
        m << "{\n  \"version\": \"cache-manifest-v1\",\n"
          << "  \"discriminator\": \"" << discriminator << "\",\n"
          << "  \"sources\": [\n"
          << "    { \"path\": \"" << kRel << "\", "
          << "\"clean\": " << (clean ? "true" : "false") << ", "
          << "\"bc\": \"" << bcSlot().generic_string() << "\", "
          << "\"obligations\": \"" << obligationsSlot().generic_string() << "\" }\n"
          << "  ]\n}\n";
        writeFileAt(manifestPath, m.str());
    }

    int compile(const std::string& backend) const {
        std::string cmd = switchCompilerBinary()
            + " --emit=obj --xpu-backend=" + backend
            + " --cache-manifest=" + manifestPath.string()
            + " kern.Kern::run " + srcRoot.string() + " " + build.string()
            + " > " + outLog.string() + " 2>&1";
        return switchExit(std::system(cmd.c_str()));
    }

    std::string output() const { return readAllAt(outLog); }

    static std::string parseDiscriminator(const std::string& out) {
        const std::string tag = "[incremental] discriminator ";
        auto pos = out.find(tag);
        if (pos == std::string::npos) return {};
        auto start = pos + tag.size();
        auto end = out.find_first_of(" \r\n", start);
        return out.substr(start, end - start);
    }

    bool skippedTheKernelSource() const {
        return output().find("[incremental] skip") != std::string::npos;
    }

    // Populate: empty discriminator means "adopt mine", all entries dirty.
    // Returns the discriminator the compiler reported for `backend`.
    std::string populate(const std::string& backend) const {
        writeManifest("", false);
        if (compile(backend) != 0) return {};
        return parseDiscriminator(output());
    }
};

bool haveCompiler() { return fs::exists(switchCompilerBinary()); }

}  // namespace

// 2.1.1 — the defect, end to end. Populate under cpu, mark the source clean,
// then ask for nvptx: the kernel source must NOT be skipped, or the artifact
// ships CPU kernels while claiming to be an NVIDIA build.
TEST(XpuBackendSwitchIncremental, switchingBackendRecompilesKernelSources) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    KernelWorld w;

    const std::string cpuKey = w.populate("cpu");
    ASSERT_FALSE(cpuKey.empty()) << "populate build failed:\n" << w.output();

    // The cache now claims the source is clean UNDER THE CPU KEY.
    w.writeManifest(cpuKey, /*clean=*/true);

    ASSERT_EQ(0, w.compile("nvptx")) << "nvptx build failed:\n" << w.output();
    EXPECT_FALSE(w.skippedTheKernelSource())
        << "a backend switch reused the previous backend's device objects:\n"
        << w.output();
}

// 2.1.2 — THE CONTROL. Without it, "recompile everything, always" passes
// 2.1.1 and silently destroys incremental builds (§3.2).
TEST(XpuBackendSwitchIncremental, sameBackendTwiceStillReusesTheCache) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    KernelWorld w;

    const std::string cpuKey = w.populate("cpu");
    ASSERT_FALSE(cpuKey.empty()) << "populate build failed:\n" << w.output();

    w.writeManifest(cpuKey, /*clean=*/true);

    ASSERT_EQ(0, w.compile("cpu")) << "second cpu build failed:\n" << w.output();
    EXPECT_TRUE(w.skippedTheKernelSource())
        << "the same backend twice must still reuse the cache:\n"
        << w.output();
}

// §4.1 — the reported discriminator is the one the objects are keyed under, so
// two tasks printing the same value genuinely share a key.
TEST(XpuBackendSwitchIncremental, theReportedKeyDiffersAcrossBackends) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    KernelWorld w;

    const std::string cpuKey = w.populate("cpu");
    ASSERT_FALSE(cpuKey.empty()) << "cpu populate failed:\n" << w.output();
    const std::string nvKey = w.populate("nvptx");
    ASSERT_FALSE(nvKey.empty()) << "nvptx populate failed:\n" << w.output();

    EXPECT_NE(cpuKey, nvKey)
        << "both backends reported the same cache key: " << cpuKey;
}
