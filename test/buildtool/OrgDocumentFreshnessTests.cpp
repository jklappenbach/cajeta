// publisher-trust §2.9, §2.10 — document freshness and the security contact.
//
// The organization document had no `issued-at`, so a mirror could serve a
// PREVIOUS, still-unexpired copy and reinstate every key the organization
// had removed. Removing a key is how §2.8's revocation repair works, which
// made this the hole that undid it.
//
// Two tests carry the suite. `anOlderDocumentIsRefused` is the mechanism,
// and `theCacheRefusesARolledBackDocument` is the one that matters more:
// the policy can be perfectly right inside loadOrgKeyDocument and never be
// reached. That is exactly how the retraction flag went unread for months.

#include "gtest/gtest.h"

#include "OrgKeyFixture.h"
#include "TempTeardown.h"

#include "cajeta/buildtool/OrgKeyCache.h"
#include "cajeta/buildtool/OrgKeyDocument.h"
#include "cajeta/buildtool/PublisherVerification.h"
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
    auto p = fs::temp_directory_path() / ("cajeta-freshness-" + tag);
    rmTree(p);
    fs::create_directories(p);
    return p;
}

std::time_t at(const char* stamp) { return *parseUtcTimestamp(stamp); }

}  // namespace

// 11.1.1 — required, not optional. An optional `issued-at` cannot be
// checked: a document without one would skip the comparison, which is the
// whole attack.
TEST(OrgDocumentFreshnessTests, aDocumentWithoutIssuedAtIsRefused) {
    auto dir = freshDir("required");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");

    OrgDocumentSpec spec;
    spec.issuedAt = "";                 // omitted entirely
    auto doc = loadOrgKeyDocument(orgKeyDocument(dir, spec, orgKey, root, "r"),
                                  {rootKeyOf(root, "r")},
                                  at("2026-06-01T00:00:00Z"), 0);
    ASSERT_FALSE(!!doc);
    EXPECT_NE(std::string::npos, errText(doc.takeError()).find("issued-at"));
    rmTree(dir);
}

// 11.1.2 — THE test. Validly root-signed, inside its own window, and still
// refused because something newer has been seen.
TEST(OrgDocumentFreshnessTests, anOlderDocumentIsRefused) {
    auto dir = freshDir("rollback");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");

    OrgDocumentSpec spec;
    spec.issuedAt = "2026-01-01T00:00:00Z";
    std::string older = orgKeyDocument(dir, spec, orgKey, root, "r");

    auto refused = loadOrgKeyDocument(older, {rootKeyOf(root, "r")},
                                      at("2026-06-01T00:00:00Z"),
                                      at("2026-03-01T00:00:00Z"));
    ASSERT_FALSE(!!refused)
        << "a replayed document is validly signed and unexpired — only its "
           "age distinguishes it";
    EXPECT_NE(std::string::npos, errText(refused.takeError()).find("older"));

    // 11.1.3 — the other half. The same bytes are fine when nothing newer
    // has been seen; a rule that always fires is not a rule.
    auto accepted = loadOrgKeyDocument(older, {rootKeyOf(root, "r")},
                                       at("2026-06-01T00:00:00Z"), 0);
    EXPECT_TRUE(!!accepted) << errText(accepted.takeError());
    rmTree(dir);
}

// 11.1.4 — the policy is reached. A correct check nothing calls is the
// failure mode this project has already shipped once.
TEST(OrgDocumentFreshnessTests, theCacheRefusesARolledBackDocument) {
    auto dir = freshDir("cache");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");
    auto repoRoot = dir / "repo";
    fs::create_directories(repoRoot / ".well-known" / "org-keys");
    auto docPath = repoRoot / ".well-known" / "org-keys" / "dev.cajeta.json";

    auto write = [&](const std::string& issuedAt, const std::string& notAfter,
                     const std::string& keyId) {
        OrgDocumentSpec spec;
        spec.issuedAt = issuedAt;
        spec.notAfter = notAfter;
        spec.keyId = keyId;
        writeWholeFile(docPath, orgKeyDocument(dir, spec, orgKey, root, "r",
                                               "doc-" + issuedAt));
    };

    RootTrustLayout layout;
    layout.searchDirs = {(dir / "trust").string()};
    layout.shippedOverride = rootKeyOf(root, "r");
    OrgKeyCache cache(layout);
    auto now = at("2026-06-01T00:00:00Z");
    FilesystemRepository repo("local", repoRoot.string());

    // A short-lived current document, so the cache entry expires and the
    // client goes back to the repository. A refetch is exactly when a
    // mirror gets to hand back something older — while the entry is still
    // cached it is never asked, so there is nothing to roll back.
    write("2026-05-01T00:00:00Z", "2026-06-01T01:00:00Z", "k-new");
    auto first = cache.documentFor(repo, "dev.cajeta", now);
    ASSERT_TRUE(!!first) << errText(first.takeError());
    ASSERT_TRUE(first->has_value());

    // The mirror swaps in last quarter's document, which still lists the
    // key that was rotated out. It is long-lived, so only its AGE marks it.
    write("2026-01-01T00:00:00Z", "2030-01-01T00:00:00Z", "k-old");
    auto rolled = cache.documentFor(repo, "dev.cajeta",
                                    at("2026-06-01T02:00:00Z"));
    EXPECT_FALSE(!!rolled)
        << "the cache must apply the freshness rule, not merely have one "
           "available in the parser below it — and the high-water mark has "
           "to outlive the evicted document, or every expiry reopens the "
           "replay";
    if (!rolled) consumeError(rolled.takeError());
    rmTree(dir);
}

// 11.1.5 — the contact parses, and its absence is not an error.
TEST(OrgDocumentFreshnessTests, theSecurityContactIsOptionalAndParses) {
    auto dir = freshDir("contact");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");
    auto now = at("2026-06-01T00:00:00Z");

    OrgDocumentSpec none;
    auto without = loadOrgKeyDocument(orgKeyDocument(dir, none, orgKey, root, "r"),
                                      {rootKeyOf(root, "r")}, now, 0);
    ASSERT_TRUE(!!without) << errText(without.takeError());
    EXPECT_TRUE(without->securityContact.uri.empty());

    OrgDocumentSpec with;
    with.securityContactUri = "mailto:security@cajeta.dev";
    with.securityContactLabel = "Cajeta security team";
    auto doc = loadOrgKeyDocument(orgKeyDocument(dir, with, orgKey, root, "r", "c"),
                                  {rootKeyOf(root, "r")}, now, 0);
    ASSERT_TRUE(!!doc) << errText(doc.takeError());
    EXPECT_EQ("mailto:security@cajeta.dev", doc->securityContact.uri);
    EXPECT_EQ("Cajeta security team", doc->securityContact.label);
    rmTree(dir);
}

// 11.1.6 — a contact that half-parses is worse than none: it looks
// authoritative and points nowhere.
TEST(OrgDocumentFreshnessTests, aContactWithoutAUriIsRefused) {
    auto dir = freshDir("badcontact");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");

    OrgDocumentSpec spec;
    std::string payload = orgDocumentPayload(spec, orgKey);
    // Splice in a contact object with only a label.
    auto brace = payload.rfind('}');
    payload.insert(brace, R"(,"security-contact":{"label":"no uri here"})");

    auto doc = loadOrgKeyDocument(envelopeAround(dir, payload, root, "r", "bad"),
                                  {rootKeyOf(root, "r")},
                                  at("2026-06-01T00:00:00Z"), 0);
    ASSERT_FALSE(!!doc);
    EXPECT_NE(std::string::npos, errText(doc.takeError()).find("uri"));
    rmTree(dir);
}

// 11.1.7 — the contact reaches the user. A signed field nothing surfaces
// is worse than no field: it looks authoritative and is never read.
TEST(OrgDocumentFreshnessTests, aRefusalCarriesTheSecurityContact) {
    auto dir = freshDir("surfaced");
    auto root = makeKeyPair(dir, "root");
    auto orgKey = makeKeyPair(dir, "org");
    auto now = at("2026-06-01T00:00:00Z");

    OrgDocumentSpec spec;
    spec.securityContactUri = "mailto:security@cajeta.dev";
    spec.securityContactLabel = "Cajeta security team";
    auto doc = loadOrgKeyDocument(orgKeyDocument(dir, spec, orgKey, root, "r"),
                                  {rootKeyOf(root, "r")}, now, 0);
    ASSERT_TRUE(!!doc) << errText(doc.takeError());

    auto artifact = dir / "a.cja";
    writeWholeFile(artifact, "bytes");

    // A name this organization does not own — any refusal will do.
    auto verdict = verifyAgainstOrgDocument(*doc, "com.other.thing",
                                            artifact.string(), "sig", now,
                                            nullptr);
    ASSERT_FALSE(verdict.ok());
    EXPECT_NE(std::string::npos,
              verdict.message.find("mailto:security@cajeta.dev"))
        << verdict.message;
    EXPECT_NE(std::string::npos, verdict.message.find("Cajeta security team"));

    // And a document without one says nothing extra, rather than printing
    // an empty contact line.
    OrgDocumentSpec bare;
    auto plain = loadOrgKeyDocument(orgKeyDocument(dir, bare, orgKey, root, "r", "b"),
                                    {rootKeyOf(root, "r")}, now, 0);
    ASSERT_TRUE(!!plain);
    auto quiet = verifyAgainstOrgDocument(*plain, "com.other.thing",
                                          artifact.string(), "sig", now,
                                          nullptr);
    EXPECT_EQ(std::string::npos, quiet.message.find("Report a problem"));

    rmTree(dir);
}
