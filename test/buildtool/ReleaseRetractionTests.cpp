// publisher-trust §7.6.2 — retraction in the signed path.
//
// `retracted` used to live only in the unsigned half of a resolve body, so
// a mirror could clear it and the client would never know. Everything else
// in the response was covered by the signature; the one signal whose job is
// to reach a client about to install a bad release was not.
//
// The test this suite exists for is `theSignedFlagBeatsThePlainOne`. The
// rest is ordinary field handling; that one is the reason the field moved.

#include "gtest/gtest.h"

#include "OrgKeyFixture.h"
#include "TempTeardown.h"

#include "cajeta/buildtool/ReleaseIntegrity.h"
#include "cajeta/buildtool/ReleaseMetadata.h"
#include "cajeta/buildtool/repo/FilesystemRepository.h"

#include <filesystem>
#include <string>

using namespace cajeta::buildtool;
using namespace cajeta::buildtool::testing;
namespace fs = std::filesystem;

namespace {

std::string errText(llvm::Error&& e) {
    std::string out;
    llvm::raw_string_ostream os(out);
    os << e;
    consumeError(std::move(e));
    return out;
}

fs::path freshDir(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("cajeta-retraction-" + tag);
    rmTree(p);
    fs::create_directories(p);
    return p;
}

std::time_t at(const char* stamp) { return *parseUtcTimestamp(stamp); }

// A filesystem repository holding one release, with `sidecar` as its
// `.release.json`. Returns the repository root.
fs::path repoWith(const fs::path& dir, const std::string& sidecar) {
    auto root = dir / "repo";
    auto rel = root / "dev.cajeta.http" / "1.0.0";
    fs::create_directories(rel);
    writeWholeFile(rel / "dev.cajeta.http-1.0.0.cja", "bytes");
    writeWholeFile(rel / "dev.cajeta.http-1.0.0.release.json", sidecar);
    return root;
}

}  // namespace

// 9.1.1 — the ordinary case: a signed retraction is reported, reason and all.
TEST(ReleaseRetractionTests, aSignedRetractionIsReported) {
    auto dir = freshDir("signed");
    auto root = makeKeyPair(dir, "root");

    auto repoRoot = repoWith(dir, envelopeAround(dir,
        retractedReleasePayload("dev.cajeta.http", "1.0.0", "sha256:abc",
                                "dev.cajeta", true, "CVE-2026-42"),
        root, "r", "rel"));

    FilesystemRepository repo("local", repoRoot.string());
    auto integrity = releaseIntegrityFor(repo, "dev.cajeta.http", "1.0.0",
                                         {rootKeyOf(root, "r")}, nullptr,
                                         at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!integrity) << errText(integrity.takeError());
    EXPECT_TRUE(integrity->fromSignedMetadata);
    EXPECT_TRUE(integrity->retracted);
    EXPECT_EQ("CVE-2026-42", integrity->retractedReason);
}

// 9.1.2 — THE test. Signed says retracted, the plain half beside it says
// not. A mirror rewriting the plain half is exactly this shape, and the
// signed half has to win.
TEST(ReleaseRetractionTests, theSignedFlagBeatsThePlainOne) {
    auto dir = freshDir("mirror");
    auto root = makeKeyPair(dir, "root");

    std::string envelope = envelopeAround(dir,
        retractedReleasePayload("dev.cajeta.http", "1.0.0", "sha256:abc",
                                "dev.cajeta", true, "CVE-2026-42"),
        root, "r", "rel");

    // The full resolve body: plain fields, with the envelope under `signed`
    // — and a plain `retracted:false` a mirror would have written.
    auto repoRoot = repoWith(dir,
        R"({"sha256":"sha256:abc","retracted":false,)"
        R"("retracted-reason":"","signed":)" + envelope + "}");

    FilesystemRepository repo("local", repoRoot.string());
    auto integrity = releaseIntegrityFor(repo, "dev.cajeta.http", "1.0.0",
                                         {rootKeyOf(root, "r")}, nullptr,
                                         at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!integrity) << errText(integrity.takeError());
    EXPECT_TRUE(integrity->retracted)
        << "the plain half is advisory and never merged — a mirror clearing "
           "it must not un-retract the release";
    EXPECT_EQ("CVE-2026-42", integrity->retractedReason);
}

// 9.1.3 — the reverse. The plain half cannot manufacture a retraction
// either, or anyone in the path could deny service by claiming one.
TEST(ReleaseRetractionTests, thePlainFlagCannotFakeARetraction) {
    auto dir = freshDir("fake");
    auto root = makeKeyPair(dir, "root");

    std::string envelope = envelopeAround(dir,
        retractedReleasePayload("dev.cajeta.http", "1.0.0", "sha256:abc",
                                "dev.cajeta", false),
        root, "r", "rel");

    auto repoRoot = repoWith(dir,
        R"({"sha256":"sha256:abc","retracted":true,)"
        R"("retracted-reason":"not really","signed":)" + envelope + "}");

    FilesystemRepository repo("local", repoRoot.string());
    auto integrity = releaseIntegrityFor(repo, "dev.cajeta.http", "1.0.0",
                                         {rootKeyOf(root, "r")}, nullptr,
                                         at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!integrity) << errText(integrity.takeError());
    EXPECT_FALSE(integrity->retracted);
    EXPECT_TRUE(integrity->retractedReason.empty());
}

// 9.1.4 — absent means false, not an error. Every release published before
// this landed omits the field, and refusing them would be a flag day.
TEST(ReleaseRetractionTests, anAbsentFlagMeansNotRetracted) {
    auto dir = freshDir("absent");
    auto root = makeKeyPair(dir, "root");

    auto repoRoot = repoWith(dir, envelopeAround(dir,
        releasePayload("dev.cajeta.http", "1.0.0", "sha256:abc", "dev.cajeta"),
        root, "r", "rel"));

    FilesystemRepository repo("local", repoRoot.string());
    auto integrity = releaseIntegrityFor(repo, "dev.cajeta.http", "1.0.0",
                                         {rootKeyOf(root, "r")}, nullptr,
                                         at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!integrity) << errText(integrity.takeError());
    EXPECT_TRUE(integrity->fromSignedMetadata);
    EXPECT_FALSE(integrity->retracted);
}

// 9.1.5 — a repository serving NO signed metadata still surfaces its plain
// flag. It is worth less: whoever can write the flag can clear it, so this
// is advisory. But a warning that might be spurious beats none at all, and
// `fromSignedMetadata` already tells a caller which kind it holds.
TEST(ReleaseRetractionTests, anUnsignedRepositoryStillSurfacesItsPlainFlag) {
    auto dir = freshDir("unsigned");
    auto root = makeKeyPair(dir, "root");

    auto repoRoot = repoWith(dir,
        R"({"sha256":"sha256:abc","retracted":true,)"
        R"("retracted-reason":"superseded by 1.0.1"})");

    FilesystemRepository repo("local", repoRoot.string());
    auto integrity = releaseIntegrityFor(repo, "dev.cajeta.http", "1.0.0",
                                         {rootKeyOf(root, "r")}, nullptr,
                                         at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!integrity) << errText(integrity.takeError());
    EXPECT_FALSE(integrity->fromSignedMetadata)
        << "fixture check: this document carries no signature";
    EXPECT_TRUE(integrity->retracted);
    EXPECT_EQ("superseded by 1.0.1", integrity->retractedReason);
}

// The parser level, so a failure points at ReleaseMetadata rather than at
// the repository plumbing above it.
TEST(ReleaseRetractionTests, loadReleaseMetadataReadsTheSignedFlag) {
    auto dir = freshDir("parser");
    auto root = makeKeyPair(dir, "root");

    auto md = loadReleaseMetadata(envelopeAround(dir,
        retractedReleasePayload("dev.cajeta.http", "1.0.0", "sha256:abc",
                                "dev.cajeta", true, "CVE-2026-42"),
        root, "r", "rel"), {rootKeyOf(root, "r")});
    ASSERT_TRUE(!!md) << errText(md.takeError());
    EXPECT_TRUE(md->retracted);
    EXPECT_EQ("CVE-2026-42", md->retractedReason);
}
