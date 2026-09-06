// `cajeta archive <sub>` — cli/ArchiveCommands.cpp (0/830 before this).
//
// The archive-emit suites exercise `cajeta build --emit=cja`, which lands in
// Compiler.cpp — the archive CLI's twelve subcommands had no consumer at
// all. Each test builds a fixture .cja in-process via CajetaArchive (the
// test links cajeta_lib), then drives the real built binary against it:
// list/cat/extract/info/deps/verify/diff read paths, repack/strip/merge
// write paths, sign/verify-sig with an openssl-generated ed25519 key, plus
// the usage and not-found refusals.

#include <gtest/gtest.h>
#include "cajeta/compile/CajetaArchive.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include "../PortableEnv.h"

namespace fs = std::filesystem;
using cajeta::CajetaArchive;
using cajeta::CajetaArchiveEntry;

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

std::vector<uint8_t> bytesOf(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

// One temp world per test: fixture archives, command output, extract dirs.
struct ArchiveWorld {
    fs::path root;
    ArchiveWorld() {
        static std::mt19937_64 rng(std::random_device{}());
        root = fs::temp_directory_path()
             / ("cajeta_arccli_" + std::to_string(rng()));
        fs::create_directories(root);
    }
    ~ArchiveWorld() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // A small two-entry .cja: one class-source entry, one resource.
    fs::path writeFixture(const std::string& name = "fix.cja",
                          const std::string& resourceBody = "hello=world\n") {
        CajetaArchive arc("com.example.fix", "1.2.3",
                          CajetaArchive::Kind::Cja);
        CajetaArchiveEntry src;
        src.name = "com/example/A.cajeta";
        src.kindTag = CajetaArchive::EntryKind::ClassSource;
        src.data = bytesOf("package com.example;\npublic class A {}\n");
        arc.addEntry(std::move(src));
        CajetaArchiveEntry res;
        res.name = "resources/app.properties";
        res.kindTag = CajetaArchive::EntryKind::Resource;
        res.data = bytesOf(resourceBody);
        arc.addEntry(std::move(res));
        fs::path p = root / name;
        arc.writeTo(p.string());
        return p;
    }

    fs::path outLog() const { return root / "out.log"; }

    int run(const std::string& archiveArgs) {
        std::string cmd = compilerBinary() + " archive " + archiveArgs
            + " > " + outLog().string() + " 2>&1";
        return exitCodeOf(std::system(cajeta_shell(cmd).c_str()));
    }
    std::string output() const {
        std::ifstream in(outLog(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
};

} // namespace

TEST(ArchiveCommandTests, noSubcommandPrintsUsage) {
    ArchiveWorld w;
    EXPECT_EQ(w.run(""), 1);
    EXPECT_NE(w.output().find("Usage: cajeta archive"), std::string::npos)
        << w.output();
}

TEST(ArchiveCommandTests, unknownSubcommandRefused) {
    ArchiveWorld w;
    EXPECT_EQ(w.run("frobnicate"), 1);
    EXPECT_NE(w.output().find("unknown subcommand: frobnicate"),
              std::string::npos) << w.output();
}

TEST(ArchiveCommandTests, listShowsEntriesTextAndJson) {
    ArchiveWorld w;
    fs::path cja = w.writeFixture();

    EXPECT_EQ(w.run("list " + cja.string()), 0);
    std::string out = w.output();
    EXPECT_NE(out.find("com/example/A.cajeta"), std::string::npos) << out;
    EXPECT_NE(out.find("resources/app.properties"), std::string::npos) << out;

    EXPECT_EQ(w.run("list --json " + cja.string()), 0);
    out = w.output();
    EXPECT_NE(out.find("\"entry_count\":2"), std::string::npos) << out;
    EXPECT_NE(out.find("\"kind\":"), std::string::npos) << out;

    // A path filter narrows the view.
    EXPECT_EQ(w.run("list --json " + cja.string() + " resources"), 0);
    EXPECT_NE(w.output().find("\"entry_count\":1"), std::string::npos)
        << w.output();
}

TEST(ArchiveCommandTests, catDumpsEntryBytesAndRefusesUnknown) {
    ArchiveWorld w;
    fs::path cja = w.writeFixture("c.cja", "the-resource-payload");
    EXPECT_EQ(w.run("cat " + cja.string() + " resources/app.properties"), 0);
    EXPECT_EQ(w.output(), "the-resource-payload");

    EXPECT_NE(w.run("cat " + cja.string() + " no/such/entry"), 0);
    EXPECT_NE(w.output().find("entry not found"), std::string::npos)
        << w.output();
}

TEST(ArchiveCommandTests, extractExplodesEntriesToDirectory) {
    ArchiveWorld w;
    fs::path cja = w.writeFixture();
    fs::path dir = w.root / "exploded";
    EXPECT_EQ(w.run("extract " + cja.string() + " -C " + dir.string()), 0);
    EXPECT_TRUE(fs::exists(dir / "com" / "example" / "A.cajeta"))
        << w.output();
    EXPECT_TRUE(fs::exists(dir / "resources" / "app.properties"))
        << w.output();
}

TEST(ArchiveCommandTests, infoPrintsManifestFields) {
    ArchiveWorld w;
    fs::path cja = w.writeFixture();
    EXPECT_EQ(w.run("info " + cja.string()), 0);
    std::string out = w.output();
    EXPECT_NE(out.find("com.example.fix"), std::string::npos) << out;
    EXPECT_NE(out.find("1.2.3"), std::string::npos) << out;

    EXPECT_EQ(w.run("info --json " + cja.string()), 0);
    EXPECT_NE(w.output().find("\"com.example.fix\""), std::string::npos)
        << w.output();
}

TEST(ArchiveCommandTests, depsOnPlainCjaIsEmpty) {
    ArchiveWorld w;
    fs::path cja = w.writeFixture();
    EXPECT_EQ(w.run("deps --json " + cja.string()), 0);
    EXPECT_EQ(w.output(), "[]\n");
}

TEST(ArchiveCommandTests, verifyPassesIntactStrictWantsProvenance) {
    ArchiveWorld w;
    fs::path cja = w.writeFixture();
    EXPECT_EQ(w.run("verify " + cja.string()), 0) << w.output();

    // --strict demands the release-provenance manifest fields
    // (build_timestamp, cajeta_lang_version); an API-built fixture has
    // neither, so strict correctly refuses it as non-canonical.
    EXPECT_NE(w.run("verify --strict " + cja.string()), 0);
    EXPECT_NE(w.output().find("build_timestamp"), std::string::npos)
        << w.output();

    EXPECT_NE(w.run("verify " + (w.root / "absent.cja").string()), 0);
}

TEST(ArchiveCommandTests, diffReportsIdenticalAndChangedEntries) {
    ArchiveWorld w;
    fs::path a = w.writeFixture("a.cja", "same-body\n");
    fs::path b = w.writeFixture("b.cja", "same-body\n");
    EXPECT_EQ(w.run("diff " + a.string() + " " + b.string()), 0)
        << w.output();

    fs::path c = w.writeFixture("c.cja", "different-body\n");
    int rc = w.run("diff " + a.string() + " " + c.string());
    EXPECT_NE(rc, 0) << w.output();
    EXPECT_NE(w.output().find("resources/app.properties"), std::string::npos)
        << w.output();
}

TEST(ArchiveCommandTests, repackRoundTripsWithExplicitZstdLevel) {
    ArchiveWorld w;
    fs::path in = w.writeFixture("in.cja", "repack-me\n");
    fs::path out = w.root / "out.cja";
    EXPECT_EQ(w.run("repack " + in.string() + " " + out.string()
                    + " --zstd=9"), 0) << w.output();
    ASSERT_TRUE(fs::exists(out));

    // The repacked archive reads back with identical content.
    auto back = CajetaArchive::readFrom(out.string());
    const auto* e = back.findEntry("resources/app.properties");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(std::string(e->data.begin(), e->data.end()), "repack-me\n");
}

TEST(ArchiveCommandTests, stripFiltersEntriesByGlob) {
    ArchiveWorld w;
    fs::path in = w.writeFixture("in.cja");
    fs::path out = w.root / "stripped.cja";
    EXPECT_EQ(w.run("strip " + in.string() + " " + out.string()
                    + " --exclude=resources/*"), 0) << w.output();
    auto back = CajetaArchive::readFrom(out.string());
    EXPECT_EQ(back.findEntry("resources/app.properties"), nullptr);
    EXPECT_NE(back.findEntry("com/example/A.cajeta"), nullptr);
}

TEST(ArchiveCommandTests, mergeCombinesArchives) {
    ArchiveWorld w;
    fs::path a = w.writeFixture("a.cja");

    CajetaArchive other("com.example.other", "0.1.0",
                        CajetaArchive::Kind::Cja);
    CajetaArchiveEntry extra;
    extra.name = "resources/extra.txt";
    extra.kindTag = CajetaArchive::EntryKind::Resource;
    extra.data = bytesOf("extra\n");
    other.addEntry(std::move(extra));
    fs::path b = w.root / "b.cja";
    other.writeTo(b.string());

    // Inputs carry different names/versions: merge refuses without an
    // explicit identity, then combines under the one given.
    fs::path out = w.root / "merged.cja";
    EXPECT_NE(w.run("merge " + out.string() + " " + a.string() + " "
                    + b.string()), 0);
    EXPECT_NE(w.output().find("inputs disagree on name"), std::string::npos)
        << w.output();
    EXPECT_EQ(w.run("merge --name=com.example.merged --version=9.9.9 "
                    + out.string() + " " + a.string() + " "
                    + b.string()), 0) << w.output();
    auto back = CajetaArchive::readFrom(out.string());
    EXPECT_NE(back.findEntry("com/example/A.cajeta"), nullptr);
    EXPECT_NE(back.findEntry("resources/extra.txt"), nullptr);
}

TEST(ArchiveCommandTests, signAndVerifySigRoundTrip) {
    ArchiveWorld w;
    // ed25519 keypair via openssl; skip cleanly on machines without it.
    fs::path key = w.root / "key.pem";
    fs::path pub = w.root / "pub.pem";
    if (std::system(("openssl genpkey -algorithm ed25519 -out "
                     + key.string() + " 2>" CAJETA_PORTABLE_DEVNULL "").c_str()) != 0) {
        GTEST_SKIP() << "openssl unavailable — signing arms untestable here";
    }
    ASSERT_EQ(std::system(("openssl pkey -in " + key.string()
                           + " -pubout -out " + pub.string()
                           + " 2>" CAJETA_PORTABLE_DEVNULL "").c_str()), 0);

    fs::path cja = w.writeFixture();
    fs::path sig = w.root / "fix.cja.sig";
    EXPECT_EQ(w.run("sign " + cja.string() + " --key " + key.string()
                    + " --out " + sig.string()), 0) << w.output();
    ASSERT_TRUE(fs::exists(sig)) << w.output();

    EXPECT_EQ(w.run("verify-sig " + cja.string() + " --pubkey "
                    + pub.string() + " --sig " + sig.string()), 0)
        << w.output();

    // A tampered archive must fail signature verification.
    fs::path tampered = w.writeFixture("fix2.cja", "tampered-body\n");
    EXPECT_NE(w.run("verify-sig " + tampered.string() + " --pubkey "
                    + pub.string() + " --sig " + sig.string()), 0)
        << w.output();
}

// ─── flag arms the happy paths above don't reach ───────────────────






// `-` reads the archive from stdin (the documented pipe convention).

TEST(ArchiveCommandTests, corruptArchiveIsReportedNotCrashed) {
    ArchiveWorld w;
    fs::path bad = w.root / "corrupt.cja";
    std::ofstream(bad, std::ios::binary) << "not an archive at all";
    for (const char* sub : {"list", "info", "deps", "verify"}) {
        EXPECT_NE(w.run(std::string(sub) + " " + bad.string()), 0)
            << sub << ": " << w.output();
    }
}
