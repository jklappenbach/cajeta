// publisher-trust Unit 5 — release integrity in the signed path (spec
// §5.1–5.2).
//
// The property under test is a preference, and preferences are the kind of
// thing that quietly stop applying. Every test here therefore serves BOTH
// a signed hash and a conflicting unsigned one, and asserts which was
// taken. A test that served only the signed hash would pass just as well
// against code that read the sidecar and found nothing there.

#include "gtest/gtest.h"

#include "OrgKeyFixture.h"
#include "TempTeardown.h"

#include "cajeta/buildtool/ReleaseIntegrity.h"
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

// Delegation is exercised in its own suite; these predate it and verify
// against roots directly, which stays supported (spec 2.7).
const std::time_t kNoDelegationTime = 0;

fs::path freshDir(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("cajeta-relint-" + tag);
    rmTree(p);
    fs::create_directories(p);
    return p;
}

struct Staged {
    fs::path repoRoot;
    TestKeyPair root;
};

// A filesystem release carrying whichever of the two hash sources the
// caller asks for. `signedHash` empty means "serve no signed metadata".
Staged stageRelease(const fs::path& dir, const std::string& signedHash,
                    const std::string& sidecarHash,
                    const std::string& organization = "dev.cajeta") {
    Staged st;
    st.repoRoot = dir / "repo";
    st.root = makeKeyPair(dir / "keys", "root");

    auto rel = st.repoRoot / "dev.cajeta.http" / "1.0.0";
    fs::create_directories(rel);
    writeWholeFile(rel / "dev.cajeta.http-1.0.0.cja", "archive bytes");
    if (!sidecarHash.empty()) {
        writeWholeFile(rel / "dev.cajeta.http-1.0.0.cja.sha256", sidecarHash);
    }
    if (!signedHash.empty()) {
        writeWholeFile(rel / "dev.cajeta.http-1.0.0.release.json",
                       envelopeAround(dir / "keys",
                                      releasePayload("dev.cajeta.http", "1.0.0",
                                                     signedHash, organization),
                                      st.root, "fixture-root", "rel"));
    }
    return st;
}

}  // namespace

// 5.1.1 / spec 5.1 — the hash comes from the signed metadata, and the
// unsigned sidecar beside it is IGNORED rather than compared or merged.
TEST(ReleaseIntegrityTests, theHashComesFromTheSignedMetadata) {
    auto dir = freshDir("signed");
    auto st = stageRelease(dir, "sha256:5164ed", "sha256:deadbeef");
    FilesystemRepository repo("local", st.repoRoot.string());

    auto integrity = releaseIntegrityFor(
        repo, "dev.cajeta.http", "1.0.0",
        {rootKeyOf(st.root, "fixture-root")}, nullptr, kNoDelegationTime);
    ASSERT_TRUE(!!integrity) << errText(integrity.takeError());
    EXPECT_EQ("sha256:5164ed", integrity->sha256)
        << "the sidecar must not win over a root-signed hash";
    EXPECT_TRUE(integrity->fromSignedMetadata);
    EXPECT_EQ("dev.cajeta", integrity->organization);
    EXPECT_EQ("fixture-root", integrity->rootKeyId);

    rmTree(dir);
}

// 5.1.3 — a mirror rewriting the sidecar to match tampered bytes changes
// nothing a verifying client reads. The sidecar here is what a mirror
// would have written; the signed hash still names the real release.
TEST(ReleaseIntegrityTests, aMirrorRewritingTheSidecarIsIgnored) {
    auto dir = freshDir("mirror");
    // Both files exist and disagree. Whichever the code reads decides.
    auto st = stageRelease(dir, "sha256:realrelease",
                           "sha256:whatthemirrorwants");
    FilesystemRepository repo("local", st.repoRoot.string());

    auto integrity = releaseIntegrityFor(
        repo, "dev.cajeta.http", "1.0.0",
        {rootKeyOf(st.root, "fixture-root")}, nullptr, kNoDelegationTime);
    ASSERT_TRUE(!!integrity) << errText(integrity.takeError());
    EXPECT_EQ("sha256:realrelease", integrity->sha256);
    EXPECT_TRUE(integrity->fromSignedMetadata);

    rmTree(dir);
}

// 5.3.1 — the unsigned sidecar keeps working where there is no signed
// metadata, which is what local, vendored and pre-v2 repositories rely on.
// It is reported as UNSIGNED, so nothing downstream can mistake a
// self-consistency check for a statement about the publisher.
TEST(ReleaseIntegrityTests, theUnsignedSidecarStillWorksAndSaysSoIsUnsigned) {
    auto dir = freshDir("sidecar");
    auto st = stageRelease(dir, "", "sha256:deadbeef");
    FilesystemRepository repo("local", st.repoRoot.string());

    auto integrity = releaseIntegrityFor(
        repo, "dev.cajeta.http", "1.0.0",
        {rootKeyOf(st.root, "fixture-root")}, nullptr, kNoDelegationTime);
    ASSERT_TRUE(!!integrity) << errText(integrity.takeError());
    EXPECT_EQ("sha256:deadbeef", integrity->sha256);
    EXPECT_FALSE(integrity->fromSignedMetadata);
    EXPECT_TRUE(integrity->organization.empty())
        << "an unsigned path must yield no organization at all — an "
           "unsigned one binds nothing";

    rmTree(dir);
}

// Release metadata that is PRESENT and does not verify is a refusal, not a
// quiet fall-through to the sidecar. Otherwise a mirror strips the
// signature and gets the weaker route for free.
TEST(ReleaseIntegrityTests, unverifiableMetadataDoesNotFallBackToTheSidecar) {
    auto dir = freshDir("badsig");
    auto st = stageRelease(dir, "sha256:realrelease", "sha256:sidecar");
    auto stranger = makeKeyPair(dir / "keys", "stranger");
    FilesystemRepository repo("local", st.repoRoot.string());

    // A root the client does not hold.
    auto integrity = releaseIntegrityFor(
        repo, "dev.cajeta.http", "1.0.0",
        {rootKeyOf(stranger, "some-other-root")}, nullptr, kNoDelegationTime);
    ASSERT_FALSE(!!integrity)
        << "an unverifiable envelope must not degrade to the sidecar";
    EXPECT_NE(std::string::npos, errText(integrity.takeError()).find("did not verify"));

    rmTree(dir);
}

// A repository publishing neither is not an error — it simply holds the
// install to no hash, which is the pre-existing behaviour for a bare
// vendored tree. Unit 6 is where the default refuses that.
TEST(ReleaseIntegrityTests, aRepositoryPublishingNoHashIsNotAnError) {
    auto dir = freshDir("none");
    auto st = stageRelease(dir, "", "");
    FilesystemRepository repo("local", st.repoRoot.string());

    auto integrity = releaseIntegrityFor(
        repo, "dev.cajeta.http", "1.0.0",
        {rootKeyOf(st.root, "fixture-root")}, nullptr, kNoDelegationTime);
    ASSERT_TRUE(!!integrity) << errText(integrity.takeError());
    EXPECT_TRUE(integrity->sha256.empty());
    EXPECT_FALSE(integrity->fromSignedMetadata);

    rmTree(dir);
}

// Unsigned release metadata carries no more authority than the sidecar, so
// it gets no more weight. Without this, a server could serve a plain
// `{"sha256": ...}` and have it treated as the signed path.
TEST(ReleaseIntegrityTests, unsignedMetadataDoesNotCountAsSigned) {
    auto dir = freshDir("plain");
    auto st = stageRelease(dir, "", "sha256:sidecar");
    // Plain metadata, no envelope.
    writeWholeFile(st.repoRoot / "dev.cajeta.http" / "1.0.0"
                       / "dev.cajeta.http-1.0.0.release.json",
                   R"({"sha256":"sha256:plain","organization":"com.attacker"})");
    FilesystemRepository repo("local", st.repoRoot.string());

    auto integrity = releaseIntegrityFor(
        repo, "dev.cajeta.http", "1.0.0",
        {rootKeyOf(st.root, "fixture-root")}, nullptr, kNoDelegationTime);
    ASSERT_TRUE(!!integrity) << errText(integrity.takeError());
    EXPECT_FALSE(integrity->fromSignedMetadata);
    EXPECT_TRUE(integrity->organization.empty())
        << "an unsigned organization must never reach a caller";
    EXPECT_EQ("sha256:sidecar", integrity->sha256)
        << "unsigned metadata is not preferred over the sidecar — neither "
           "binds anything, and preferring one would imply it did";

    rmTree(dir);
}
