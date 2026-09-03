// publisher-trust §2.7 — the repository delegation.
//
// The root signs this; this names the keys that may sign release metadata.
// The point is that the root need never be online: it signs organization
// documents and this, both rare, while the delegated key does the per-publish
// work.
//
// The test this suite exists for is `anOrgDocumentIsNotADelegation`. Both are
// root-signed envelopes carrying keys with validity windows, and if one were
// ever accepted as the other, ANY organization's signing key could sign
// release metadata for EVERY organization. Everything else here is ordinary
// validity checking; that one is the reason the type exists separately.

#include "gtest/gtest.h"

#include "OrgKeyFixture.h"
#include "TempTeardown.h"

#include "cajeta/buildtool/OrgKeyCache.h"
#include "cajeta/buildtool/ReleaseIntegrity.h"
#include "cajeta/buildtool/RepositoryDelegation.h"
#include "cajeta/buildtool/repo/FilesystemRepository.h"

#include <filesystem>
#include <sstream>
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
    auto p = fs::temp_directory_path() / ("cajeta-delegation-" + tag);
    rmTree(p);
    fs::create_directories(p);
    return p;
}

std::time_t at(const char* stamp) { return *parseUtcTimestamp(stamp); }

// The repository ORIGIN these documents speak for. A URL, not a manifest
// label — the label is the user's and differs between machines.
constexpr const char* kOrigin = "https://olla.test";

std::string delegationPayload(const std::string& repository,
                              const TestKeyPair& releaseKey,
                              const std::string& notAfter = "2030-01-01T00:00:00Z",
                              const std::string& keyNotBefore = "2020-01-01T00:00:00Z",
                              const std::string& keyNotAfter = "2030-01-01T00:00:00Z",
                              const std::string& issuedAt = "2026-01-01T00:00:00Z") {
    std::ostringstream p;
    p << "{\"type\":\"repository-delegation\","
      << "\"repository\":\"" << repository << "\",";
    if (!issuedAt.empty()) p << "\"issued-at\":\"" << issuedAt << "\",";
    p << "\"not-after\":\"" << notAfter << "\","
      << "\"keys\":[{\"id\":\"release-1\",\"algorithm\":\"ed25519\","
      << "\"public-key\":\"" << jsonEscapePem(readWholeFile(releaseKey.pub))
      << "\",\"not-before\":\"" << keyNotBefore << "\","
      << "\"not-after\":\"" << keyNotAfter << "\"}]}";
    return p.str();
}

}  // namespace

// THE test. An organization key document and a delegation are both
// root-signed envelopes of keys-with-windows. If a client took one for the
// other, any org could sign release metadata for everybody.
TEST(RepositoryDelegationTests, anOrgDocumentIsNotADelegation) {
    auto dir = freshDir("confusion");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");
    auto roots = std::vector<RootKey>{rootKeyOf(root, "r")};
    auto now = at("2026-06-01T00:00:00Z");

    // A perfectly valid organization key document, signed by the real root.
    std::string orgDoc = orgKeyDocument(dir, OrgDocumentSpec{}, orgKey, root, "r");
    ASSERT_TRUE(!!loadOrgKeyDocument(orgDoc, roots, now))
        << "fixture check: this must be a VALID org document";

    auto asDelegation = loadRepositoryDelegation(orgDoc, roots, "central", now);
    ASSERT_FALSE(!!asDelegation)
        << "an organization key document must never be usable as a "
           "delegation — that would let any org sign release metadata for all";
    EXPECT_NE(std::string::npos,
              errText(asDelegation.takeError()).find("repository-delegation"));

    // And the reverse: a delegation is not an organization document. It
    // carries no `organization` or `namespaces`, so it cannot authorise a
    // namespace even if something tried to read it that way.
    auto release = makeKeyPair(dir, "release");
    std::string del = envelopeAround(dir, delegationPayload(kOrigin, release),
                                     root, "r", "d");
    auto asOrgDoc = loadOrgKeyDocument(del, roots, now);
    EXPECT_FALSE(!!asOrgDoc) << "a delegation must not parse as an org document";
    if (!asOrgDoc) consumeError(asOrgDoc.takeError());

    rmTree(dir);
}

// The ordinary case: root-signed, in window, names a usable key.
TEST(RepositoryDelegationTests, aRootSignedDelegationNamesItsReleaseKeys) {
    auto dir = freshDir("ok");
    auto root = makeKeyPair(dir, "root");
    auto release = makeKeyPair(dir, "release");

    auto del = loadRepositoryDelegation(
        envelopeAround(dir, delegationPayload(kOrigin, release), root, "r", "d"),
        {rootKeyOf(root, "r")}, kOrigin, at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!del) << errText(del.takeError());
    EXPECT_EQ(kOrigin, del->repository);
    EXPECT_EQ("r", del->rootKeyId);
    ASSERT_EQ(1u, del->usableKeys(at("2026-06-01T00:00:00Z")).size());
    EXPECT_EQ("release-1", del->usableKeys(at("2026-06-01T00:00:00Z"))[0]->id);

    rmTree(dir);
}

// Signed by something that is not a trusted root: refused. This is what stops
// olla's own online key from minting its own delegation.
TEST(RepositoryDelegationTests, aDelegationNotSignedByARootIsRefused) {
    auto dir = freshDir("badroot");
    auto root = makeKeyPair(dir, "root");
    auto impostor = makeKeyPair(dir, "impostor");
    auto release = makeKeyPair(dir, "release");

    auto del = loadRepositoryDelegation(
        envelopeAround(dir, delegationPayload(kOrigin, release), impostor,
                       "r", "d"),
        {rootKeyOf(root, "r")}, kOrigin, at("2026-06-01T00:00:00Z"));
    ASSERT_FALSE(!!del)
        << "only the ROOT delegates — an online key must not be able to "
           "extend its own authority";
    EXPECT_NE(std::string::npos,
              errText(del.takeError()).find("does not match any trusted root"));

    rmTree(dir);
}

// An expired delegation is an ERROR, not a value with a flag. A stale
// delegation is how a revoked signing key keeps working.
TEST(RepositoryDelegationTests, anExpiredDelegationIsRefused) {
    auto dir = freshDir("expired");
    auto root = makeKeyPair(dir, "root");
    auto release = makeKeyPair(dir, "release");

    auto del = loadRepositoryDelegation(
        envelopeAround(dir, delegationPayload(kOrigin, release,
                                              "2026-07-01T00:00:00Z"),
                       root, "r", "d"),
        {rootKeyOf(root, "r")}, kOrigin, at("2026-08-01T00:00:00Z"));
    ASSERT_FALSE(!!del);
    EXPECT_NE(std::string::npos, errText(del.takeError()).find("expired"));

    rmTree(dir);
}

// A delegated key outside its own window authorises nothing, even though the
// delegation itself is current. usableKeys() may legitimately return empty,
// and the caller must read that as "cannot verify".
TEST(RepositoryDelegationTests, aDelegatedKeyOutsideItsWindowIsUnusable) {
    auto dir = freshDir("keywindow");
    auto root = makeKeyPair(dir, "root");
    auto release = makeKeyPair(dir, "release");

    auto del = loadRepositoryDelegation(
        envelopeAround(dir,
                       delegationPayload(kOrigin, release,
                                         "2030-01-01T00:00:00Z",
                                         "2020-01-01T00:00:00Z",
                                         "2025-01-01T00:00:00Z"),
                       root, "r", "d"),
        {rootKeyOf(root, "r")}, kOrigin, at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!del) << "the delegation itself is still current";
    EXPECT_TRUE(del->usableKeys(at("2026-06-01T00:00:00Z")).empty());
    EXPECT_EQ(1u, del->usableKeys(at("2024-06-01T00:00:00Z")).size());

    rmTree(dir);
}

// End to end through releaseIntegrityFor: metadata signed by the DELEGATED
// key verifies, and the root is never needed to check it. This is the whole
// point — olla signs releases without its root being online.
TEST(RepositoryDelegationTests, releaseMetadataVerifiesAgainstTheDelegatedKey) {
    auto dir = freshDir("endtoend");
    auto root = makeKeyPair(dir, "root");
    auto release = makeKeyPair(dir, "release");
    auto repoRoot = dir / "repo";
    auto rel = repoRoot / "dev.cajeta.http" / "1.0.0";
    fs::create_directories(rel);
    writeWholeFile(rel / "dev.cajeta.http-1.0.0.cja", "bytes");

    // Signed by the RELEASE key, not the root.
    writeWholeFile(rel / "dev.cajeta.http-1.0.0.release.json",
                   envelopeAround(dir,
                                  releasePayload("dev.cajeta.http", "1.0.0",
                                                 "sha256:abc", "dev.cajeta"),
                                  release, "release-1", "rel"));

    auto del = loadRepositoryDelegation(
        envelopeAround(dir, delegationPayload(kOrigin, release), root, "r", "d"),
        {rootKeyOf(root, "r")}, kOrigin, at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!del) << errText(del.takeError());

    FilesystemRepository repo("local", repoRoot.string());
    auto integrity = releaseIntegrityFor(repo, "dev.cajeta.http", "1.0.0",
                                         {rootKeyOf(root, "r")}, &*del,
                                         at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!integrity) << errText(integrity.takeError());
    EXPECT_TRUE(integrity->fromSignedMetadata);
    EXPECT_TRUE(integrity->viaDelegation);
    EXPECT_EQ("sha256:abc", integrity->sha256);
    EXPECT_EQ("dev.cajeta", integrity->organization);

    // And the negative: with the delegation withheld, the same metadata does
    // NOT verify against the root, because the root did not sign it. That is
    // what proves the delegated key was actually doing the work.
    auto withoutDelegation = releaseIntegrityFor(
        repo, "dev.cajeta.http", "1.0.0", {rootKeyOf(root, "r")}, nullptr,
        at("2026-06-01T00:00:00Z"));
    EXPECT_FALSE(!!withoutDelegation)
        << "release metadata signed by the delegated key must not verify "
           "against the root alone";
    if (!withoutDelegation) consumeError(withoutDelegation.takeError());

    rmTree(dir);
}

// A delegation whose keys have all expired must refuse rather than silently
// falling back to the roots — the fallback exists for a repository that
// serves NO delegation, not one whose delegation has lapsed.
TEST(RepositoryDelegationTests, alapsedDelegationDoesNotFallBackToTheRoot) {
    auto dir = freshDir("nofallback");
    auto root = makeKeyPair(dir, "root");
    auto release = makeKeyPair(dir, "release");
    auto repoRoot = dir / "repo";
    auto rel = repoRoot / "dev.cajeta.http" / "1.0.0";
    fs::create_directories(rel);
    writeWholeFile(rel / "dev.cajeta.http-1.0.0.cja", "bytes");
    // Signed by the ROOT this time — so a fallback would accept it.
    writeWholeFile(rel / "dev.cajeta.http-1.0.0.release.json",
                   envelopeAround(dir,
                                  releasePayload("dev.cajeta.http", "1.0.0",
                                                 "sha256:abc", "dev.cajeta"),
                                  root, "r", "rel"));

    auto del = loadRepositoryDelegation(
        envelopeAround(dir,
                       delegationPayload(kOrigin, release,
                                         "2030-01-01T00:00:00Z",
                                         "2020-01-01T00:00:00Z",
                                         "2025-01-01T00:00:00Z"),
                       root, "r", "d"),
        {rootKeyOf(root, "r")}, kOrigin, at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!del);

    FilesystemRepository repo("local", repoRoot.string());
    auto integrity = releaseIntegrityFor(repo, "dev.cajeta.http", "1.0.0",
                                         {rootKeyOf(root, "r")}, &*del,
                                         at("2026-06-01T00:00:00Z"));
    ASSERT_FALSE(!!integrity)
        << "a delegation with no usable key must refuse, not quietly let the "
           "root verify instead";
    EXPECT_NE(std::string::npos,
              errText(integrity.takeError()).find("validity window"));

    rmTree(dir);
}

// 13.1.1 — the check the header, the schema and spec §2.7.1 all claimed and
// none of them had. A delegation fetched from one repository must not
// authorise another, or that repository's online release key signs for both.
TEST(RepositoryDelegationTests, aDelegationForAnotherRepositoryIsRefused) {
    auto dir = freshDir("replay");
    auto root = makeKeyPair(dir, "root");
    auto release = makeKeyPair(dir, "release");

    auto del = loadRepositoryDelegation(
        envelopeAround(dir, delegationPayload("https://elsewhere.test", release),
                       root, "r", "d"),
        {rootKeyOf(root, "r")}, kOrigin, at("2026-06-01T00:00:00Z"));
    ASSERT_FALSE(!!del)
        << "validly root-signed and inside its window — only the repository "
           "it names is wrong, and that is the whole attack";
    EXPECT_NE(std::string::npos,
              errText(del.takeError()).find("https://elsewhere.test"));

    rmTree(dir);
}

// 13.1.2 — the regression this unit exists for. Two clients that call the
// same server different things must both accept the same delegation. When
// the binding was `name()`, whichever label the operator signed won and
// every other client was refused — and revocation fails CLOSED, so that was
// installs stopping.
TEST(RepositoryDelegationTests, theBindingIsTheOriginNotTheManifestLabel) {
    auto dir = freshDir("nickname");
    auto root = makeKeyPair(dir, "root");
    auto release = makeKeyPair(dir, "release");
    auto repoRoot = dir / "repo";
    fs::create_directories(repoRoot);

    std::string signed_ = envelopeAround(
        dir, delegationPayload(FilesystemRepository("ignored", repoRoot.string())
                                   .origin(),
                               release),
        root, "r", "d");

    // Same tree, two manifests, two different nicknames for it.
    FilesystemRepository asCentral("central", repoRoot.string());
    FilesystemRepository asOlla("olla-prod", repoRoot.string());
    ASSERT_NE(asCentral.name(), asOlla.name()) << "fixture: labels must differ";
    ASSERT_EQ(asCentral.origin(), asOlla.origin()) << "fixture: one server";

    for (const Repository* r : {static_cast<const Repository*>(&asCentral),
                                static_cast<const Repository*>(&asOlla)}) {
        auto del = loadRepositoryDelegation(signed_, {rootKeyOf(root, "r")},
                                            r->origin(),
                                            at("2026-06-01T00:00:00Z"));
        EXPECT_TRUE(!!del) << "refused for label '" << r->name()
                           << "': " << errText(del.takeError());
    }

    rmTree(dir);
}

// ── §2.9 freshness, applied to the delegation ────────────────────────────
//
// The delegation predates §2.9: it landed on 2026-08-30 and `issued-at`
// arrived later, written inside §2 (the organization key document) and never
// carried across. There is no asymmetry that would justify the gap — both
// documents share the same key definition with per-key windows, and §2.9.1's
// argument transfers word for word. A previous delegation is still validly
// signed and still inside its own window, so a mirror serves last quarter's
// copy and the release key that was rotated out is trusted again.

TEST(RepositoryDelegationTests, aDelegationWithoutIssuedAtIsRefused) {
    auto dir = freshDir("no-issued-at");
    auto root = makeKeyPair(dir, "root");
    auto release = makeKeyPair(dir, "release");

    auto del = loadRepositoryDelegation(
        envelopeAround(dir,
                       delegationPayload(kOrigin, release,
                                         "2030-01-01T00:00:00Z",
                                         "2020-01-01T00:00:00Z",
                                         "2030-01-01T00:00:00Z",
                                         /*issuedAt=*/""),
                       root, "r", "d"),
        {rootKeyOf(root, "r")}, kOrigin, at("2026-06-01T00:00:00Z"), 0);
    ASSERT_FALSE(!!del)
        << "an optional issued-at cannot be checked: a delegation omitting it "
           "would skip the comparison, which is the whole attack";
    EXPECT_NE(std::string::npos, errText(del.takeError()).find("issued-at"));
    rmTree(dir);
}

TEST(RepositoryDelegationTests, anOlderDelegationIsRefused) {
    auto dir = freshDir("rollback");
    auto root = makeKeyPair(dir, "root");
    auto release = makeKeyPair(dir, "release");
    auto roots = std::vector<RootKey>{rootKeyOf(root, "r")};
    auto now = at("2026-06-01T00:00:00Z");
    auto seen = at("2026-05-01T00:00:00Z");

    auto older = loadRepositoryDelegation(
        envelopeAround(dir,
                       delegationPayload(kOrigin, release,
                                         "2030-01-01T00:00:00Z",
                                         "2020-01-01T00:00:00Z",
                                         "2030-01-01T00:00:00Z",
                                         "2026-01-01T00:00:00Z"),
                       root, "r", "old"),
        roots, kOrigin, now, seen);
    ASSERT_FALSE(!!older) << "a delegation older than one already accepted "
                             "reinstates the release key that was rotated out";
    EXPECT_NE(std::string::npos, errText(older.takeError()).find("older"));

    // And the half that proves the check is not simply refusing everything.
    auto newer = loadRepositoryDelegation(
        envelopeAround(dir,
                       delegationPayload(kOrigin, release,
                                         "2030-01-01T00:00:00Z",
                                         "2020-01-01T00:00:00Z",
                                         "2030-01-01T00:00:00Z",
                                         "2026-05-15T00:00:00Z"),
                       root, "r", "new"),
        roots, kOrigin, now, seen);
    EXPECT_TRUE(!!newer) << errText(newer.takeError());
    rmTree(dir);
}

// ── The delegation reaches the cache at all ──────────────────────────────
//
// OrgKeyCache::delegationFor compared the delegation's signed `repository`
// against repo.name() — the label from the user's own manifest — AFTER
// loadRepositoryDelegation had already checked it against repo.origin(). The
// two are equal only when a repository is configured under its own origin, so
// every real deployment failed, and Packages.install turns that error into a
// hard failure. Commit 1bc40610 fixed exactly this in the revocation path and
// left this copy behind; the contract tests call loadRepositoryDelegation
// directly, so nothing exercised it.
TEST(RepositoryDelegationTests, theCacheAcceptsADelegationNamingTheOrigin) {
    auto dir = freshDir("cache-origin");
    auto root = makeKeyPair(dir, "root");
    auto release = makeKeyPair(dir, "release");
    auto repoRoot = dir / "repo";
    fs::create_directories(repoRoot / ".well-known");

    // A repository configured under a NICKNAME, which is the ordinary case:
    // the manifest says "central", the origin is where it actually lives.
    FilesystemRepository repo("central", repoRoot.string());
    ASSERT_NE(repo.name(), repo.origin())
        << "fixture check: this test is only meaningful when the manifest "
           "label and the origin differ, which is the normal deployment";

    writeWholeFile(repoRoot / ".well-known" / "repository-keys.json",
                   envelopeAround(dir, delegationPayload(repo.origin(), release),
                                  root, "r", "d"));

    RootTrustLayout layout;
    layout.searchDirs = {(dir / "trust").string()};
    layout.shippedOverride = rootKeyOf(root, "r");
    OrgKeyCache cache(layout);

    auto del = cache.delegationFor(repo, at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!del) << errText(del.takeError());
    ASSERT_TRUE(del->has_value());
    EXPECT_EQ(repo.origin(), (*del)->repository);
    rmTree(dir);
}
