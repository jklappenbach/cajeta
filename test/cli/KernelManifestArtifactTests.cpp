// xpu-tile-manifest Unit 2 — the manifest is EMBEDDED in the artifact it
// describes and never modified after build (spec §12.4, §12.5, D7):
//
//   * an --emit=exe binary carries it in a named data section and a program
//     reads it through k.manifest() with the build directory gone;
//   * a .cja carries it as a KernelManifest member beside the class bitcode
//     that holds the device code, and a consumer built against that archive
//     reads the very same record at run time;
//   * a JIT run serves it from memory and writes no file;
//   * running the program — launching the kernel, reading the manifest —
//     leaves the artifact's bytes untouched, and reading opens no file.
//
// These drive the real compiler binary (build/src/cajeta), so they skip when
// it is not built. CPU backend throughout: GPU-free.
#include "gtest/gtest.h"

#include "../PortableEnv.h"
#include "cajeta/compile/CajetaArchive.h"

#include "llvm/Support/JSON.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string sourceRoot() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    if (envRoot && *envRoot) return envRoot;
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
    return CAJETA_SOURCE_ROOT_DEFAULT;
#else
    return ".";
#endif
}

std::string compilerBinary() { return sourceRoot() + "/build/src/cajeta"; }

int exitOf(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<fs::path> manifestCopiesUnder(const fs::path& dir) {
    std::vector<fs::path> out;
    if (!fs::exists(dir)) return out;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        std::string n = e.path().filename().string();
        if (n.size() > 14 && n.substr(n.size() - 14) == ".manifest.json") out.push_back(e.path());
    }
    return out;
}

// A program with one kernel that LAUNCHES it on the CPU backend (so a run
// "measures" the kernel) and then prints its manifest on one line.
const char* kAppMain =
    "package app;\n"
    "import cajeta.lang.Optional;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.lang.System;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelManifest;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class Main {\n"
    "    @Kernel\n"
    "    public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,\n"
    "                             float32 a, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = a * x[i] + y[i]; }\n"
    "    }\n"
    "    public static int32 main(String[] args) {\n"
    "        uint32 n = 1024;\n"
    "        float32[] hx = heap float32[n];\n"
    "        float32[] hy = heap float32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { hx[i] = 1.0f; hy[i] = 2.0f; }\n"
    "        KernelBuffer<float32> x = heap KernelBuffer<float32>(0, n);\n"
    "        KernelBuffer<float32> y = heap KernelBuffer<float32>(0, n);\n"
    "        x.allocate();\n"
    "        y.allocate();\n"
    "        x.upload(hx);\n"
    "        y.upload(hy);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        saxpy.launch(s, grid: [4], block: [256])(y, x, 2.0f, n);\n"
    "        s.sync();\n"
    "        y.download(hy);\n"
    "        x.free();\n"
    "        y.free();\n"
    "        float32 sum = 0.0f;\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { sum = sum + hy[i]; }\n"
    "        KernelManifest m = saxpy.manifest();\n"
    "        if (m == null) { System.stdout.println(\"manifest: none\"); return 2; }\n"
    "        Optional<int32> v = m.vgpr();\n"
    "        System.stdout.println(\"manifest: \" + m.kernel + \" \" + m.target + \" \"\n"
    "            + m.codeHash + \" \" + (v.isPresent() ? \"vgpr\" : \"novgpr\")\n"
    "            + \" sum=\" + sum);\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

// A library with one kernel and no host logic of its own.
const char* kLibK =
    "package lib;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class K {\n"
    "    @Kernel\n"
    "    public static void scale(KernelBuffer<float32> y, float32 a, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = a * y[i]; }\n"
    "    }\n"
    "    public static void main(String[] args) { }\n"
    "}\n";

// A consumer built against the library's archive: it reads the library
// kernel's manifest by name, without a kernel of its own.
const char* kConsumerMain =
    "package app2;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.lang.System;\n"
    "import cajeta.xpu.KernelManifest;\n"
    "public class Main {\n"
    "    public static int32 main(String[] args) {\n"
    "        KernelManifest m #= KernelManifest.of(\"scale\");\n"
    "        if (m == null) { System.stdout.println(\"manifest: none\"); return 2; }\n"
    "        System.stdout.println(\"hash: \" + m.codeHash + \" kernel: \" + m.kernel);\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

struct World {
    fs::path root;
    World() {
        static std::mt19937_64 rng(std::random_device{}());
        root = fs::temp_directory_path() / ("cajeta_manifest_art_" + std::to_string(rng()));
        fs::create_directories(root);
    }
    ~World() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void write(const fs::path& rel, const char* text) const {
        fs::create_directories((root / rel).parent_path());
        std::ofstream(root / rel) << text;
    }
    // Run `cmd` with the world root as cwd; returns the exit code, output in `out`.
    int sh(const std::string& cmd, std::string& out, const std::string& tag) const {
        fs::path log = root / (tag + ".log");
        std::string full = "cd \"" + root.string() + "\" && " + cmd + " > \"" + log.string() + "\" 2>&1";
        int rc = exitOf(std::system(cajeta_shell(full).c_str()));
        out = readFile(log);
        return rc;
    }
    int buildExe(const std::string& entry, const fs::path& src, const fs::path& out,
                 const fs::path& exe, const std::string& extra, std::string& log) const {
        return sh("\"" + compilerBinary() + "\" --emit=exe --xpu-backend=cpu " + extra
                  + " -o \"" + exe.string() + "\" " + entry + " \"" + src.string()
                  + "\" \"" + out.string() + "\"", log, "build-" + exe.filename().string());
    }
};

bool haveCompiler() { return fs::exists(compilerBinary()); }
bool haveTool(const char* name) {
    std::string probe = std::string("command -v ") + name + " > /dev/null 2>&1";
    return exitOf(std::system(probe.c_str())) == 0;
}

} // namespace

// 2.1.1 — the exe carries the manifest in a named data section, and the
// program reads it through k.manifest() after the build directory is gone.
TEST(KernelManifestArtifact, exeCarriesManifestSectionAndReadsItWithoutBuildDir) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    World w;
    w.write("src/app/Main.cajeta", kAppMain);
    fs::create_directories(w.root / "out");
    fs::create_directories(w.root / "run");
    std::string log;
    ASSERT_EQ(w.buildExe("app.Main.main", w.root / "src", w.root / "out",
                         w.root / "out" / "prog", "", log), 0) << log;
    ASSERT_TRUE(fs::exists(w.root / "out" / "prog")) << log;

    // The named section is in the binary (ELF: readelf lists it).
    if (haveTool("llvm-readelf")) {
        std::string sections;
        ASSERT_EQ(w.sh("llvm-readelf -S out/prog", sections, "readelf"), 0) << sections;
        EXPECT_NE(sections.find(".cajeta.manifest"), std::string::npos)
            << "no .cajeta.manifest section:\n" << sections;
        std::string dump;
        ASSERT_EQ(w.sh("llvm-readelf -p .cajeta.manifest out/prog", dump, "readelf-p"), 0);
        EXPECT_NE(dump.find("\"kernel\": \"app.Main.saxpy\""), std::string::npos) << dump;
        EXPECT_NE(dump.find("\"schemaVersion\": 1"), std::string::npos) << dump;
    }

    // Move the binary away and delete everything the build touched.
    fs::rename(w.root / "out" / "prog", w.root / "run" / "prog");
    fs::remove_all(w.root / "out");
    fs::remove_all(w.root / "src");
    ASSERT_FALSE(fs::exists(w.root / "out"));

    std::string run;
    ASSERT_EQ(w.sh("./run/prog", run, "run"), 0) << run;
    EXPECT_NE(run.find("manifest: app.Main.saxpy cpu/"), std::string::npos) << run;
    EXPECT_NE(run.find(" sha256:"), std::string::npos) << run;
    EXPECT_NE(run.find(" novgpr"), std::string::npos) << "CPU footprint must be absent:\n" << run;
    EXPECT_NE(run.find(" sum=4096"), std::string::npos) << "the launch ran:\n" << run;
}

// 2.1.2 — a .cja carries the manifest as a member beside the class bitcode; a
// consumer built against the archive reads the same record at run time.
TEST(KernelManifestArtifact, cjaCarriesManifestMemberAndConsumerReadsIt) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    World w;
    w.write("libsrc/lib/K.cajeta", kLibK);
    w.write("consumer/app2/Main.cajeta", kConsumerMain);
    fs::create_directories(w.root / "libout");
    fs::create_directories(w.root / "out2");
    std::string log;
    ASSERT_EQ(w.sh("\"" + compilerBinary() + "\" --emit=cja --xpu-backend=cpu -o libout/lib.cja "
                   "lib.K.main libsrc libout", log, "build-cja"), 0) << log;
    ASSERT_TRUE(fs::exists(w.root / "libout" / "lib.cja")) << log;

    // The member: kind KernelManifest, under xpu/manifests/, qualified name.
    cajeta::CajetaArchive arc = cajeta::CajetaArchive::readFrom((w.root / "libout" / "lib.cja").string());
    const cajeta::CajetaArchiveEntry* member = nullptr;
    bool sawBitcode = false;
    for (const auto& e : arc.getEntries()) {
        if (e.kindTag == cajeta::CajetaArchive::EntryKind::KernelManifest) member = &e;
        if (e.kindTag == cajeta::CajetaArchive::EntryKind::ClassBitcode
                && e.name.find("lib/K") != std::string::npos) sawBitcode = true;
    }
    ASSERT_NE(member, nullptr) << "no KernelManifest member in lib.cja";
    EXPECT_TRUE(sawBitcode) << "the class bitcode the manifest sits beside is missing";
    EXPECT_EQ(member->name.rfind("xpu/manifests/lib.K.scale.cpu-", 0), 0u) << member->name;
    EXPECT_NE(member->name.find(".manifest.json"), std::string::npos) << member->name;
    std::string json(member->data.begin(), member->data.end());
    auto doc = llvm::json::parse(json);
    ASSERT_TRUE(!!doc) << json;
    auto* id = doc->getAsObject()->getObject("identity");
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->getString("kernel").value_or(""), "lib.K.scale");
    std::string archivedHash = id->getString("codeHash").value_or("").str();
    EXPECT_EQ(archivedHash.rfind("sha256:", 0), 0u) << archivedHash;

    // The consumer, built against the archive, sees the same record.
    ASSERT_EQ(w.buildExe("app2.Main.main", w.root / "consumer", w.root / "out2",
                         w.root / "out2" / "cons", "--classpath=libout/lib.cja", log), 0) << log;
    std::string run;
    ASSERT_EQ(w.sh("./out2/cons", run, "run-cons"), 0) << run;
    EXPECT_NE(run.find("kernel: lib.K.scale"), std::string::npos) << run;
    EXPECT_NE(run.find("hash: " + archivedHash), std::string::npos)
        << "the consumer's runtime record differs from the archived one:\n" << run
        << "\narchived: " << archivedHash;
}

// 2.1.3 — a JIT run serves the manifest from memory and writes no file.
TEST(KernelManifestArtifact, jitRunServesManifestWithoutWritingAFile) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    World w;
    w.write("src/app/Main.cajeta", kAppMain);
    std::string run;
    ASSERT_EQ(w.sh("\"" + compilerBinary() + "\" jit-run --xpu-backend=cpu src app.Main.main",
                   run, "jit-run"), 0) << run;
    EXPECT_NE(run.find("manifest: app.Main.saxpy cpu/"), std::string::npos) << run;
    EXPECT_NE(run.find(" sum=4096"), std::string::npos) << run;
    auto copies = manifestCopiesUnder(w.root);
    EXPECT_TRUE(copies.empty()) << "a JIT run wrote " << copies.size()
                                << " manifest file(s); first: " << copies.front();
}

// 2.1.4 — the artifact's bytes are identical before and after a run that
// launched the kernel and read its manifest (nothing writes back).
TEST(KernelManifestArtifact, artifactBytesUnchangedByARun) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    World w;
    w.write("src/app/Main.cajeta", kAppMain);
    fs::create_directories(w.root / "out");
    std::string log;
    ASSERT_EQ(w.buildExe("app.Main.main", w.root / "src", w.root / "out",
                         w.root / "out" / "prog", "", log), 0) << log;
    auto copies = manifestCopiesUnder(w.root / "out");
    ASSERT_EQ(copies.size(), 1u) << log;
    std::string exeBefore = readFile(w.root / "out" / "prog");
    std::string jsonBefore = readFile(copies[0]);
    ASSERT_FALSE(exeBefore.empty());
    ASSERT_FALSE(jsonBefore.empty());

    std::string run;
    ASSERT_EQ(w.sh("./out/prog", run, "run"), 0) << run;
    ASSERT_NE(run.find(" sum=4096"), std::string::npos) << run;

    EXPECT_EQ(readFile(w.root / "out" / "prog"), exeBefore) << "the binary changed";
    EXPECT_EQ(readFile(copies[0]), jsonBefore) << "the JSON copy changed";
    EXPECT_EQ(manifestCopiesUnder(w.root).size(), 1u) << "the run wrote another manifest file";
    // And the embedded record IS the JSON copy, byte for byte.
    EXPECT_NE(exeBefore.find(jsonBefore), std::string::npos)
        << "the JSON copy's bytes are not embedded verbatim in the binary";
}

// 2.3.1 — reading a manifest performs no filesystem access: under strace the
// program opens no manifest file (it opens its own shared libraries and the
// like, which is why the assertion is on the manifest path, not on zero opens).
TEST(KernelManifestArtifact, readingAManifestOpensNoFile) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    if (!haveTool("strace")) GTEST_SKIP() << "strace not installed";
    World w;
    w.write("src/app/Main.cajeta", kAppMain);
    fs::create_directories(w.root / "out");
    std::string log;
    ASSERT_EQ(w.buildExe("app.Main.main", w.root / "src", w.root / "out",
                         w.root / "out" / "prog", "", log), 0) << log;
    std::string run;
    int rc = w.sh("strace -f -e trace=openat,open,stat,newfstatat,readlink -o strace.log ./out/prog",
                  run, "strace-run");
    ASSERT_EQ(rc, 0) << run;
    ASSERT_NE(run.find("manifest: app.Main.saxpy"), std::string::npos) << run;
    std::string trace = readFile(w.root / "strace.log");
    ASSERT_FALSE(trace.empty());
    EXPECT_EQ(trace.find("manifest"), std::string::npos)
        << "the program touched a manifest path on disk:\n" << trace;
}
