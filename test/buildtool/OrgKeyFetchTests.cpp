// publisher-trust Unit 3 — serving and fetching the key document (spec
// §6.1–6.4).
//
// The seam under test is deliberately narrow: a driver returns UNVERIFIED
// bytes, and one caller — OrgKeyCache — resolves roots, verifies, and
// remembers. Drivers hold no trust anchors, so there is no second place a
// verification could be skipped.
//
// The distinction these tests exist to protect is absence vs failure. Spec
// 5.4 degrades when a repository serves no document; degrading on a network
// error instead would turn an outage into a verification bypass.

#include "gtest/gtest.h"

#include "OrgKeyFixture.h"
#include "TempTeardown.h"
#include "TestHttpServer.h"

#include "cajeta/buildtool/OrgKeyCache.h"
#include "cajeta/buildtool/ReleaseMetadata.h"
#include "cajeta/buildtool/repo/FilesystemRepository.h"
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
    auto p = fs::temp_directory_path() / ("cajeta-orgkeys-" + tag);
    rmTree(p);
    fs::create_directories(p);
    return p;
}

std::time_t at(const char* stamp) { return *parseUtcTimestamp(stamp); }

// A driver that serves canned bytes and counts how often it was asked.
// Cheaper than a socket for the cache tests, and it makes the fetch count
// — the only evidence a cache hit actually happened — directly observable.
class StubRepository : public Repository {
public:
    StubRepository(std::string name, std::string document)
        : name_(std::move(name)), document_(std::move(document)) {}

    std::string name() const override { return name_; }
    // Deliberately NOT name_: a stub whose origin equals its label would let
    // a nickname/origin mix-up pass unnoticed.
    std::string origin() const override { return "https://stub.invalid/" + name_; }
    llvm::Expected<std::vector<std::string>> listVersions(
        const std::string&) const override {
        return std::vector<std::string>{};
    }
    llvm::Expected<std::string> fetch(const std::string&,
                                      const std::string&) const override {
        return std::string{};
    }
    llvm::Expected<std::optional<std::string>> fetchManifestJson(
        const std::string&, const std::string&) const override {
        return std::optional<std::string>{};
    }
    llvm::Expected<std::optional<std::string>> organizationKeys(
        const std::string&) const override {
        ++calls;
        if (document_.empty()) return std::optional<std::string>{};
        return std::optional<std::string>{document_};
    }

    void serve(std::string document) { document_ = std::move(document); }
    mutable int calls = 0;

private:
    std::string name_;
    std::string document_;
};

RootTrustLayout layoutWith(const fs::path& dir, const RootKey& shipped) {
    RootTrustLayout layout;
    layout.searchDirs = {(dir / "trust").string()};
    layout.shippedOverride = shipped;
    return layout;
}

}  // namespace

// 3.1.1 / spec 6.1 — the HTTP driver fetches a document from the endpoint
// the server contract defines.
TEST(OrgKeyFetchTests, theHttpDriverFetchesAnOrganizationKeyDocument) {
    auto dir = freshDir("http");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");
    std::string document = orgKeyDocument(dir, OrgDocumentSpec{}, orgKey,
                                          root, "olla-root-test");

    TestHttpServer srv;
    srv.route("/.well-known/cajeta-capabilities.json", 200,
              R"({"protocol-versions":["v1","v2"]})");
    srv.route("/v2/org-keys/dev.cajeta", 200, document);

    HttpRepository repo("central", srv.baseUrl(), RepositoryAuth{},
                        (dir / "stage").string());
    auto bytes = repo.organizationKeys("dev.cajeta");
    ASSERT_TRUE(!!bytes) << errText(bytes.takeError());
    ASSERT_TRUE(bytes->has_value()) << "the server serves a document here";
    EXPECT_EQ(document, **bytes)
        << "the driver must hand back the bytes verbatim — it holds no "
           "trust anchors and must not interpret them";

    // And it verifies, through the same path a real install takes.
    OrgKeyCache cache(layoutWith(dir, rootKeyOf(root, "olla-root-test")));
    auto doc = cache.documentFor(repo, "dev.cajeta", at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!doc) << errText(doc.takeError());
    ASSERT_TRUE(doc->has_value());
    EXPECT_EQ("dev.cajeta", (*doc)->organization);

    rmTree(dir);
}

// 3.1.2 — a local repository can participate rather than only being
// exempted from verification.
TEST(OrgKeyFetchTests, theFilesystemDriverReadsASidecarDocument) {
    auto dir = freshDir("fs");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");
    auto repoRoot = dir / "repo";
    writeWholeFile(repoRoot / ".well-known" / "org-keys" / "dev.cajeta.json",
                   orgKeyDocument(dir, OrgDocumentSpec{}, orgKey, root,
                                  "olla-root-test"));

    FilesystemRepository repo("local", repoRoot.string());
    OrgKeyCache cache(layoutWith(dir, rootKeyOf(root, "olla-root-test")));
    auto doc = cache.documentFor(repo, "dev.cajeta", at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!doc) << errText(doc.takeError());
    ASSERT_TRUE(doc->has_value());
    EXPECT_EQ("dev.cajeta", (*doc)->organization);

    rmTree(dir);
}

// 3.1.3 / spec 6.4 — absence is NOT an error, in every driver. The degrade
// path of 5.4 keys off exactly this distinction.
TEST(OrgKeyFetchTests, aRepositoryServingNoDocumentReportsAbsence) {
    auto dir = freshDir("absent");
    auto root = makeKeyPair(dir, "root");
    OrgKeyCache cache(layoutWith(dir, rootKeyOf(root, "olla-root-test")));

    // A filesystem repository with no sidecar.
    fs::create_directories(dir / "repo");
    FilesystemRepository local("local", (dir / "repo").string());
    auto none = cache.documentFor(local, "dev.cajeta", at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!none) << "a missing sidecar is not an error";
    EXPECT_FALSE(none->has_value());

    // A v2 server that 404s the org.
    TestHttpServer srv;
    srv.route("/.well-known/cajeta-capabilities.json", 200,
              R"({"protocol-versions":["v1","v2"]})");
    HttpRepository http("central", srv.baseUrl(), RepositoryAuth{},
                        (dir / "stage").string());
    auto missing = cache.documentFor(http, "dev.cajeta",
                                     at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!missing) << "a 404 is absence, not failure";
    EXPECT_FALSE(missing->has_value());

    // A v1-only server has no such surface at all — also absence.
    TestHttpServer v1;
    v1.route("/.well-known/cajeta-capabilities.json", 200,
             R"({"protocol-versions":["v1"]})");
    HttpRepository old("legacy", v1.baseUrl(), RepositoryAuth{},
                       (dir / "stage2").string());
    auto legacy = cache.documentFor(old, "dev.cajeta",
                                    at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!legacy) << "a v1-only server is the legacy path, not a fault";
    EXPECT_FALSE(legacy->has_value());

    rmTree(dir);
}

// The other half of 3.1.3, and the one that matters: a server that FAILS is
// not a server that serves nothing. Without this, a 500 from a hostile
// proxy would read as "no document" and take the degrade path.
TEST(OrgKeyFetchTests, aFailingRepositoryIsAnErrorNotAnAbsence) {
    auto dir = freshDir("failing");
    auto root = makeKeyPair(dir, "root");

    TestHttpServer srv;
    srv.route("/.well-known/cajeta-capabilities.json", 200,
              R"({"protocol-versions":["v1","v2"]})");
    srv.route("/v2/org-keys/dev.cajeta", 500, "upstream on fire");

    HttpRepository repo("central", srv.baseUrl(), RepositoryAuth{},
                        (dir / "stage").string());
    OrgKeyCache cache(layoutWith(dir, rootKeyOf(root, "olla-root-test")));
    auto doc = cache.documentFor(repo, "dev.cajeta", at("2026-06-01T00:00:00Z"));
    ASSERT_FALSE(!!doc) << "a 500 must not be reported as 'serves no document'";
    EXPECT_NE(std::string::npos, errText(doc.takeError()).find("500"));

    rmTree(dir);
}

// 3.1.4 / spec 6.3 — which root vouched for this document is an answer the
// client can give, not an assertion the document makes about itself.
TEST(OrgKeyFetchTests, theRootThatSignedADocumentIsDiscoverable) {
    auto dir = freshDir("rootid");
    auto shipped = makeKeyPair(dir, "shipped");
    auto mirror = makeKeyPair(dir, "mirror");
    auto orgKey = makeKeyPair(dir, "org");

    // The envelope CLAIMS the shipped root; the mirror root actually signed.
    std::string document = envelopeAround(
        dir, orgDocumentPayload(OrgDocumentSpec{}, orgKey), mirror,
        "olla-root-test", "m");

    auto layout = layoutWith(dir, rootKeyOf(shipped, "olla-root-test"));
    ASSERT_FALSE(!!addRootKey(layout, "mirror-root", mirror.pub.string()));

    StubRepository repo("central", document);
    OrgKeyCache cache(layout);
    auto doc = cache.documentFor(repo, "dev.cajeta", at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!doc) << errText(doc.takeError());
    ASSERT_TRUE(doc->has_value());
    EXPECT_EQ("mirror-root", (*doc)->rootKeyId)
        << "the reported root must be the one that VERIFIED, not the one the "
           "envelope named";

    rmTree(dir);
}

// A document that speaks for a different organization than the one asked
// about is refused. Without this a repository could answer every request
// with one org's document and lend out its namespaces.
TEST(OrgKeyFetchTests, aDocumentForAnotherOrganizationIsRefused) {
    auto dir = freshDir("wrongorg");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");

    OrgDocumentSpec spec;
    spec.organization = "dev.cajeta";
    StubRepository repo("central",
                        orgKeyDocument(dir, spec, orgKey, root, "r"));
    OrgKeyCache cache(layoutWith(dir, rootKeyOf(root, "r")));

    auto doc = cache.documentFor(repo, "com.attacker",
                                 at("2026-06-01T00:00:00Z"));
    ASSERT_FALSE(!!doc);
    auto msg = errText(doc.takeError());
    EXPECT_NE(std::string::npos, msg.find("com.attacker"));
    EXPECT_NE(std::string::npos, msg.find("dev.cajeta"));

    rmTree(dir);
}

// 3.2.3 / spec 2.5 — the cache is bounded by the document's OWN validity
// window. A second call inside the window is served from memory; a call
// past it goes back to the repository rather than serving something whose
// window has closed.
TEST(OrgKeyFetchTests, aCachedDocumentIsRefetchedOnceItsWindowCloses) {
    auto dir = freshDir("cache");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");

    OrgDocumentSpec spec;
    spec.notAfter = "2026-07-01T00:00:00Z";
    StubRepository repo("central",
                        orgKeyDocument(dir, spec, orgKey, root, "r", "d1"));
    OrgKeyCache cache(layoutWith(dir, rootKeyOf(root, "r")));

    auto first = cache.documentFor(repo, "dev.cajeta",
                                   at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!first) << errText(first.takeError());
    ASSERT_TRUE(first->has_value());
    EXPECT_EQ(1, repo.calls);

    auto second = cache.documentFor(repo, "dev.cajeta",
                                    at("2026-06-02T00:00:00Z"));
    ASSERT_TRUE(!!second);
    ASSERT_TRUE(second->has_value());
    EXPECT_EQ(1, repo.calls) << "still inside the window — served from cache";
    EXPECT_EQ(1, cache.fetches());

    // Past the window. The repository now serves a renewed document; the
    // client must ask for it rather than reuse what it holds.
    OrgDocumentSpec renewed;
    renewed.notAfter = "2027-07-01T00:00:00Z";
    repo.serve(orgKeyDocument(dir, renewed, orgKey, root, "r", "d2"));

    auto third = cache.documentFor(repo, "dev.cajeta",
                                   at("2026-08-01T00:00:00Z"));
    ASSERT_TRUE(!!third) << errText(third.takeError());
    ASSERT_TRUE(third->has_value());
    EXPECT_EQ(2, repo.calls) << "an expired entry must be refetched, never served";
    EXPECT_EQ(at("2027-07-01T00:00:00Z"), (*third)->notAfter);

    rmTree(dir);
}

// The expired-cache case where the repository has NOTHING newer: the client
// must refuse, not fall back on what it already had.
TEST(OrgKeyFetchTests, anExpiredCachedDocumentIsNotServedWhenNoRenewalExists) {
    auto dir = freshDir("stale");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");

    OrgDocumentSpec spec;
    spec.notAfter = "2026-07-01T00:00:00Z";
    StubRepository repo("central",
                        orgKeyDocument(dir, spec, orgKey, root, "r"));
    OrgKeyCache cache(layoutWith(dir, rootKeyOf(root, "r")));

    ASSERT_TRUE(!!cache.documentFor(repo, "dev.cajeta",
                                    at("2026-06-01T00:00:00Z")));

    auto later = cache.documentFor(repo, "dev.cajeta",
                                   at("2026-08-01T00:00:00Z"));
    ASSERT_FALSE(!!later) << "a stale document must not survive as a fallback";
    EXPECT_NE(std::string::npos, errText(later.takeError()).find("expired"));

    rmTree(dir);
}

// 3.2.2a / spec 6.2 — ownership arrives with the resolve the client already
// performs, inside the signed path. The plain fields beside the envelope are
// NOT merged: a mirror rewriting them changes nothing a verifier reads.
TEST(OrgKeyFetchTests, theOwningOrganizationArrivesInSignedResolveMetadata) {
    auto dir = freshDir("resolve");
    auto root = makeKeyPair(dir, "root");
    auto roots = std::vector<RootKey>{rootKeyOf(root, "olla-root-test")};

    std::string signedHalf = envelopeAround(
        dir,
        releasePayload("dev.cajeta.http", "1.0.0", "sha256:abc", "dev.cajeta"),
        root, "olla-root-test", "r");

    // The shape a v2 server serves: the plain body a non-verifying client
    // reads, plus the envelope a verifying one reads. Here the plain half
    // has been tampered with — it names a different organization and a
    // different hash.
    std::string body =
        R"({"sha256":"sha256:tampered","size":4,)"
        R"("organization":"com.attacker","signed":)" + signedHalf + "}";

    TestHttpServer srv;
    srv.route("/.well-known/cajeta-capabilities.json", 200,
              R"({"protocol-versions":["v1","v2"]})");
    srv.route("/v2/resolve?name=dev.cajeta.http&version=1.0.0", 200, body);

    HttpRepository repo("central", srv.baseUrl(), RepositoryAuth{},
                        (dir / "stage").string());
    auto raw = repo.releaseMetadataJson("dev.cajeta.http", "1.0.0");
    ASSERT_TRUE(!!raw) << errText(raw.takeError());
    ASSERT_TRUE(raw->has_value());

    auto md = loadReleaseMetadata(**raw, roots);
    ASSERT_TRUE(!!md) << errText(md.takeError());
    EXPECT_TRUE(md->signedByRoot);
    EXPECT_EQ("dev.cajeta", md->organization)
        << "the signed half is authoritative; the plain half is not merged";
    EXPECT_EQ("sha256:abc", md->sha256);
    EXPECT_EQ("olla-root-test", md->rootKeyId);

    rmTree(dir);
}

// A server that does not sign yields metadata flagged unsigned. It parses —
// it is useful for diagnostics — but `signedByRoot` is false, and that is
// the field the binding has to read.
TEST(OrgKeyFetchTests, unsignedResolveMetadataIsMarkedUnsigned) {
    auto dir = freshDir("unsigned");
    auto root = makeKeyPair(dir, "root");
    auto roots = std::vector<RootKey>{rootKeyOf(root, "olla-root-test")};

    auto md = loadReleaseMetadata(
        R"({"sha256":"cafe","organization":"com.attacker"})", roots);
    ASSERT_TRUE(!!md) << errText(md.takeError());
    EXPECT_FALSE(md->signedByRoot)
        << "an unsigned organization must never look like a signed one";
    EXPECT_EQ("sha256:cafe", md->sha256) << "the digest is normalised";

    rmTree(dir);
}

// A signed envelope that does not verify is an ERROR, not a fall-through to
// the plain fields beside it. Falling through would let a mirror strip a
// signature and get the unsigned path.
TEST(OrgKeyFetchTests, releaseMetadataSignedByAnUnknownRootIsRefused) {
    auto dir = freshDir("badroot");
    auto root = makeKeyPair(dir, "root");
    auto other = makeKeyPair(dir, "other");

    std::string signedHalf = envelopeAround(
        dir,
        releasePayload("dev.cajeta.http", "1.0.0", "sha256:abc", "dev.cajeta"),
        other, "olla-root-test", "r");
    std::string body = R"({"sha256":"sha256:abc","signed":)" + signedHalf + "}";

    auto md = loadReleaseMetadata(
        body, std::vector<RootKey>{rootKeyOf(root, "olla-root-test")});
    ASSERT_FALSE(!!md) << "an unverifiable envelope must not degrade to plain";
    EXPECT_NE(std::string::npos,
              errText(md.takeError()).find("does not match any trusted root"));

    rmTree(dir);
}

// An organization name reaches a URL path and a filesystem path. Neither
// may be escapable.
TEST(OrgKeyFetchTests, aTraversingOrganizationNameIsRefused) {
    auto dir = freshDir("traverse");
    fs::create_directories(dir / "repo");
    FilesystemRepository local("local", (dir / "repo").string());
    HttpRepository http("central", "http://127.0.0.1:1", RepositoryAuth{},
                        (dir / "stage").string());

    for (const char* hostile : {"../../etc/passwd", "..", "a/b", ""}) {
        auto l = local.organizationKeys(hostile);
        EXPECT_FALSE(!!l) << "filesystem accepted: " << hostile;
        if (!l) consumeError(l.takeError());

        auto h = http.organizationKeys(hostile);
        EXPECT_FALSE(!!h) << "http accepted: " << hostile;
        if (!h) consumeError(h.takeError());
    }

    rmTree(dir);
}
