// publisher-trust Unit 2 — the trust anchor (spec §3).
//
// The shipped root is INJECTED in most of these rather than used directly.
// That is not a convenience: signing as the production root would require
// its private key, and a test that could do that would be evidence the key
// was somewhere it must never be. What the tests exercise is the mechanism —
// "a root that needs no operator action" — plus one check that the embedded
// key is really there and really parses.

#include "gtest/gtest.h"
#include "../PortableEnv.h"

#include "cajeta/buildtool/OrgKeyDocument.h"
#include "cajeta/buildtool/RootTrust.h"
#include "cajeta/buildtool/Signature.h"

#include <llvm/Support/Base64.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace cajeta::buildtool;
namespace fs = std::filesystem;

namespace {

int sh(const std::string& cmd) {
    int rc = std::system((cmd + " > /dev/null 2>&1").c_str());
#ifdef _WIN32
    return rc;
#else
    return WEXITSTATUS(rc);
#endif
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

fs::path freshDir(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("cajeta-root-trust-" + tag);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

struct KeyPair { fs::path priv, pub; };

KeyPair makeKey(const fs::path& dir, const std::string& tag) {
    KeyPair kp{dir / (tag + ".key"), dir / (tag + ".pub")};
    EXPECT_EQ(0, sh("openssl genpkey -algorithm ED25519 -out " + kp.priv.string()));
    EXPECT_EQ(0, sh("openssl pkey -in " + kp.priv.string() + " -pubout -out "
                    + kp.pub.string()));
    return kp;
}

RootKey rootOf(const KeyPair& kp, const std::string& id) {
    RootKey r;
    r.id = id;
    r.pem = readFile(kp.pub);
    return r;
}

std::string signBytes(const fs::path& dir, const std::string& data,
                      const fs::path& priv, const std::string& tag) {
    auto in = dir / (tag + ".in"), out = dir / (tag + ".sig");
    std::ofstream(in, std::ios::binary) << data;
    EXPECT_EQ(0, sh("openssl pkeyutl -sign -rawin -inkey " + priv.string()
                    + " -in " + in.string() + " -out " + out.string()));
    return readFile(out);
}

std::string escapePem(const std::string& pem) {
    std::string out;
    for (char c : pem) {
        if (c == '\n') out += "\\n";
        else if (c != '\r') out += c;
    }
    return out;
}

// A minimal valid document for `org`, signed by `signer`.
std::string documentFor(const fs::path& dir, const KeyPair& signer,
                        const KeyPair& orgKey, const std::string& tag) {
    std::ostringstream payload;
    payload << "{\"organization\":\"dev.cajeta\","
            << "\"namespaces\":[\"dev.cajeta\"],"
            << "\"issued-at\":\"2026-01-01T00:00:00Z\","
            << "\"not-after\":\"2030-01-01T00:00:00Z\","
            << "\"keys\":[{\"id\":\"k1\",\"algorithm\":\"ed25519\","
            << "\"public-key\":\"" << escapePem(readFile(orgKey.pub)) << "\","
            << "\"not-before\":\"2020-01-01T00:00:00Z\","
            << "\"not-after\":\"2030-01-01T00:00:00Z\"}]}";
    std::string body = payload.str();
    std::string sig = signBytes(dir, body, signer.priv, tag);
    std::ostringstream env;
    env << "{\"format\":1,\"root-key-id\":\"whatever\","
        << "\"payload\":\"" << llvm::encodeBase64(body) << "\","
        << "\"signature\":\"" << llvm::encodeBase64(sig) << "\"}";
    return env.str();
}

std::time_t now() { return *parseUtcTimestamp("2026-06-01T00:00:00Z"); }

}  // namespace

// 2.1.1 / spec 3.1 — the shipped root works with NO operator action: an
// empty trust store still yields a usable anchor.
TEST(RootTrustTests, theShippedRootNeedsNoOperatorAction) {
    auto dir = freshDir("shipped");
    auto shipped = makeKey(dir, "shipped");
    auto org = makeKey(dir, "org");

    RootTrustLayout layout;
    layout.searchDirs = {(dir / "trust").string()};   // nothing installed
    layout.shippedOverride = rootOf(shipped, "olla-root-test");

    auto roots = rootsFor(layout, "central");
    ASSERT_TRUE(!!roots) << llvm::toString(roots.takeError());
    ASSERT_EQ(1u, roots->size()) << "the shipped root should stand alone";
    EXPECT_EQ("olla-root-test", (*roots)[0].id);

    auto doc = loadOrgKeyDocument(documentFor(dir, shipped, org, "s"), *roots,
                                  now());
    EXPECT_TRUE(!!doc) << "a default install must verify without setup";
    if (!doc) llvm::consumeError(doc.takeError());

    fs::remove_all(dir);
}

// The embedded anchor is really present and really an ed25519 key. Cheap,
// and the thing that catches an embed that silently produced an empty
// string — which would make every verification "no trusted root" with no
// hint as to why.
TEST(RootTrustTests, theEmbeddedRootIsPresentAndParses) {
    const RootKey& root = shippedRoot();
    EXPECT_FALSE(root.id.empty()) << "the embedded root has no id";
    EXPECT_NE(std::string::npos, root.pem.find("BEGIN PUBLIC KEY"))
        << "the embedded root is not a PEM public key";
    EXPECT_TRUE(root.shipped);

    // Not a valid signature, but it must fail as "did not verify" rather
    // than as "unusable key" — which is what an unparseable PEM would give.
    auto ok = verifyDetachedEd25519PemBytes("data", "not-a-signature", root.pem);
    EXPECT_TRUE(!!ok) << "the embedded root does not parse as an ed25519 key";
    if (ok) EXPECT_FALSE(*ok);
    else llvm::consumeError(ok.takeError());
}

// 2.1.2 / spec 3.3 — an operator root verifies what the shipped one cannot.
TEST(RootTrustTests, anOperatorRootVerifiesAPrivateRepository) {
    auto dir = freshDir("operator");
    auto shipped = makeKey(dir, "shipped");
    auto mirror = makeKey(dir, "mirror");
    auto org = makeKey(dir, "org");

    RootTrustLayout layout;
    layout.searchDirs = {(dir / "trust").string()};
    layout.shippedOverride = rootOf(shipped, "olla-root-test");

    // Signed by the mirror's root: refused before it is installed.
    std::string envelope = documentFor(dir, mirror, org, "m");
    auto before = rootsFor(layout, "mirror");
    ASSERT_TRUE(!!before);
    auto refused = loadOrgKeyDocument(envelope, *before, now());
    EXPECT_FALSE(!!refused) << "an unknown root must not verify";
    if (!refused) llvm::consumeError(refused.takeError());

    ASSERT_FALSE(!!addRootKey(layout, "mirror-root", mirror.pub.string()));

    auto after = rootsFor(layout, "mirror");
    ASSERT_TRUE(!!after);
    EXPECT_EQ(2u, after->size()) << "an added root is ADDITIVE to the shipped "
                                    "one, not a replacement";
    auto accepted = loadOrgKeyDocument(envelope, *after, now());
    EXPECT_TRUE(!!accepted) << "the installed root should now verify";
    if (!accepted) llvm::consumeError(accepted.takeError());

    fs::remove_all(dir);
}

// 2.1.3 / spec 3.3 — a pin NARROWS, even against an otherwise trusted root.
TEST(RootTrustTests, aPinnedRepositoryRejectsAnotherTrustedRoot) {
    auto dir = freshDir("pin");
    auto shipped = makeKey(dir, "shipped");
    auto mirror = makeKey(dir, "mirror");
    auto org = makeKey(dir, "org");

    RootTrustLayout layout;
    layout.searchDirs = {(dir / "trust").string()};
    layout.shippedOverride = rootOf(shipped, "olla-root-test");
    ASSERT_FALSE(!!addRootKey(layout, "mirror-root", mirror.pub.string()));

    // Unpinned, a document signed by EITHER root verifies.
    auto both = rootsFor(layout, "central");
    ASSERT_TRUE(!!both);
    EXPECT_EQ(2u, both->size());

    ASSERT_FALSE(!!pinRepository(layout, "central", "olla-root-test"));
    auto pinned = rootsFor(layout, "central");
    ASSERT_TRUE(!!pinned);
    ASSERT_EQ(1u, pinned->size()) << "a pin restricts to exactly one root";
    EXPECT_EQ("olla-root-test", (*pinned)[0].id);

    // The mirror root is still trusted for OTHER repositories, and is still
    // refused for this one. That asymmetry is the whole point of a pin.
    auto elsewhere = rootsFor(layout, "mirror");
    ASSERT_TRUE(!!elsewhere);
    EXPECT_EQ(2u, elsewhere->size());

    auto refused = loadOrgKeyDocument(documentFor(dir, mirror, org, "p"),
                                      *pinned, now());
    EXPECT_FALSE(!!refused)
        << "a pinned repository must refuse a root it is not pinned to, even "
           "one the machine trusts elsewhere";
    if (!refused) llvm::consumeError(refused.takeError());

    fs::remove_all(dir);
}

// A pin naming a root that is not installed FAILS rather than falling back.
// Falling back would invert the operator's intent at the one moment it
// matters.
TEST(RootTrustTests, anUnhonourablePinIsAnErrorNotAWidening) {
    auto dir = freshDir("badpin");
    auto shipped = makeKey(dir, "shipped");

    RootTrustLayout layout;
    layout.searchDirs = {(dir / "trust").string()};
    layout.shippedOverride = rootOf(shipped, "olla-root-test");
    ASSERT_FALSE(!!pinRepository(layout, "central", "root-that-is-not-here"));

    auto roots = rootsFor(layout, "central");
    EXPECT_FALSE(!!roots)
        << "an unhonourable pin must refuse, never silently verify against "
           "everything";
    if (!roots) llvm::consumeError(roots.takeError());

    // Clearing the pin restores the normal set.
    ASSERT_FALSE(!!pinRepository(layout, "central", ""));
    auto cleared = rootsFor(layout, "central");
    ASSERT_TRUE(!!cleared);
    EXPECT_EQ(1u, cleared->size());

    fs::remove_all(dir);
}

// The shipped root is part of the binary, so removing it would be a lie the
// next lookup undoes.
TEST(RootTrustTests, theShippedRootCannotBeRemoved) {
    auto dir = freshDir("rm");
    RootTrustLayout layout;
    layout.searchDirs = {(dir / "trust").string()};

    auto e = removeRootKey(layout, shippedRoot().id);
    EXPECT_TRUE(!!e) << "removing the shipped root must be refused";
    if (e) llvm::consumeError(std::move(e));

    fs::remove_all(dir);
}

// A repository name reaches the filesystem when a pin is read or written.
// It must not be able to escape the trust store.
TEST(RootTrustTests, aTraversingNameCannotEscapeTheTrustStore) {
    auto dir = freshDir("traverse");
    RootTrustLayout layout;
    layout.searchDirs = {(dir / "trust").string()};

    for (const char* hostile : {"../../etc/passwd", "..", "a/b", ""}) {
        auto e = pinRepository(layout, hostile, "some-root");
        EXPECT_TRUE(!!e) << "should have refused the name: " << hostile;
        if (e) llvm::consumeError(std::move(e));
        EXPECT_FALSE(pinFor(layout, hostile).has_value());
    }
    EXPECT_FALSE(fs::exists(dir / "trust" / "pins" / ".." / ".." / "etc"));

    fs::remove_all(dir);
}
