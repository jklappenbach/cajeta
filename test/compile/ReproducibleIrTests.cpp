// Reproducible-build determinism for the embedded source-file name.
//
// Incremental compilation (docs/IncrementalCompilation.md,
// plan Phase 1) trusts that the same source + flags yields a
// byte-identical module. The module's *source file name* used to be
// the absolute on-disk path (CajetaModule ctor), which baked a
// machine-specific string into every emitted module and broke
// byte-identical IR across hosts / build roots.
//
// CajetaModule::remapSourcePath now scrubs that: it honors
// --debug-prefix-map=<from>=<to>, and falls back to a sourceRoot-
// relative path when no map is supplied. These tests pin both the
// pure mapping and the end-to-end effect through createModule.

#include <gtest/gtest.h>

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"

#include <llvm/IR/Module.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

// std::system runs via cmd.exe on Windows, where /dev/null is invalid.
#ifdef _WIN32
#  define CAJETA_DEVNULL "NUL"
#else
#  define CAJETA_DEVNULL "/dev/null"
#endif

using cajeta::Compiler;
using cajeta::CajetaModule;

namespace {

namespace fs = std::filesystem;

// Resolve the in-tree compiler binary (mirrors CajetaArchiveEmitTests'
// helper): CAJETA_SOURCE_ROOT env wins, else the CMake-baked default.
std::string compilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r;
    if (envRoot && *envRoot) {
        r = envRoot;
    } else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
#ifdef _WIN32
    if (r.size() >= 3 && r[0] == '/' && std::isalpha((unsigned char) r[1]) && r[2] == '/') {
        r = std::string(1, r[1]) + ":" + r.substr(2);
    }
    std::string p = r + "/build/src/cajeta.exe";
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
#else
    return r + "/build/src/cajeta";
#endif
}

std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

// Lay `package util; ... class Math` under <root>/util/Math.cajeta and
// return the file path. Content is identical regardless of root.
std::string writeSource(const fs::path& root) {
    fs::create_directories(root / "util");
    auto file = root / "util" / "Math.cajeta";
    std::ofstream out(file);
    out << "package util;\n"
           "public final class Math {\n"
           "    public static void main() {\n"
           "    }\n"
           "}\n";
    out.close();
    return file.string();
}

fs::path freshTempDir(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_repro_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

} // namespace

// ---- pure mapping ---------------------------------------------------

TEST(ReproducibleIr, DebugPrefixMapRewritesMatchingPrefix) {
    EXPECT_EQ(
        CajetaModule::remapSourcePath(
            "/home/dev/proj/util/Math.cajeta", "/home/dev/proj",
            "/home/dev/proj=cajeta:"),
        "cajeta:/util/Math.cajeta");
}

TEST(ReproducibleIr, SameSourceUnderTwoRootsMapsIdentically) {
    // The crux: two machines (or two build roots) with their own
    // <root>=cajeta: maps must produce the same embedded name.
    auto a = CajetaModule::remapSourcePath(
        "/builds/a/util/Math.cajeta", "/builds/a", "/builds/a=cajeta:");
    auto b = CajetaModule::remapSourcePath(
        "/srv/ci/checkout/util/Math.cajeta", "/srv/ci/checkout",
        "/srv/ci/checkout=cajeta:");
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, "cajeta:/util/Math.cajeta");
}

TEST(ReproducibleIr, NoMapFallsBackToRootRelative) {
    // Direct compiler invocation (no build tool, no map) is still
    // machine-independent via the sourceRoot-relative fallback.
    auto a = CajetaModule::remapSourcePath(
        "/builds/a/util/Math.cajeta", "/builds/a", "");
    auto b = CajetaModule::remapSourcePath(
        "/totally/different/root/util/Math.cajeta",
        "/totally/different/root", "");
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, "util/Math.cajeta");
}

TEST(ReproducibleIr, NormalizesWindowsSeparators) {
    EXPECT_EQ(
        CajetaModule::remapSourcePath(
            "C:\\builds\\a\\util\\Math.cajeta", "C:\\builds\\a",
            "C:/builds/a=cajeta:"),
        "cajeta:/util/Math.cajeta");
}

TEST(ReproducibleIr, UnrelatedPathPassesThroughNormalized) {
    // Neither the map prefix nor the root matches: return the path
    // (separator-normalized), never garbage.
    EXPECT_EQ(
        CajetaModule::remapSourcePath(
            "/elsewhere/util/Math.cajeta", "/builds/a", "/builds/a=cajeta:"),
        "/elsewhere/util/Math.cajeta");
}

// ---- end-to-end through createModule --------------------------------

TEST(ReproducibleIr, CreateModuleEmbedsSameNameAcrossRoots) {
    Compiler compiler;
    auto rootA = freshTempDir("rootA");
    auto rootB = freshTempDir("rootB");
    auto archive = freshTempDir("arch");
    auto fileA = writeSource(rootA);
    auto fileB = writeSource(rootB);

    auto mA = compiler.createModule(fileA, rootA.string(), archive.string());
    auto mB = compiler.createModule(fileB, rootB.string(), archive.string());

    // The embedded source_filename — the only path-dependent field for a
    // trivial module — must match despite the differing absolute roots.
    EXPECT_EQ(mA->getLlvmModule()->getSourceFileName(),
              mB->getLlvmModule()->getSourceFileName());
    // And it must not be either absolute path.
    EXPECT_NE(mA->getLlvmModule()->getSourceFileName(), fileA);
    EXPECT_NE(mB->getLlvmModule()->getSourceFileName(), fileB);
}

TEST(ReproducibleIr, CreateModuleHonorsDebugPrefixMap) {
    Compiler compiler;
    auto root = freshTempDir("mapRoot");
    auto archive = freshTempDir("mapArch");
    auto file = writeSource(root);
    compiler.getMutableFlags().debugPrefixMap = root.string() + "=cajeta:";

    auto m = compiler.createModule(file, root.string(), archive.string());
    EXPECT_EQ(m->getLlvmModule()->getSourceFileName(),
              "cajeta:/util/Math.cajeta");
}

// ---- end-to-end: byte-identical IR across build roots ---------------

namespace {

// Emit per-module .ll for a trivial source under `root` via the compiler
// binary, with --debug-prefix-map=<srcRoot>=cajeta: so the embedded name
// is root-independent. Returns the emitted Hello.ll path (empty on failure).
std::string emitIrUnderRoot(const fs::path& root) {
    auto src = root / "src" / "demo";
    auto build = root / "build";
    fs::create_directories(src);
    fs::create_directories(build);
    // The body deliberately owns a local String: its drop entry carries a
    // source tag (`.cajeta.src.*`), and the method emits a line-info frame
    // descriptor — both embed the source path and must round-trip through
    // --debug-prefix-map like source_filename does (docs-refactor 15.12.2).
    { std::ofstream out(src / "Hello.cajeta");
      out << "package demo;\n"
             "public final class Hello {\n"
             "    public static int32 run() {\n"
             "        String s = \"hey\" + 1;\n"
             "        return (int32) s.count();\n"
             "    }\n"
             "}\n"; }
    auto srcRoot = (root / "src");
    std::string cmd = compilerBinary()
        + " --emit=ir --debug-prefix-map=" + srcRoot.string() + "=cajeta: "
        + "demo.Hello.run " + srcRoot.string() + " " + build.string()
        + " > " CAJETA_DEVNULL " 2>&1";
    if (std::system(cmd.c_str()) != 0) return {};
    auto ll = build / "demo" / "Hello.ll";
    return fs::exists(ll) ? ll.string() : std::string{};
}

} // namespace

TEST(ReproducibleIr, EmittedIrIsByteIdenticalAcrossRoots) {
    auto rootA = freshTempDir("e2eA");
    auto rootB = freshTempDir("e2eB");
    auto llA = emitIrUnderRoot(rootA);
    auto llB = emitIrUnderRoot(rootB);
    if (llA.empty() || llB.empty()) {
        GTEST_SKIP() << "compiler binary unavailable or emit failed; "
                        "skipping the e2e byte-identical check";
    }
    auto a = readBytes(llA);
    auto b = readBytes(llB);
    ASSERT_FALSE(a.empty());
    // The whole emitted module is byte-identical despite the differing
    // absolute build roots — this is the "byte-identical IR" criterion,
    // and also catches any path embedding beyond source_filename.
    EXPECT_EQ(a, b);
}
