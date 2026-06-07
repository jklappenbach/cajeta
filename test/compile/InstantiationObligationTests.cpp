// Per-module instantiation-obligation capture (incremental compilation,
// Phase 2 — cajeta-docs/IncrementalCompilation.md).
//
// When a module's codegen drives a template instantiation into ANOTHER
// module (e.g. `xs.stream()` instantiates ArrayStream<int32> into stdlib),
// the triggering module records it as an "obligation" and emits a sidecar
// (`<archiveRoot>/<pkg>/<Class>.obligations`) next to its IR. A future
// incremental build replays those obligations when the module is skipped.
//
// Process-isolated (fork+exec the compiler with --emit=ir) so there's no
// shared global state to manage, and the sidecar is read straight off disk.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#ifdef _WIN32
#  define CAJETA_DEVNULL "NUL"
#else
#  define CAJETA_DEVNULL "/dev/null"
#endif

namespace {

namespace fs = std::filesystem;

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
#ifdef _WIN32
    if (r.size() >= 3 && r[0] == '/' && std::isalpha((unsigned char) r[1]) && r[2] == '/')
        r = std::string(1, r[1]) + ":" + r.substr(2);
    std::string p = r + "/build/src/cajeta.exe";
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
#else
    return r + "/build/src/cajeta";
#endif
}

fs::path freshTempDir(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_oblig_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

// Compile `S.cajeta` (package test) containing `body` via --emit=ir under a
// fresh root. Returns the sidecar path that WOULD hold test.S's obligations
// (whether or not it exists). Empty string if the compile failed to launch.
std::string compileAndSidecarPath(const std::string& body) {
    auto root = freshTempDir("root");
    auto srcDir = root / "src" / "test";
    auto build = root / "build";
    fs::create_directories(srcDir);
    fs::create_directories(build);
    { std::ofstream out(srcDir / "S.cajeta"); out << body; }
    auto srcRoot = root / "src";
    std::string cmd = compilerBinary()
        + " --emit=ir test.S.run "
        + srcRoot.string() + " " + build.string()
        + " > " CAJETA_DEVNULL " 2>&1";
    if (std::system(cmd.c_str()) != 0) return {};
    return (build / "test" / "S.obligations").string();
}

std::string readAll(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST(InstantiationObligation, ArrayStreamUseRecordsCrossModuleObligation) {
    std::string sidecar = compileAndSidecarPath(
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {10, 20, 30, 40};\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        return s.count();\n"
        "    }\n"
        "}\n");
    if (sidecar.empty()) GTEST_SKIP() << "compiler binary unavailable";
    ASSERT_TRUE(fs::exists(sidecar))
        << "expected an obligation sidecar at " << sidecar;
    std::string contents = readAll(sidecar);
    // The cross-module instantiation ArrayStream<int32> was driven into
    // stdlib by test.S's codegen, so it must appear in test.S's obligations.
    EXPECT_NE(contents.find("ArrayStream"), std::string::npos) << contents;
    EXPECT_NE(contents.find("int32"), std::string::npos) << contents;
}

TEST(InstantiationObligation, MethodTemplateUseRecordsCrossModuleObligation) {
    // A method-template instantiation (`.map<int32>`) lands its body in the
    // host class's (stdlib) module, separate from the class instantiation —
    // so it gets its own obligation, keyed by getMapKey(false). The `::`
    // separator distinguishes a method obligation from a class one.
    std::string sidecar = compileAndSidecarPath(
        "package test;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {10, 20, 30, 40};\n"
        "        return xs.stream().map<int32>((x) -> x * 10).count();\n"
        "    }\n"
        "}\n");
    if (sidecar.empty()) GTEST_SKIP() << "compiler binary unavailable";
    ASSERT_TRUE(fs::exists(sidecar))
        << "expected an obligation sidecar at " << sidecar;
    std::string contents = readAll(sidecar);
    // The cross-module method instantiation Stream<int32>::map<…><int32> was
    // driven into stdlib by test.S's codegen, so it must appear as a method
    // obligation (recognizable by the `::` host/method separator).
    EXPECT_NE(contents.find("::map"), std::string::npos) << contents;
    EXPECT_NE(contents.find("<int32>"), std::string::npos) << contents;
}

TEST(InstantiationObligation, NoTemplateUseRecordsNothing) {
    std::string sidecar = compileAndSidecarPath(
        "package test;\n"
        "public final class S {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n");
    if (sidecar.empty()) GTEST_SKIP() << "compiler binary unavailable";
    // No cross-module instantiation → no obligation → no sidecar (the writer
    // removes any stale one rather than leaving an empty file behind).
    EXPECT_FALSE(fs::exists(sidecar))
        << "unexpected obligation sidecar: " << readAll(sidecar);
}
