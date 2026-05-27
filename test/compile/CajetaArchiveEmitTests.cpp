// End-to-end tests for --emit=cja / --emit=uber. Drives the
// in-tree cajeta compiler via fork+exec, points it at a tiny
// cajeta source tree, then inspects the resulting .cja file for
// the expected header / manifest / entry shape.
//
// --emit=cja: project-only library archive (no stdlib, no deps).
// --emit=uber: project + stdlib + transitively-referenced classpath
// deps nested under deps/<name>-<version>/. The unit-level
// CajetaArchiveTests pin the writer's byte-level output; these
// tests pin the compile-driver wiring.

#include <gtest/gtest.h>

#include "cajeta/compile/CajetaArchive.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());
}

uint32_t readU32LE(const std::vector<uint8_t>& bytes, size_t offset) {
    return  (uint32_t) bytes[offset]
        | (((uint32_t) bytes[offset + 1]) << 8)
        | (((uint32_t) bytes[offset + 2]) << 16)
        | (((uint32_t) bytes[offset + 3]) << 24);
}

uint64_t readU64LE(const std::vector<uint8_t>& bytes, size_t offset) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= ((uint64_t) bytes[offset + (size_t) i]) << (i * 8);
    }
    return v;
}

// Resolve the in-tree compiler binary from CAJETA_SOURCE_ROOT (set by
// the test runner). Falls back to the default build path when the env
// var isn't set.
std::string compilerPath() {
    const char* root = std::getenv("CAJETA_SOURCE_ROOT");
    if (!root) root = ".";
    return std::string(root) + "/build/src/cajeta";
}

// Lay out a one-file cajeta source tree under a fresh temp dir,
// returning (sourceRoot, buildRoot). Caller cleans up. Body is a
// minimal main that exits 0.
struct TmpProject {
    fs::path sourceRoot;
    fs::path buildRoot;
};

TmpProject makeTmpProject(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_archive_" + tag + "_" + std::to_string(rng()));
    auto src = base / "src" / "demo";
    auto build = base / "build";
    fs::create_directories(src);
    fs::create_directories(build);
    std::ofstream out(src / "Hello.cajeta");
    out << "package demo;\n"
        << "public final class Hello {\n"
        << "    public static int32 run() { return 0; }\n"
        << "}\n";
    return TmpProject{base / "src", build};
}

} // namespace

TEST(CajetaArchiveEmitTests, cjaModeWritesCjaWithMagicHeader) {
    auto proj = makeTmpProject("magic");
    std::string cmd =
        compilerPath()
        + " --emit=cja demo.Hello.run "
        + proj.sourceRoot.string() + " "
        + proj.buildRoot.string()
        + " > /dev/null 2>&1";
    int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0) << "compiler exited non-zero: " << cmd;

    auto cja = proj.buildRoot / "Hello.cja";
    ASSERT_TRUE(fs::exists(cja)) << "expected " << cja;
    // Magic + version are stable across compression modes; everything past
    // the header may be zstd-compressed (current writer default), so use
    // the reader for the deeper assertions.
    auto bytes = readBytes(cja.string());
    ASSERT_GE(bytes.size(), (size_t) 32);
    EXPECT_EQ(std::string((const char*) bytes.data(), 8), std::string("CAJETA01"));
    EXPECT_EQ(readU32LE(bytes, 8), 1u);

    fs::remove_all(proj.sourceRoot.parent_path());
}

TEST(CajetaArchiveEmitTests, cjaModeManifestSaysCja) {
    auto proj = makeTmpProject("cja_kind");
    std::string cmd =
        compilerPath()
        + " --emit=cja demo.Hello.run "
        + proj.sourceRoot.string() + " "
        + proj.buildRoot.string()
        + " > /dev/null 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0);

    auto arc = cajeta::CajetaArchive::readFrom(
        (proj.buildRoot / "Hello.cja").string());
    EXPECT_EQ(arc.getKind(), cajeta::CajetaArchive::Kind::Cja);
    EXPECT_EQ(arc.getName(), "demo.Hello");

    fs::remove_all(proj.sourceRoot.parent_path());
}

TEST(CajetaArchiveEmitTests, uberModeManifestSaysUber) {
    auto proj = makeTmpProject("uber");
    std::string cmd =
        compilerPath()
        + " --emit=uber demo.Hello.run "
        + proj.sourceRoot.string() + " "
        + proj.buildRoot.string()
        + " > /dev/null 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0);

    auto arc = cajeta::CajetaArchive::readFrom(
        (proj.buildRoot / "Hello.cja").string());
    EXPECT_EQ(arc.getKind(), cajeta::CajetaArchive::Kind::Uber);

    fs::remove_all(proj.sourceRoot.parent_path());
}

// --- --classpath ingestion + uber bundling --------------------------------

namespace {

// Lay out a tiny "dep" cajeta project with a stand-alone library class
// no user code references at build time — purely a payload for uber
// to bundle. Returns the resulting .cja path.
fs::path buildDepArchive(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_dep_" + tag + "_" + std::to_string(rng()));
    auto src = base / "src" / "deplib";
    auto build = base / "build";
    fs::create_directories(src);
    fs::create_directories(build);
    {
        std::ofstream out(src / "Util.cajeta");
        out << "package deplib;\n"
            << "public final class Util {\n"
            << "    public static int32 ten() { return 10; }\n"
            << "}\n";
        out.flush();
    }
    std::string cmd =
        compilerPath()
        + " --emit=cja deplib.Util.ten "
        + (base / "src").string() + " "
        + build.string()
        + " > /dev/null 2>&1";
    int rc = std::system(cmd.c_str());
    if (rc != 0) return {};
    auto cja = build / "Util.cja";
    if (!fs::exists(cja)) return {};
    return cja;
}

} // anonymous namespace

TEST(CajetaArchiveEmitTests, uberWithClasspathBundlesDepEntriesNoPrune) {
    // --prune-uber=off path: uber bundles every classpath entry
    // regardless of whether user code references it. The default
    // pruning-on path is covered separately below. Nested layout:
    // each dep's entries land under deps/<name>-<version>/<orig>.
    auto depCja = buildDepArchive("classpath");
    ASSERT_FALSE(depCja.empty()) << "buildDepArchive failed";
    ASSERT_TRUE(fs::exists(depCja));

    auto depArc = cajeta::CajetaArchive::readFrom(depCja.string());
    std::vector<std::string> depEntryNames;
    for (const auto& e : depArc.getEntries()) {
        depEntryNames.push_back(e.name);
    }
    ASSERT_FALSE(depEntryNames.empty());
    std::string depPrefix = "deps/"
        + depArc.getName() + "-" + depArc.getVersion() + "/";

    auto proj = makeTmpProject("uberCp");
    std::string cmd =
        compilerPath()
        + " --emit=uber --prune-uber=off --classpath=" + depCja.string()
        + " demo.Hello.run "
        + proj.sourceRoot.string() + " "
        + proj.buildRoot.string()
        + " > /dev/null 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0);

    auto uberCja = proj.buildRoot / "Hello.cja";
    auto uber = cajeta::CajetaArchive::readFrom(uberCja.string());

    for (const auto& depName : depEntryNames) {
        std::string nestedName = depPrefix + depName;
        bool found = false;
        for (const auto& uberEntry : uber.getEntries()) {
            if (uberEntry.name == nestedName) {
                found = true;
                EXPECT_EQ(uberEntry.originTag,
                          (uint8_t) cajeta::CajetaArchive::Origin::Dependency)
                    << nestedName << " not tagged Dependency";
                break;
            }
        }
        EXPECT_TRUE(found) << "uber missing dep entry: " << nestedName;
    }

    // Manifest deps array carries the dep summary.
    bool depFound = false;
    for (const auto& d : uber.getDeps()) {
        if (d.name == depArc.getName()) { depFound = true; break; }
    }
    EXPECT_TRUE(depFound) << "manifest deps array missing entry for "
                          << depArc.getName();

    fs::remove_all(proj.sourceRoot.parent_path());
    fs::remove_all(depCja.parent_path().parent_path());
}

TEST(CajetaArchiveEmitTests, uberDedupesSameNamedEntriesAcrossClasspathNoPrune) {
    // --prune-uber=off; two dep archives with the same manifest
    // name+version produce the same deps/<name>-<version>/ prefix,
    // so per-entry dedupe (first-archive-wins) keeps exactly one
    // copy at each nested path.
    auto depA = buildDepArchive("dupA");
    auto depB = buildDepArchive("dupB");
    ASSERT_FALSE(depA.empty());
    ASSERT_FALSE(depB.empty());

    auto a = cajeta::CajetaArchive::readFrom(depA.string());
    std::string depPrefix = "deps/" + a.getName() + "-" + a.getVersion() + "/";

    auto proj = makeTmpProject("uberDup");
    std::string cmd =
        compilerPath()
        + " --emit=uber --prune-uber=off --classpath=" + depA.string()
        + "," + depB.string()
        + " demo.Hello.run "
        + proj.sourceRoot.string() + " "
        + proj.buildRoot.string()
        + " > /dev/null 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0);

    auto uber = cajeta::CajetaArchive::readFrom(
        (proj.buildRoot / "Hello.cja").string());

    for (const auto& depEntry : a.getEntries()) {
        std::string nestedName = depPrefix + depEntry.name;
        int count = 0;
        for (const auto& ub : uber.getEntries()) {
            if (ub.name == nestedName) count++;
        }
        EXPECT_EQ(count, 1) << "name `" << nestedName
                            << "` appears " << count << "x in uber";
    }

    fs::remove_all(proj.sourceRoot.parent_path());
    fs::remove_all(depA.parent_path().parent_path());
    fs::remove_all(depB.parent_path().parent_path());
}

// --- Reachability-pruned uber (default behavior) --------------------------

TEST(CajetaArchiveEmitTests, uberDefaultPrunesUnreferencedDepEntries) {
    // User code doesn't reference deplib at all → uber should NOT
    // bundle anything from the dep's deps/<name>-<version>/ subtree
    // AND should not list the dep in the manifest deps array.
    auto depCja = buildDepArchive("prune_unref");
    ASSERT_FALSE(depCja.empty());

    auto depArc = cajeta::CajetaArchive::readFrom(depCja.string());
    std::string depPrefix = "deps/" + depArc.getName() + "-" + depArc.getVersion() + "/";

    auto proj = makeTmpProject("prune_unref");
    std::string cmd =
        compilerPath()
        + " --emit=uber --classpath=" + depCja.string()
        + " demo.Hello.run "
        + proj.sourceRoot.string() + " "
        + proj.buildRoot.string()
        + " > /dev/null 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0);

    auto uber = cajeta::CajetaArchive::readFrom(
        (proj.buildRoot / "Hello.cja").string());

    // The whole dep subtree must be absent. The user's
    // `demo/Hello.bc` and the stdlib bundle are present unconditionally.
    bool depSubtreeSeen = false;
    bool userHelloSeen = false;
    for (const auto& e : uber.getEntries()) {
        if (e.name.rfind(depPrefix, 0) == 0) depSubtreeSeen = true;
        if (e.name == "demo/Hello.bc")       userHelloSeen = true;
    }
    EXPECT_FALSE(depSubtreeSeen)
        << depPrefix << " subtree should have been pruned (unreferenced)";
    EXPECT_TRUE(userHelloSeen)
        << "demo/Hello.bc must always be included";
    // Dep dropped from the manifest deps array since nothing survived.
    EXPECT_TRUE(uber.getDeps().empty())
        << "manifest deps array should be empty when the only dep was pruned";

    fs::remove_all(proj.sourceRoot.parent_path());
    fs::remove_all(depCja.parent_path().parent_path());
}

TEST(CajetaArchiveEmitTests, uberDefaultKeepsReferencedDepEntries) {
    // User code DOES reference deplib.Util → compile-time classpath
    // ingestion resolves the import, user bitcode carries the dep
    // canonical, and the uber pruner keeps the dep entry. End-to-end
    // proof that classpath ingestion is wired through parse → codegen
    // → uber bundling.
    auto depCja = buildDepArchive("prune_kept");
    ASSERT_FALSE(depCja.empty());

    // Build a user project that explicitly calls deplib.Util.ten().
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_user_uses_dep_" + std::to_string(rng()));
    auto src = base / "src" / "demo";
    auto build = base / "build";
    fs::create_directories(src);
    fs::create_directories(build);
    {
        std::ofstream out(src / "Hello.cajeta");
        out << "package demo;\n"
            << "import deplib.Util;\n"
            << "public final class Hello {\n"
            << "    public static int32 run() {\n"
            << "        return Util.ten();\n"
            << "    }\n"
            << "}\n";
        out.flush();
    }
    std::string cmd =
        compilerPath()
        + " --emit=uber --classpath=" + depCja.string()
        + " demo.Hello.run "
        + (base / "src").string() + " "
        + build.string()
        + " > /dev/null 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0)
        << "compile failed; classpath ingestion should have resolved "
        << "deplib.Util — see cajeta-compile.log";

    auto uber = cajeta::CajetaArchive::readFrom((build / "Hello.cja").string());
    auto depArc = cajeta::CajetaArchive::readFrom(depCja.string());
    std::string depPrefix = "deps/" + depArc.getName() + "-" + depArc.getVersion() + "/";
    std::string depUtilNested = depPrefix + "deplib/Util.bc";

    // The pruner keeps the dep entry only when some included bitcode
    // references its canonical. With classpath ingestion live, the
    // user's bitcode carries `deplib.Util` mangled symbols.
    bool userBitcodeReferencesDep = false;
    bool depUtilSeen = false;
    for (const auto& e : uber.getEntries()) {
        if (e.name == "demo/Hello.bc") {
            const std::string needle = "deplib.Util";
            auto it = std::search(e.data.begin(), e.data.end(),
                needle.begin(), needle.end());
            userBitcodeReferencesDep = (it != e.data.end());
        }
        if (e.name == depUtilNested) depUtilSeen = true;
    }
    EXPECT_TRUE(userBitcodeReferencesDep)
        << "user bitcode should reference deplib.Util — classpath "
        << "ingestion must have resolved the import";
    EXPECT_TRUE(depUtilSeen)
        << depUtilNested << " should have been kept (user bitcode "
        << "references the dep canonical)";

    fs::remove_all(base);
    fs::remove_all(depCja.parent_path().parent_path());
}

TEST(CajetaArchiveEmitTests, cjaContainsProjectEntriesOnly) {
    // --emit=cja produces a project-only library archive — the
    // parsed-stdlib module is stripped, no deps are bundled. The
    // user's demo/Hello.bc is the sole entry; consumers must bring
    // their own stdlib via --classpath at compile time.
    auto proj = makeTmpProject("entries");
    std::string cmd =
        compilerPath()
        + " --emit=cja demo.Hello.run "
        + proj.sourceRoot.string() + " "
        + proj.buildRoot.string()
        + " > /dev/null 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0);

    auto arc = cajeta::CajetaArchive::readFrom(
        (proj.buildRoot / "Hello.cja").string());

    bool userHelloBcSeen = false;
    bool userHelloSrcSeen = false;
    bool stdlibSeen = false;
    for (const auto& e : arc.getEntries()) {
        if (e.name == "demo/Hello.bc")     userHelloBcSeen  = true;
        if (e.name == "demo/Hello.cajeta") userHelloSrcSeen = true;
        if (e.name.rfind("cajeta/", 0) == 0) stdlibSeen = true;
        // Every entry is either bitcode (.bc) or source (.cajeta) and
        // uses jar-style '/' separators.
        bool isBc  = e.name.size() >= 3
            && e.name.compare(e.name.size() - 3, 3, ".bc") == 0;
        bool isSrc = e.name.size() >= 7
            && e.name.compare(e.name.size() - 7, 7, ".cajeta") == 0;
        EXPECT_TRUE(isBc || isSrc) << "entry name = " << e.name;
    }
    EXPECT_TRUE(userHelloBcSeen) << "demo/Hello.bc must be present";
    EXPECT_TRUE(userHelloSrcSeen)
        << "demo/Hello.cajeta must be present (source-shipping for "
        << "downstream --classpath ingestion)";
    EXPECT_FALSE(stdlibSeen)
        << "--emit=cja must not bundle stdlib (cajeta.* entries)";
    EXPECT_TRUE(arc.getDeps().empty()) << "cja archives must not list deps";

    fs::remove_all(proj.sourceRoot.parent_path());
}

// --- Classpath ingestion ---------------------------------------------------
//
// The compiler must load + parse the source of every class shipped in
// each --classpath archive at compile-start, so user code can `import`
// those classes and the codegen path emits proper extern references.
// Tests below pin the behavior end-to-end.

namespace {

// Build a tiny user project that imports + calls deplib.Util.ten().
// Returns (sourceRoot, buildRoot). Caller cleans up.
TmpProject makeUserProjectThatCallsDep(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_classpath_user_" + tag + "_" + std::to_string(rng()));
    auto src = base / "src" / "demo";
    auto build = base / "build";
    fs::create_directories(src);
    fs::create_directories(build);
    std::ofstream out(src / "Hello.cajeta");
    out << "package demo;\n"
        << "import deplib.Util;\n"
        << "public final class Hello {\n"
        << "    public static int32 run() {\n"
        << "        return Util.ten();\n"
        << "    }\n"
        << "}\n";
    return TmpProject{base / "src", build};
}

} // anonymous namespace

TEST(ClasspathIngestionTests, cjaCarriesClassSourceEntries) {
    // For classpath ingestion to work, the writer must ship the original
    // .cajeta source bytes alongside each .bc. The reader needs them to
    // re-parse the dep's classes into the canonical-name registry.
    auto depCja = buildDepArchive("source_entries");
    ASSERT_FALSE(depCja.empty());

    auto dep = cajeta::CajetaArchive::readFrom(depCja.string());

    bool foundUtilSrc = false;
    for (const auto& e : dep.getEntries()) {
        if (e.name == "deplib/Util.cajeta"
            && e.kindTag == cajeta::CajetaArchive::EntryKind::ClassSource) {
            foundUtilSrc = true;
            // Spot-check the bytes — sanity, the source we packaged.
            std::string text((const char*) e.data.data(), e.data.size());
            EXPECT_NE(text.find("package deplib"), std::string::npos)
                << "ClassSource entry content doesn't look like the dep source";
            EXPECT_NE(text.find("class Util"), std::string::npos);
            break;
        }
    }
    EXPECT_TRUE(foundUtilSrc)
        << "deplib/Util.cajeta ClassSource entry missing from "
        << depCja.string()
        << " — writer must ship per-class source alongside bitcode";

    fs::remove_all(depCja.parent_path().parent_path());
}

TEST(ClasspathIngestionTests, userIrReferencesClasspathClassUnderEmitIr) {
    // Simplest end-to-end check. Compile a user project that imports +
    // calls deplib.Util.ten() with the dep on --classpath. With
    // classpath ingestion working: compile succeeds AND the resulting
    // user .ll text references the deplib.Util canonical.
    auto depCja = buildDepArchive("emit_ir_ingest");
    ASSERT_FALSE(depCja.empty());

    auto proj = makeUserProjectThatCallsDep("emit_ir");
    auto logPath = proj.buildRoot / "compile.log";
    std::string cmd =
        compilerPath()
        + " --emit=ir --classpath=" + depCja.string()
        + " demo.Hello.run "
        + proj.sourceRoot.string() + " "
        + proj.buildRoot.string()
        + " > " + logPath.string() + " 2>&1";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::ifstream logf(logPath);
        std::string log(
            (std::istreambuf_iterator<char>(logf)),
            std::istreambuf_iterator<char>());
        FAIL() << "compile failed (rc=" << rc << ") — classpath "
                  "ingestion should have resolved deplib.Util. log:\n"
               << log;
    }

    // Find the user IR. With --emit=ir the compiler writes one .ll per
    // module; demo.Hello goes to <build>/demo/Hello.ll.
    fs::path userIr = proj.buildRoot / "demo" / "Hello.ll";
    ASSERT_TRUE(fs::exists(userIr))
        << "expected user IR at " << userIr;

    std::ifstream irFile(userIr);
    std::string irText(
        (std::istreambuf_iterator<char>(irFile)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(irText.find("deplib.Util"), std::string::npos)
        << "user IR doesn't reference deplib.Util — resolution failed.\n"
        << "IR head:\n" << irText.substr(0, 800);

    fs::remove_all(proj.sourceRoot.parent_path());
    fs::remove_all(depCja.parent_path().parent_path());
}

TEST(ClasspathIngestionTests, classpathClassNotEmittedInUserCja) {
    // The classpath class is ingested for resolution only — its
    // bitcode/IR must not land in the user's project archive (that
    // would defeat the point of having it as a classpath dep).
    auto depCja = buildDepArchive("no_double_emit");
    ASSERT_FALSE(depCja.empty());

    auto proj = makeUserProjectThatCallsDep("no_double_emit");
    auto logPath = proj.buildRoot / "compile.log";
    std::string cmd =
        compilerPath()
        + " --emit=cja --classpath=" + depCja.string()
        + " demo.Hello.run "
        + proj.sourceRoot.string() + " "
        + proj.buildRoot.string()
        + " > " + logPath.string() + " 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0)
        << "compile failed; expected classpath ingestion to succeed";

    auto arc = cajeta::CajetaArchive::readFrom(
        (proj.buildRoot / "Hello.cja").string());

    bool depBcInUserCja = false;
    bool userBcPresent = false;
    for (const auto& e : arc.getEntries()) {
        if (e.name == "deplib/Util.bc") depBcInUserCja = true;
        if (e.name == "demo/Hello.bc")  userBcPresent  = true;
    }
    EXPECT_TRUE(userBcPresent) << "user code must be in the cja";
    EXPECT_FALSE(depBcInUserCja)
        << "classpath dep bitcode must NOT be in the user's --emit=cja "
        << "archive (it's a library form; deps stay external)";

    fs::remove_all(proj.sourceRoot.parent_path());
    fs::remove_all(depCja.parent_path().parent_path());
}
