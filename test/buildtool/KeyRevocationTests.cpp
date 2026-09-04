// publisher-trust §2.8 — revocation.
//
// The root is offline (§2.7), so a re-signed key document omitting a
// compromised key waits on the offline ceremony. This statement is signed
// by the DELEGATED key instead and applies in seconds. It is the brake; the
// re-signed document is the repair.
//
// Two tests carry this suite. `aRevokedKeyIsUnusableDespiteAValidDocument`
// is the mechanism — the organization document still lists the key, its
// window is still open, and it must not verify anything. And the pair
// `anAdvertisedRepositoryFailsClosed` / `anUnadvertisedRepositoryProceeds`
// is the fail-closed rule: this is the ONLY document here whose absence is
// a failure, and a check needs a test that it fires and one that it does
// not.

#include "gtest/gtest.h"

#include "OrgKeyFixture.h"
#include "TempTeardown.h"

#include "cajeta/buildtool/KeyRevocation.h"
#include "cajeta/buildtool/PublisherVerification.h"
#include "cajeta/buildtool/RepositoryDelegation.h"

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
    auto p = fs::temp_directory_path() / ("cajeta-revocation-" + tag);
    rmTree(p);
    fs::create_directories(p);
    return p;
}

std::time_t at(const char* stamp) { return *parseUtcTimestamp(stamp); }

// The repository ORIGIN, not a manifest label (spec §2.7.1, §2.8).
constexpr const char* kOrigin = "https://olla.test";

std::string delegationPayload(const std::string& repository,
                              const TestKeyPair& releaseKey) {
    std::ostringstream p;
    p << "{\"type\":\"repository-delegation\","
      << "\"issued-at\":\"2026-01-01T00:00:00Z\","
      << "\"repository\":\"" << repository << "\","
      << "\"not-after\":\"2030-01-01T00:00:00Z\","
      << "\"keys\":[{\"id\":\"release-1\",\"algorithm\":\"ed25519\","
      << "\"public-key\":\"" << jsonEscapePem(readWholeFile(releaseKey.pub))
      << "\",\"not-before\":\"2020-01-01T00:00:00Z\","
      << "\"not-after\":\"2030-01-01T00:00:00Z\"}]}";
    return p.str();
}

// One revoked entry, optionally scoped to an organization.
std::string revocationPayload(const std::string& repository,
                              const std::string& revokedIds,
                              const std::string& issuedAt = "2026-06-01T00:00:00Z",
                              const std::string& notAfter = "2026-06-01T01:00:00Z") {
    std::ostringstream p;
    p << "{\"type\":\"key-revocation\","
      << "\"repository\":\"" << repository << "\","
      << "\"issued-at\":\"" << issuedAt << "\","
      << "\"not-after\":\"" << notAfter << "\","
      << "\"revoked\":[" << revokedIds << "]}";
    return p.str();
}

struct Fixture {
    fs::path dir;
    TestKeyPair root;
    TestKeyPair release;
    RepositoryDelegation delegation;

    explicit Fixture(const std::string& tag)
        : dir(freshDir(tag)),
          root(makeKeyPair(dir, "root")),
          release(makeKeyPair(dir, "release")) {
        auto del = loadRepositoryDelegation(
            envelopeAround(dir, delegationPayload(kOrigin, release),
                           root, "r", "d"),
            {rootKeyOf(root, "r")}, kOrigin, at("2026-06-01T00:00:00Z"));
        delegation = std::move(*del);
    }
    ~Fixture() { rmTree(dir); }

    std::string signedByRelease(const std::string& payload) {
        return envelopeAround(dir, payload, release, "release-1", "rev");
    }
};

}  // namespace

// 10.1.1 — signed by a delegated key, and NOT by a root. Accepting a root
// signature would invite exactly the long-lived statement §2.8.3 forbids.
TEST(KeyRevocationTests, onlyADelegatedKeyMaySignIt) {
    Fixture f("signer");
    auto now = at("2026-06-01T00:00:00Z");

    auto ok = loadKeyRevocation(
        f.signedByRelease(revocationPayload(kOrigin, "")),
        f.delegation, kOrigin, now, 0);
    ASSERT_TRUE(!!ok) << errText(ok.takeError());
    EXPECT_EQ("release-1", ok->signedByKeyId);

    auto byRoot = loadKeyRevocation(
        envelopeAround(f.dir, revocationPayload(kOrigin, ""), f.root, "r", "rr"),
        f.delegation, kOrigin, now, 0);
    EXPECT_FALSE(!!byRoot)
        << "a root signature must not be accepted here — this document's "
           "short lifetime is only sustainable because an online key makes it";
    if (!byRoot) consumeError(byRoot.takeError());
}

// 10.1.2 — THE mechanism test. The organization document still lists the
// key and its window is still open; the revocation alone must stop it.
TEST(KeyRevocationTests, aRevokedKeyIsUnusableDespiteAValidDocument) {
    Fixture f("mechanism");
    auto now = at("2026-06-01T00:00:00Z");
    auto orgKey = makeKeyPair(f.dir, "org");

    OrgDocumentSpec spec;
    spec.organization = "dev.cajeta";
    spec.namespaces = {"dev.cajeta"};
    auto doc = loadOrgKeyDocument(
        orgKeyDocument(f.dir, spec, orgKey, f.root, "r"),
        {rootKeyOf(f.root, "r")}, now);
    ASSERT_TRUE(!!doc);
    ASSERT_FALSE(doc->usableKeys(now).empty())
        << "fixture check: the document's key must be valid right now, or "
           "this test would pass without the revocation doing anything";
    std::string keyId = doc->usableKeys(now)[0]->id;

    auto artifact = f.dir / "dev.cajeta.http-1.0.0.cja";
    writeWholeFile(artifact, "archive bytes");
    std::string sig = signWithKey(f.dir, readWholeFile(artifact),
                                  orgKey.priv, "art");

    // Without a revocation it verifies.
    auto before = verifyAgainstOrgDocument(*doc, "dev.cajeta.http",
                                           artifact.string(), sig, now,
                                           nullptr);
    ASSERT_TRUE(before.ok()) << before.message;

    std::ostringstream entry;
    entry << "{\"id\":\"" << keyId << "\",\"organization\":\"dev.cajeta\","
          << "\"reason\":\"laptop stolen\"}";
    auto rev = loadKeyRevocation(
        f.signedByRelease(revocationPayload(kOrigin, entry.str())),
        f.delegation, kOrigin, now, 0);
    ASSERT_TRUE(!!rev) << errText(rev.takeError());

    auto after = verifyAgainstOrgDocument(*doc, "dev.cajeta.http",
                                          artifact.string(), sig, now, &*rev);
    EXPECT_FALSE(after.ok());
    EXPECT_EQ(PublisherCheck::Revoked, after.check)
        << "the message must say REVOKED, not 'no usable key' — an operator "
           "sent to the wrong place loses the incident";
    EXPECT_NE(std::string::npos, after.message.find("laptop stolen"));
}

// 10.1.3 — an expired statement is refused, not ignored. Serving an old one
// is how an attacker suppresses a revocation.
TEST(KeyRevocationTests, anExpiredStatementIsRefused) {
    Fixture f("expired");
    auto rev = loadKeyRevocation(
        f.signedByRelease(revocationPayload(kOrigin, "")),
        f.delegation, kOrigin, at("2026-06-01T02:00:00Z"), 0);
    ASSERT_FALSE(!!rev);
    EXPECT_NE(std::string::npos, errText(rev.takeError()).find("expired"));
}

// 10.1.5 — an empty list is a real statement, and a different one from
// serving nothing: it asserts that nothing is revoked as of `issued-at`.
TEST(KeyRevocationTests, anEmptyListAssertsNothingIsRevoked) {
    Fixture f("empty");
    auto rev = loadKeyRevocation(
        f.signedByRelease(revocationPayload(kOrigin, "")),
        f.delegation, kOrigin, at("2026-06-01T00:00:00Z"), 0);
    ASSERT_TRUE(!!rev) << errText(rev.takeError());
    EXPECT_TRUE(rev->revoked.empty());
    EXPECT_EQ(at("2026-06-01T00:00:00Z"), rev->issuedAt);
}

// 10.1.6 — one repository's statement replayed at another is refused. It
// revokes nothing it should not, but it is a denial of service someone else
// gets to trigger.
TEST(KeyRevocationTests, aStatementForAnotherRepositoryIsRefused) {
    Fixture f("replay");
    auto rev = loadKeyRevocation(
        f.signedByRelease(revocationPayload("https://elsewhere.test", "")),
        f.delegation, kOrigin, at("2026-06-01T00:00:00Z"), 0);
    ASSERT_FALSE(!!rev);
    EXPECT_NE(std::string::npos, errText(rev.takeError()).find("https://elsewhere.test"));
}

// 10.1.7 — a rollback to an older statement is refused once a newer one has
// been seen. Short windows already bound this: an attacker can only replay
// something still inside its own validity window.
TEST(KeyRevocationTests, aRollbackToAnOlderStatementIsRefused) {
    Fixture f("rollback");
    auto now = at("2026-06-01T00:30:00Z");

    auto older = loadKeyRevocation(
        f.signedByRelease(revocationPayload(kOrigin, "",
                                            "2026-06-01T00:00:00Z",
                                            "2026-06-01T01:00:00Z")),
        f.delegation, kOrigin, now, at("2026-06-01T00:20:00Z"));
    ASSERT_FALSE(!!older)
        << "a statement older than one already seen must be refused, even "
           "though it is validly signed and inside its own window";
    EXPECT_NE(std::string::npos, errText(older.takeError()).find("older"));

    // The same statement is fine when nothing newer has been seen.
    auto fresh = loadKeyRevocation(
        f.signedByRelease(revocationPayload(kOrigin, "",
                                            "2026-06-01T00:00:00Z",
                                            "2026-06-01T01:00:00Z")),
        f.delegation, kOrigin, now, 0);
    EXPECT_TRUE(!!fresh) << errText(fresh.takeError());
}

// 10.1.8 — an entry with no `organization` revokes that id everywhere. Key
// ids are only unique within a document, so an unscoped id is ambiguous and
// the safe reading is the broad one.
TEST(KeyRevocationTests, anUnscopedEntryRevokesTheIdEverywhere) {
    Fixture f("unscoped");
    auto now = at("2026-06-01T00:00:00Z");

    auto rev = loadKeyRevocation(
        f.signedByRelease(revocationPayload(kOrigin, R"({"id":"org-1"})")),
        f.delegation, kOrigin, now, 0);
    ASSERT_TRUE(!!rev) << errText(rev.takeError());

    EXPECT_NE(nullptr, rev->find("org-1", "dev.cajeta"));
    EXPECT_NE(nullptr, rev->find("org-1", "com.example"));
    EXPECT_EQ(nullptr, rev->find("org-2", "dev.cajeta"));
}

// A scoped entry stays scoped — the counterpart to the test above, so
// "revokes everywhere" cannot be implemented by ignoring the field.
TEST(KeyRevocationTests, aScopedEntryRevokesOnlyThatOrganization) {
    Fixture f("scoped");
    auto rev = loadKeyRevocation(
        f.signedByRelease(revocationPayload(kOrigin,
            R"({"id":"org-1","organization":"dev.cajeta"})")),
        f.delegation, kOrigin, at("2026-06-01T00:00:00Z"), 0);
    ASSERT_TRUE(!!rev) << errText(rev.takeError());

    EXPECT_NE(nullptr, rev->find("org-1", "dev.cajeta"));
    EXPECT_EQ(nullptr, rev->find("org-1", "com.example"));
}

// 10.1.9 — §2.7.4 applied to the new type. A delegation is not a revocation
// and cannot be read as one.
TEST(KeyRevocationTests, aDelegationIsNotARevocation) {
    Fixture f("confusion");
    auto now = at("2026-06-01T00:00:00Z");

    auto asRevocation = loadKeyRevocation(
        envelopeAround(f.dir, delegationPayload(kOrigin, f.release),
                       f.release, "release-1", "d2"),
        f.delegation, kOrigin, now, 0);
    ASSERT_FALSE(!!asRevocation);
    EXPECT_NE(std::string::npos,
              errText(asRevocation.takeError()).find("key-revocation"));

    auto asDelegation = loadRepositoryDelegation(
        envelopeAround(f.dir, revocationPayload(kOrigin, ""), f.root, "r", "x"),
        {rootKeyOf(f.root, "r")}, kOrigin, now);
    EXPECT_FALSE(!!asDelegation);
    if (!asDelegation) consumeError(asDelegation.takeError());
}
