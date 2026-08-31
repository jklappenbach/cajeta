// publisher-trust Unit 4 — binding an artifact to its publisher (spec §4).
//
// Every test here signs real bytes with a real ed25519 key. The point of
// the unit is that a good signature is NOT sufficient: the key has to come
// from the owning organization's document, be inside its window, and the
// name has to fall inside the namespaces that document claims. So most of
// these are refusals where the signature itself is perfectly valid.

#include "gtest/gtest.h"

#include "OrgKeyFixture.h"
#include "TempTeardown.h"

#include "cajeta/buildtool/PublisherVerification.h"

#include <filesystem>
#include <string>

using namespace cajeta::buildtool;
using namespace cajeta::buildtool::testing;
namespace fs = std::filesystem;

namespace {

fs::path freshDir(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("cajeta-pubverify-" + tag);
    rmTree(p);
    fs::create_directories(p);
    return p;
}

std::time_t at(const char* stamp) { return *parseUtcTimestamp(stamp); }

// A document parsed the ordinary way, so these tests exercise the same
// value the fetch path produces rather than a hand-built struct.
OrgKeyDocument documentOf(const fs::path& dir, const OrgDocumentSpec& spec,
                          const TestKeyPair& orgKey, const TestKeyPair& root,
                          std::time_t now, const std::string& tag = "d") {
    auto doc = loadOrgKeyDocument(
        orgKeyDocument(dir, spec, orgKey, root, "r", tag),
        {rootKeyOf(root, "r")}, now);
    EXPECT_TRUE(!!doc);
    if (!doc) {
        consumeError(doc.takeError());
        return OrgKeyDocument{};
    }
    return *doc;
}

struct Artifact {
    fs::path path;
    std::string signature;
};

Artifact signedArtifact(const fs::path& dir, const TestKeyPair& key,
                        const std::string& tag = "a") {
    Artifact a;
    a.path = dir / (tag + ".cja");
    writeWholeFile(a.path, "pretend this is an archive: " + tag);
    a.signature = signWithKey(dir, readWholeFile(a.path), key.priv,
                              tag + "-sig");
    return a;
}

}  // namespace

// 4.1.3a / spec 4.3.1 — the case a naive `starts_with` gets wrong, written
// first because it passes every OTHER test in this unit.
TEST(PublisherVerificationTests, namespaceMatchingIsSegmentAware) {
    EXPECT_TRUE(namespaceOwns("dev.cajeta", "dev.cajeta"));
    EXPECT_TRUE(namespaceOwns("dev.cajeta", "dev.cajeta.http"));
    EXPECT_TRUE(namespaceOwns("dev.cajeta", "dev.cajeta.http.client"));

    EXPECT_FALSE(namespaceOwns("dev.cajeta", "dev.cajetaevil"))
        << "the separator is the whole check — an attacker picks the name";
    EXPECT_FALSE(namespaceOwns("dev.cajeta", "dev.cajet"));
    EXPECT_FALSE(namespaceOwns("dev.cajeta", "com.dev.cajeta"))
        << "a namespace owns a prefix position, not a substring";
    EXPECT_FALSE(namespaceOwns("dev.cajeta", ""));
    EXPECT_FALSE(namespaceOwns("", "dev.cajeta"));
}

// 4.1.1 / spec 4.1 — the ordinary case.
TEST(PublisherVerificationTests, anArtifactSignedByAValidOrgKeyVerifies) {
    auto dir = freshDir("ok");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");
    auto now = at("2026-06-01T00:00:00Z");

    auto doc = documentOf(dir, OrgDocumentSpec{}, orgKey, root, now);
    auto artifact = signedArtifact(dir, orgKey);

    auto v = verifyAgainstOrgDocument(doc, "dev.cajeta.http", artifact.path.string(),
                                      artifact.signature, now, nullptr);
    EXPECT_TRUE(v.ok()) << v.message;
    EXPECT_EQ("k1", v.keyId);
    EXPECT_EQ("dev.cajeta", v.organization);

    rmTree(dir);
}

// 4.1.2 / spec 4.2 — a key valid for a DIFFERENT organization. The bytes
// carry a real signature by a real key; it is simply not this org's, and
// that is the binding the whole spec exists to create.
TEST(PublisherVerificationTests, aKeyFromAnotherOrganizationIsRefused) {
    auto dir = freshDir("otherorg");
    auto root = makeKeyPair(dir, "root");
    auto ours = makeKeyPair(dir, "ours");
    auto theirs = makeKeyPair(dir, "theirs");
    auto now = at("2026-06-01T00:00:00Z");

    auto doc = documentOf(dir, OrgDocumentSpec{}, ours, root, now);
    auto artifact = signedArtifact(dir, theirs);

    auto v = verifyAgainstOrgDocument(doc, "dev.cajeta.http", artifact.path.string(),
                                      artifact.signature, now, nullptr);
    EXPECT_FALSE(v.ok());
    EXPECT_EQ(PublisherCheck::Signature, v.check);
    EXPECT_NE(std::string::npos, v.message.find("does not match"));

    rmTree(dir);
}

// 4.1.3 / spec 4.3 — outside the claimed namespaces, refused even though
// the signature is this organization's and verifies perfectly.
TEST(PublisherVerificationTests, aNameOutsideTheNamespacesIsRefused) {
    auto dir = freshDir("namespace");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");
    auto now = at("2026-06-01T00:00:00Z");

    auto doc = documentOf(dir, OrgDocumentSpec{}, orgKey, root, now);
    auto artifact = signedArtifact(dir, orgKey);

    // Same signature that verified in the ordinary case above.
    auto v = verifyAgainstOrgDocument(doc, "com.someoneelse.thing",
                                      artifact.path.string(), artifact.signature,
                                      now, nullptr);
    EXPECT_FALSE(v.ok());
    EXPECT_EQ(PublisherCheck::Namespace, v.check);

    // And the adversarial neighbour, end to end rather than only through
    // the predicate.
    auto evil = verifyAgainstOrgDocument(doc, "dev.cajetaevil",
                                         artifact.path.string(),
                                         artifact.signature, now, nullptr);
    EXPECT_FALSE(evil.ok());
    EXPECT_EQ(PublisherCheck::Namespace, evil.check);

    rmTree(dir);
}

// 4.1.4 / spec 4.1 — the key must be valid AT THE TIME OF VERIFICATION.
// The document is current; the key inside it is not.
TEST(PublisherVerificationTests, aSignatureByAnExpiredKeyIsRefused) {
    auto dir = freshDir("expiredkey");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");
    auto now = at("2026-06-01T00:00:00Z");

    OrgDocumentSpec spec;
    spec.notAfter = "2030-01-01T00:00:00Z";      // the document is fine
    spec.keyNotBefore = "2020-01-01T00:00:00Z";
    spec.keyNotAfter = "2025-01-01T00:00:00Z";   // the key is not
    auto doc = documentOf(dir, spec, orgKey, root, now);
    auto artifact = signedArtifact(dir, orgKey);

    auto v = verifyAgainstOrgDocument(doc, "dev.cajeta.http",
                                      artifact.path.string(), artifact.signature,
                                      now, nullptr);
    EXPECT_FALSE(v.ok());
    EXPECT_EQ(PublisherCheck::NoUsableKey, v.check)
        << "an out-of-window key must read as 'cannot verify', never as a "
           "clean signature mismatch";

    // The same artifact, the same document, verified at a time the key WAS
    // valid: this shows the refusal is the window and nothing else.
    auto earlier = verifyAgainstOrgDocument(doc, "dev.cajeta.http",
                                            artifact.path.string(),
                                            artifact.signature,
                                            at("2024-06-01T00:00:00Z"), nullptr);
    EXPECT_TRUE(earlier.ok()) << earlier.message;

    rmTree(dir);
}

// 2.6 through this path — rotation without a flag day. Two keys with
// overlapping windows, and a signature by either is accepted.
TEST(PublisherVerificationTests, eitherOfTwoOverlappingKeysVerifies) {
    auto dir = freshDir("rotate");
    auto root = makeKeyPair(dir, "root");
    auto outgoing = makeKeyPair(dir, "outgoing");
    auto incoming = makeKeyPair(dir, "incoming");
    auto now = at("2026-06-01T00:00:00Z");

    // Hand-build the two-key payload: the fixture's spec carries one key,
    // and the overlap is exactly what is under test.
    std::ostringstream payload;
    payload << "{\"organization\":\"dev.cajeta\","
            << "\"namespaces\":[\"dev.cajeta\"],"
            << "\"issued-at\":\"2026-01-01T00:00:00Z\","
            << "\"not-after\":\"2030-01-01T00:00:00Z\",\"keys\":["
            << "{\"id\":\"outgoing\",\"algorithm\":\"ed25519\",\"public-key\":\""
            << jsonEscapePem(readWholeFile(outgoing.pub))
            << "\",\"not-before\":\"2020-01-01T00:00:00Z\","
            << "\"not-after\":\"2027-01-01T00:00:00Z\"},"
            << "{\"id\":\"incoming\",\"algorithm\":\"ed25519\",\"public-key\":\""
            << jsonEscapePem(readWholeFile(incoming.pub))
            << "\",\"not-before\":\"2026-01-01T00:00:00Z\","
            << "\"not-after\":\"2030-01-01T00:00:00Z\"}]}";

    auto parsed = loadOrgKeyDocument(
        envelopeAround(dir, payload.str(), root, "r", "two"),
        {rootKeyOf(root, "r")}, now);
    ASSERT_TRUE(!!parsed);

    for (const auto* key : {&outgoing, &incoming}) {
        auto artifact = signedArtifact(dir, *key,
                                       key == &outgoing ? "out" : "in");
        auto v = verifyAgainstOrgDocument(*parsed, "dev.cajeta.http",
                                          artifact.path.string(),
                                          artifact.signature, now, nullptr);
        EXPECT_TRUE(v.ok()) << v.message;
    }

    rmTree(dir);
}

// 4.1.3b / spec 4.4 — no arity assumption. `uk.co.acme.thing` and
// `io.foo.bar` place the org/name boundary differently, and both verify,
// because the boundary is never computed from the name.
TEST(PublisherVerificationTests, dottedNamesCarryNoArityAssumption) {
    auto dir = freshDir("arity");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");
    auto now = at("2026-06-01T00:00:00Z");

    struct Case { const char* org; const char* ns; const char* artifact; };
    const Case cases[] = {
        {"uk.co.acme", "uk.co.acme", "uk.co.acme.thing"},   // three segments
        {"io.foo", "io.foo", "io.foo.bar"},                 // two
        {"acme", "acme", "acme.widget"},                    // one
    };

    int i = 0;
    for (const auto& c : cases) {
        OrgDocumentSpec spec;
        spec.organization = c.org;
        spec.namespaces = {c.ns};
        auto tag = "case" + std::to_string(i++);
        auto doc = documentOf(dir, spec, orgKey, root, now, tag);
        auto artifact = signedArtifact(dir, orgKey, tag);

        auto v = verifyAgainstOrgDocument(doc, c.artifact, artifact.path.string(),
                                          artifact.signature, now, nullptr);
        EXPECT_TRUE(v.ok()) << c.artifact << ": " << v.message;
    }

    rmTree(dir);
}

// 4.3.1 — each failure names WHICH check decided it. "Verification failed"
// sends a reader nowhere; these have to be actionable and distinguishable.
TEST(PublisherVerificationTests, failureTextsNameWhichCheckFailed) {
    auto dir = freshDir("messages");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");
    auto other = makeKeyPair(dir, "other");
    auto now = at("2026-06-01T00:00:00Z");

    auto doc = documentOf(dir, OrgDocumentSpec{}, orgKey, root, now);
    auto mine = signedArtifact(dir, orgKey, "mine");
    auto theirs = signedArtifact(dir, other, "theirs");

    auto ns = verifyAgainstOrgDocument(doc, "com.elsewhere.thing",
                                       mine.path.string(), mine.signature, now, nullptr);
    EXPECT_NE(std::string::npos, ns.message.find("outside the namespaces"));
    EXPECT_NE(std::string::npos, ns.message.find("dev.cajeta"))
        << "the message must name the organization and what it owns";

    auto sig = verifyAgainstOrgDocument(doc, "dev.cajeta.http",
                                        theirs.path.string(), theirs.signature,
                                        now, nullptr);
    EXPECT_NE(std::string::npos, sig.message.find("does not match"));

    OrgDocumentSpec stale;
    stale.keyNotAfter = "2025-01-01T00:00:00Z";
    auto staleDoc = documentOf(dir, stale, orgKey, root, now, "stale");
    auto win = verifyAgainstOrgDocument(staleDoc, "dev.cajeta.http",
                                        mine.path.string(), mine.signature, now, nullptr);
    EXPECT_NE(std::string::npos, win.message.find("validity window"));

    // Three different failures, three different texts.
    EXPECT_NE(ns.message, sig.message);
    EXPECT_NE(sig.message, win.message);

    rmTree(dir);
}

// A key the document carries that is not a readable ed25519 key must read
// as "we could not check", not as a clean mismatch. The two demand
// different responses from an operator.
TEST(PublisherVerificationTests, anUnreadableKeyIsNotACleanMismatch) {
    auto dir = freshDir("unreadable");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");
    auto now = at("2026-06-01T00:00:00Z");

    auto doc = documentOf(dir, OrgDocumentSpec{}, orgKey, root, now);
    ASSERT_EQ(1u, doc.keys.size());
    doc.keys[0].publicKeyPem = "-----BEGIN PUBLIC KEY-----\nnot a key\n"
                               "-----END PUBLIC KEY-----\n";

    auto artifact = signedArtifact(dir, orgKey);
    auto v = verifyAgainstOrgDocument(doc, "dev.cajeta.http",
                                      artifact.path.string(), artifact.signature,
                                      now, nullptr);
    EXPECT_FALSE(v.ok());
    EXPECT_EQ(PublisherCheck::Unreadable, v.check);
    EXPECT_NE(std::string::npos, v.message.find("could not be checked"));

    rmTree(dir);
}
