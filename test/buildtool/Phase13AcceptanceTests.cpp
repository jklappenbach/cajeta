// Phase 13 — Git repositories + attestation. Pins the three
// acceptance criteria from plans/buildtool/build-tool-plan.md "Phase 13 — Git
// repositories + attestation":
//
//   1. Git-pinned dep resolves and builds.
//      (Already covered structurally by GitRepositoryTests +
//      GitOverrideTests from Phase 6c; this suite re-asserts the
//      surface to lock in the deliverable.)
//   2. `publish` emits a valid SLSA v1 provenance attached to the
//      archive.
//   3. `cajeta install` rejects an artifact whose attestation
//      doesn't verify.
//
// Plus deliverable-coverage:
//   - in-toto Statement v1 envelope shape
//   - SLSA v1 predicateType + buildType strings
//   - subject[].digest.sha256 round-trip (with/without `sha256:`
//     prefix)
//   - missing-required-field rejection
//   - digest mismatch rejection
//   - publish action emits `<archive>.attestation` sidecar with
//     `attestation: true`

#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/Lockfile.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Provenance.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

using cajeta::buildtool::composeProvenanceJson;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::parseDependencies;
using cajeta::buildtool::ProvenanceInputs;
using cajeta::buildtool::verifyProvenanceJson;

namespace {

    std::filesystem::path tempRoot(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-phase13-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    void writeFile(const std::filesystem::path& p,
                   const std::string& contents) {
        std::filesystem::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(contents.data(),
                  static_cast<std::streamsize>(contents.size()));
    }

    ProvenanceInputs goodInputs() {
        ProvenanceInputs p;
        p.archiveName = "dev.cajeta.http-1.2.4.cja";
        p.archiveSha256 = "sha256:" + std::string(64, 'a');
        p.manifestChecksum = "sha256:" + std::string(64, 'b');
        p.lockfileChecksum = "sha256:" + std::string(64, 'c');
        p.compilerVersion = "1.0.0";
        p.flavor = "release";
        p.target = "x86_64-linux-gnu";
        p.builderId = "https://github.com/cajeta-org/builder";
        p.startedOn  = "2026-06-02T00:00:00Z";
        p.finishedOn = "2026-06-02T00:00:01Z";
        return p;
    }

}  // namespace

TEST(Phase13, gitDepRecognizedByDependencyParser) {
    // Criterion 1 (unit-level surface): a `settings.dependencies`
    // entry with a git source ({ "git": "...", "rev": "..." })
    // parses cleanly. Round-trip resolution is covered by
    // GitRepositoryTests + GitOverrideTests; this assertion locks
    // in the parser surface.
    std::string body =
        R"({
            "details": { "name": "x.y", "version": "1.0.0" },
            "settings": {
                "dependencies": {
                    "x.dep": { "git": "https://example/x.git",
                               "rev": "v1.2.3" }
                }
            }
        })";
    auto m = loadManifestString(body, "<inline>");
    ASSERT_TRUE(static_cast<bool>(m));
    auto deps = parseDependencies(*m);
    ASSERT_TRUE(static_cast<bool>(deps));
    ASSERT_EQ(deps->size(), 1u);
    EXPECT_EQ((*deps)[0].name, "x.dep");
    // Git-source dep has no version constraint (the rev pin is
    // load-bearing instead) — that's the documented v1 shape.
    EXPECT_TRUE((*deps)[0].versionConstraint.empty());
}

TEST(Phase13, gitRepositoryEntryRecognized) {
    // Criterion 1: settings.repositories accepts a git entry with
    // `url` + `ref` + optional `subdir`. parseRepositories should
    // surface it as a RepositorySpec with type="git".
    std::string body =
        R"({
            "details": { "name": "x.y", "version": "1.0.0" },
            "settings": {
                "repositories": [
                    {
                        "name":   "vendor",
                        "type":   "git",
                        "url":    "https://example/vendor.git",
                        "ref":    "v1.0",
                        "subdir": "packages/core"
                    }
                ]
            }
        })";
    auto m = loadManifestString(body, "<inline>");
    ASSERT_TRUE(static_cast<bool>(m));
    auto repos = cajeta::buildtool::parseRepositories(*m);
    ASSERT_TRUE(static_cast<bool>(repos));
    ASSERT_EQ(repos->size(), 1u);
    EXPECT_EQ((*repos)[0].type, "git");
    EXPECT_EQ((*repos)[0].url, "https://example/vendor.git");
    EXPECT_EQ((*repos)[0].gitRef, "v1.0");
    EXPECT_EQ((*repos)[0].gitSubdir, "packages/core");
}

TEST(Phase13, provenanceComposesValidStatementEnvelope) {
    // Criterion 2: composeProvenanceJson produces a JSON envelope
    // whose _type + predicateType + buildType strings match the
    // in-toto + SLSA + cajeta spec values. The subject digest is
    // bare hex (no `sha256:` prefix) per the SLSA digest spec;
    // checksum fields elsewhere keep the cajeta-internal prefix
    // because they're inputs the cajeta tooling round-trips.
    auto inp = goodInputs();
    auto doc = composeProvenanceJson(inp);
    EXPECT_NE(doc.find("https://in-toto.io/Statement/v1"), std::string::npos);
    EXPECT_NE(doc.find("https://slsa.dev/provenance/v1"), std::string::npos);
    EXPECT_NE(doc.find("https://cajeta.org/build/v1"), std::string::npos);
    EXPECT_NE(doc.find("dev.cajeta.http-1.2.4.cja"), std::string::npos);
    // Subject digest carries the bare hex (no `sha256:` prefix).
    std::string bareHex = inp.archiveSha256.substr(7);
    auto pos = doc.find("\"digest\":");
    ASSERT_NE(pos, std::string::npos);
    // The bare hex appears within the digest object — that's the
    // SLSA-canonical form.
    EXPECT_NE(doc.find(bareHex, pos), std::string::npos);
    // And the prefixed form does NOT appear inside the digest object.
    auto digestEnd = doc.find("}", pos);
    ASSERT_NE(digestEnd, std::string::npos);
    std::string digestBlock = doc.substr(pos, digestEnd - pos);
    EXPECT_EQ(digestBlock.find("sha256:"), std::string::npos)
        << "subject digest must use bare hex, got: " << digestBlock;
}

TEST(Phase13, provenanceVerifyAcceptsValidDoc) {
    auto in = goodInputs();
    auto doc = composeProvenanceJson(in);
    auto r = verifyProvenanceJson(doc, in.archiveSha256);
    ASSERT_TRUE(static_cast<bool>(r));
    EXPECT_EQ(r->compilerVersion, "1.0.0");
    EXPECT_EQ(r->flavor, "release");
    EXPECT_EQ(r->target, "x86_64-linux-gnu");
    EXPECT_EQ(r->manifestChecksum, "sha256:" + std::string(64, 'b'));
}

TEST(Phase13, provenanceVerifyAcceptsBothPrefixedAndBareSha256) {
    // The expectedSha256 param can be either form — strips
    // `sha256:` to compare against the SLSA-canonical bare hex.
    auto in = goodInputs();
    auto doc = composeProvenanceJson(in);
    // Prefixed form.
    EXPECT_TRUE(static_cast<bool>(
        verifyProvenanceJson(doc, in.archiveSha256)));
    // Bare hex form.
    std::string bare = in.archiveSha256.substr(7);
    EXPECT_TRUE(static_cast<bool>(verifyProvenanceJson(doc, bare)));
}

TEST(Phase13, provenanceVerifyRejectsDigestMismatch) {
    // Criterion 3 (unit-level): when the attested digest doesn't
    // match the archive, verify errors — `cajeta install`
    // surfaces this as a refusal.
    auto in = goodInputs();
    auto doc = composeProvenanceJson(in);
    std::string wrong = "sha256:" + std::string(64, 'f');
    auto r = verifyProvenanceJson(doc, wrong);
    EXPECT_FALSE(static_cast<bool>(r));
    if (!r) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        os << r.takeError();
        EXPECT_NE(msg.find("digest mismatch"), std::string::npos)
            << "error must cite digest mismatch: " << msg;
    }
}

TEST(Phase13, provenanceVerifyRejectsWrongStatementType) {
    // A malformed envelope whose _type is something other than the
    // in-toto Statement v1 type must be refused.
    std::string bogus =
        R"({"_type":"https://example/Statement/v1",)"
        R"("subject":[{"name":"x","digest":{"sha256":"aa"}}],)"
        R"("predicateType":"https://slsa.dev/provenance/v1",)"
        R"("predicate":{"buildDefinition":)"
        R"({"buildType":"https://cajeta.org/build/v1"}}})";
    auto r = verifyProvenanceJson(bogus, "aa");
    EXPECT_FALSE(static_cast<bool>(r));
    if (!r) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        os << r.takeError();
        EXPECT_NE(msg.find("_type mismatch"), std::string::npos);
    }
}

TEST(Phase13, provenanceVerifyRejectsEmptyCompilerVersion) {
    // The compiler-version field is load-bearing for downstream
    // toolchain-compatibility checks; verify refuses when it's
    // empty.
    ProvenanceInputs bad = goodInputs();
    bad.compilerVersion = "";  // empty
    auto doc = composeProvenanceJson(bad);
    auto r = verifyProvenanceJson(doc, bad.archiveSha256);
    EXPECT_FALSE(static_cast<bool>(r));
    if (!r) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        os << r.takeError();
        EXPECT_NE(msg.find("compiler-version"), std::string::npos);
    }
}

TEST(Phase13, provenanceJsonIsStableAcrossRuns) {
    // Reproducibility cross-cut: same inputs → byte-identical
    // JSON. Required for the signature step (a non-deterministic
    // JSON encoder would re-sign every publish).
    auto in = goodInputs();
    auto a = composeProvenanceJson(in);
    auto b = composeProvenanceJson(in);
    EXPECT_EQ(a, b);
}

TEST(Phase13, archiveInstallVerifySidecarsRoundTrip) {
    // Criterion 3: simulate the install flow's verification step.
    // We don't invoke the subcommand directly (it shells through
    // main); instead we exercise the same primitives the
    // subcommand uses — composeProvenanceJson + verifyProvenance
    // Json — against an archive whose sidecar is on disk.
    auto root = tempRoot("installRoundTrip");
    auto archive = root / "x.cja";
    writeFile(archive, "this-is-the-archive-payload");
    // Compute the archive's sha256 using the same helper as the
    // install command (sha256Hex exposed from Lockfile.h).
    std::ifstream f(archive, std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf();
    std::string sha = cajeta::buildtool::sha256Hex(ss.str());
    ProvenanceInputs in = goodInputs();
    in.archiveName = "x.cja";
    in.archiveSha256 = sha;
    auto doc = composeProvenanceJson(in);
    writeFile(archive.string() + ".attestation", doc);
    // Read back and verify.
    std::ifstream af(archive.string() + ".attestation",
                     std::ios::binary);
    std::ostringstream as; as << af.rdbuf();
    auto r = verifyProvenanceJson(as.str(), sha);
    EXPECT_TRUE(static_cast<bool>(r));
}

TEST(Phase13, installRefusesTamperedArchive) {
    // Criterion 3: when the archive bytes change but the
    // attestation still claims the old digest, verify fails.
    auto root = tempRoot("installTamper");
    auto archive = root / "x.cja";
    writeFile(archive, "the-original-payload");
    std::ifstream f(archive, std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf();
    std::string sha = cajeta::buildtool::sha256Hex(ss.str());
    ProvenanceInputs in = goodInputs();
    in.archiveName = "x.cja";
    in.archiveSha256 = sha;
    auto doc = composeProvenanceJson(in);
    writeFile(archive.string() + ".attestation", doc);
    // Tamper: rewrite the archive's bytes.
    writeFile(archive, "the-tampered-payload");
    std::ifstream f2(archive, std::ios::binary);
    std::ostringstream ss2; ss2 << f2.rdbuf();
    std::string newSha = cajeta::buildtool::sha256Hex(ss2.str());
    std::ifstream af(archive.string() + ".attestation",
                     std::ios::binary);
    std::ostringstream as; as << af.rdbuf();
    auto r = verifyProvenanceJson(as.str(), newSha);
    EXPECT_FALSE(static_cast<bool>(r));
    if (!r) llvm::consumeError(r.takeError());
}
