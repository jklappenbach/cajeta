// publisher-trust Unit 7 — the server contract, executed (plan 7.1.1).
//
// specs/schemas/publisher-trust-protocol-v1.md §3 says what a server does.
// `OllaContractStub` does it. These run the SHIPPED client against that
// stub, so the document is checked rather than merely written: if the
// contract and the client ever disagree, one of these fails.
//
// The unit tests elsewhere check each client piece against a fixture built
// for it. What these add is that ONE server, behaving as documented,
// satisfies the whole chain at once — capabilities, key document, signed
// release metadata, hash, and the publisher binding.

#include "gtest/gtest.h"

#include "OllaContractStub.h"
#include "TempTeardown.h"

#include "cajeta/buildtool/OrgKeyCache.h"
#include "cajeta/buildtool/PublisherVerification.h"
#include "cajeta/buildtool/ReleaseIntegrity.h"
#include "cajeta/buildtool/repo/HttpRepository.h"

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
    auto p = fs::temp_directory_path() / ("cajeta-olla-contract-" + tag);
    rmTree(p);
    fs::create_directories(p);
    return p;
}

std::time_t at(const char* stamp) { return *parseUtcTimestamp(stamp); }

// A client configured the way a default install is: it holds the server's
// root and nothing else.
RootTrustLayout clientTrusting(const fs::path& dir, const RootKey& root) {
    RootTrustLayout layout;
    layout.searchDirs = {(dir / "trust").string()};
    layout.shippedOverride = root;
    return layout;
}

std::unique_ptr<HttpRepository> clientFor(const OllaContractStub& stub,
                                          const fs::path& dir) {
    return std::make_unique<HttpRepository>(
        "central", stub.baseUrl(), RepositoryAuth{}, (dir / "stage").string());
}

}  // namespace

// The whole chain against one conformant server: capabilities advertise
// v2, the key document verifies against the shipped root, the release
// metadata is signed, and an artifact signed by the organization's key
// passes the publisher binding.
TEST(OllaContractTests, aConformantServerSatisfiesTheWholeChain) {
    auto dir = freshDir("conformant");
    OllaContractStub stub(dir);

    OrgDocumentSpec org;
    org.organization = "dev.cajeta";
    org.namespaces = {"dev.cajeta"};
    stub.serveOrganization(org);

    // A real artifact, signed by the organization's key.
    auto artifact = dir / "dev.cajeta.http-1.0.0.cja";
    writeWholeFile(artifact, "archive bytes");
    std::string signature = signWithKey(dir, readWholeFile(artifact),
                                        stub.organizationKey().priv, "art");
    stub.serveRelease("dev.cajeta.http", "1.0.0", "sha256:abc", "dev.cajeta");

    auto repo = clientFor(stub, dir);
    auto now = at("2026-06-01T00:00:00Z");
    auto layout = clientTrusting(dir, stub.root());
    auto roots = rootsFor(layout, "central");
    ASSERT_TRUE(!!roots) << errText(roots.takeError());

    // §3.4 — the hash and the owning organization, out of the signed half.
    auto integrity = releaseIntegrityFor(*repo, "dev.cajeta.http", "1.0.0",
                                         *roots);
    ASSERT_TRUE(!!integrity) << errText(integrity.takeError());
    EXPECT_TRUE(integrity->fromSignedMetadata);
    EXPECT_EQ("sha256:abc", integrity->sha256);
    EXPECT_EQ("dev.cajeta", integrity->organization);
    EXPECT_EQ(stub.rootId(), integrity->rootKeyId);

    // §3.3 — the key document for that organization.
    OrgKeyCache cache(layout);
    auto doc = cache.documentFor(*repo, integrity->organization, now);
    ASSERT_TRUE(!!doc) << errText(doc.takeError());
    ASSERT_TRUE(doc->has_value());

    // §4 of the spec — the binding itself.
    auto verdict = verifyAgainstOrgDocument(**doc, "dev.cajeta.http",
                                            artifact.string(), signature, now);
    EXPECT_TRUE(verdict.ok()) << verdict.message;
    EXPECT_EQ("dev.cajeta", verdict.organization);

    rmTree(dir);
}

// §3.1, written as a test because it is the contract's sharpest foot-gun: a
// server can implement every endpoint correctly and disable verification
// entirely by not advertising v2. Nothing errors. This pins the behaviour
// so the hazard is documented in code as well as prose.
TEST(OllaContractTests, notAdvertisingV2DisablesVerificationSilently) {
    auto dir = freshDir("nov2");
    OllaContractStub stub(dir);

    OrgDocumentSpec org;
    org.organization = "dev.cajeta";
    stub.serveOrganization(org);        // the endpoint IS there and correct
    stub.advertiseV2(false);            // but the client is never told

    auto repo = clientFor(stub, dir);
    OrgKeyCache cache(clientTrusting(dir, stub.root()));
    auto doc = cache.documentFor(*repo, "dev.cajeta",
                                 at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!doc) << "a v1-only server is the legacy path, not an error";
    EXPECT_FALSE(doc->has_value())
        << "the document is never fetched — the client took the unverified "
           "path and nothing anywhere reported a problem";

    rmTree(dir);
}

// §3.6 — the table. A 404 is absence and degrades; a 5xx is failure and
// refuses. A server answering 404 for a transient fault converts an outage
// into a fleet-wide verification bypass.
TEST(OllaContractTests, absenceDegradesAndFailureRefuses) {
    auto dir = freshDir("absence");
    OllaContractStub stub(dir);
    auto repo = clientFor(stub, dir);
    OrgKeyCache cache(clientTrusting(dir, stub.root()));
    auto now = at("2026-06-01T00:00:00Z");

    // Nothing routed for this org: the stub's server 404s it.
    auto absent = cache.documentFor(*repo, "dev.unknown", now);
    ASSERT_TRUE(!!absent) << "404 is absence, not failure";
    EXPECT_FALSE(absent->has_value());

    stub.failOrganization("dev.cajeta", 503);
    auto failed = cache.documentFor(*repo, "dev.cajeta", now);
    ASSERT_FALSE(!!failed)
        << "a 503 must refuse — degrading here would turn an outage into a "
           "verification bypass";
    EXPECT_NE(std::string::npos, errText(failed.takeError()).find("503"));

    rmTree(dir);
}

// §3.4 — the signed half is authoritative and the plain half is never
// merged. The stub serves a body whose two halves disagree about both the
// hash and the organization, which is the only way to see which was read.
TEST(OllaContractTests, theSignedHalfIsAuthoritativeOverThePlainHalf) {
    auto dir = freshDir("authoritative");
    OllaContractStub stub(dir);
    stub.serveRelease("dev.cajeta.http", "1.0.0",
                      "sha256:signed", "dev.cajeta",
                      "sha256:tampered", "com.attacker");

    auto repo = clientFor(stub, dir);
    auto roots = rootsFor(clientTrusting(dir, stub.root()), "central");
    ASSERT_TRUE(!!roots);

    auto integrity = releaseIntegrityFor(*repo, "dev.cajeta.http", "1.0.0",
                                         *roots);
    ASSERT_TRUE(!!integrity) << errText(integrity.takeError());
    EXPECT_EQ("sha256:signed", integrity->sha256);
    EXPECT_EQ("dev.cajeta", integrity->organization);
    EXPECT_TRUE(integrity->fromSignedMetadata);

    rmTree(dir);
}

// A payload altered after signing is caught, and it is an ERROR rather
// than a quiet fall-through to the plain fields sitting beside it.
TEST(OllaContractTests, aPayloadAlteredAfterSigningIsRefused) {
    auto dir = freshDir("tampered");
    OllaContractStub stub(dir);
    stub.serveTamperedRelease("dev.cajeta.http", "1.0.0", "sha256:abc",
                              "dev.cajeta");

    auto repo = clientFor(stub, dir);
    auto roots = rootsFor(clientTrusting(dir, stub.root()), "central");
    ASSERT_TRUE(!!roots);

    auto integrity = releaseIntegrityFor(*repo, "dev.cajeta.http", "1.0.0",
                                         *roots);
    ASSERT_FALSE(!!integrity)
        << "an altered payload must not degrade to the unsigned fields "
           "beside it — a mirror would then just strip the signature";
    EXPECT_NE(std::string::npos,
              errText(integrity.takeError()).find("did not verify"));

    rmTree(dir);
}

// A server that serves plain resolve fields and no envelope binds nothing.
// It still supplies a hash — a self-consistency check on the download —
// but no organization reaches the caller.
TEST(OllaContractTests, anUnsignedResolveBindsNoPublisher) {
    auto dir = freshDir("unsigned");
    OllaContractStub stub(dir);
    stub.serveUnsignedRelease("dev.cajeta.http", "1.0.0", "sha256:abc",
                              "com.attacker");

    auto repo = clientFor(stub, dir);
    auto roots = rootsFor(clientTrusting(dir, stub.root()), "central");
    ASSERT_TRUE(!!roots);

    auto integrity = releaseIntegrityFor(*repo, "dev.cajeta.http", "1.0.0",
                                         *roots);
    ASSERT_TRUE(!!integrity) << errText(integrity.takeError());
    EXPECT_FALSE(integrity->fromSignedMetadata);
    EXPECT_TRUE(integrity->organization.empty())
        << "an unsigned organization must never reach a caller";

    rmTree(dir);
}

// §3.3 — a client holding a DIFFERENT root refuses everything this server
// says, which is what makes the root the anchor rather than the transport.
TEST(OllaContractTests, aClientHoldingAnotherRootRefusesTheDocument) {
    auto dir = freshDir("otherroot");
    OllaContractStub stub(dir);
    OrgDocumentSpec org;
    org.organization = "dev.cajeta";
    stub.serveOrganization(org);

    auto stranger = makeKeyPair(dir / "client-keys", "stranger");
    auto repo = clientFor(stub, dir);
    OrgKeyCache cache(clientTrusting(dir, rootKeyOf(stranger, "some-root")));

    auto doc = cache.documentFor(*repo, "dev.cajeta",
                                 at("2026-06-01T00:00:00Z"));
    ASSERT_FALSE(!!doc)
        << "serving over TLS is not enough — the root is the anchor";
    EXPECT_NE(std::string::npos,
              errText(doc.takeError()).find("does not match any trusted root"));

    rmTree(dir);
}

// §3.3 — the rotation property, end to end against the server: two keys
// with overlapping windows, and an artifact signed by either verifies.
// This is the mechanism that lets a publisher rotate without a flag day,
// so a server that serves only one key at a time has broken it.
TEST(OllaContractTests, overlappingKeyWindowsLetAPublisherRotate) {
    auto dir = freshDir("rotate");
    OllaContractStub stub(dir);

    OrgDocumentSpec org;
    org.organization = "dev.cajeta";
    org.keyNotBefore = "2020-01-01T00:00:00Z";
    org.keyNotAfter = "2030-01-01T00:00:00Z";
    stub.serveOrganization(org);

    auto artifact = dir / "a.cja";
    writeWholeFile(artifact, "archive bytes");
    std::string signature = signWithKey(dir, readWholeFile(artifact),
                                        stub.organizationKey().priv, "art");

    auto repo = clientFor(stub, dir);
    OrgKeyCache cache(clientTrusting(dir, stub.root()));

    // Valid early in the window and late in it: the same document, two
    // instants, because expiry is a parameter rather than a clock read.
    for (const char* when : {"2021-01-01T00:00:00Z", "2029-01-01T00:00:00Z"}) {
        auto doc = cache.documentFor(*repo, "dev.cajeta", at(when));
        ASSERT_TRUE(!!doc) << when << ": " << errText(doc.takeError());
        ASSERT_TRUE(doc->has_value());
        auto v = verifyAgainstOrgDocument(**doc, "dev.cajeta.http",
                                          artifact.string(), signature,
                                          at(when));
        EXPECT_TRUE(v.ok()) << when << ": " << v.message;
    }

    rmTree(dir);
}
