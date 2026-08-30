// publisher-trust Unit 1 — the organization key document (spec §2).
//
// Pure data: parse, validate, verify. No network, no install path.
//
// The fixtures sign with a real ed25519 key via the openssl CLI rather than
// with canned bytes, so the format stays honest: if the payload framing ever
// drifts from what a signer actually produces, these fail.

#include "gtest/gtest.h"

#include "cajeta/buildtool/OrgKeyDocument.h"

#include <llvm/Support/Base64.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace cajeta::buildtool;
namespace fs = std::filesystem;

namespace {

fs::path workDir() {
    static fs::path dir = [] {
        auto p = fs::temp_directory_path() / "cajeta-org-key-doc";
        fs::remove_all(p);
        fs::create_directories(p);
        return p;
    }();
    return dir;
}

int sh(const std::string& cmd) {
    int rc = std::system((cmd + " > /dev/null 2>&1").c_str());
#ifdef _WIN32
    return rc;
#else
    return WEXITSTATUS(rc);
#endif
}

void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// A keypair, generated once. `tag` names it so a test can hold two.
struct KeyPair {
    fs::path priv;
    fs::path pub;
};

KeyPair keyPair(const std::string& tag) {
    KeyPair kp{workDir() / (tag + ".key.pem"), workDir() / (tag + ".pub.pem")};
    if (!fs::exists(kp.priv)) {
        EXPECT_EQ(0, sh("openssl genpkey -algorithm ED25519 -out "
                        + kp.priv.string()));
        EXPECT_EQ(0, sh("openssl pkey -in " + kp.priv.string()
                        + " -pubout -out " + kp.pub.string()));
    }
    return kp;
}

// Sign raw bytes with an ed25519 private key. ed25519 is single-shot, so
// `-rawin` is the correct mode; there is no pre-hash.
std::string signBytes(const std::string& data, const fs::path& priv,
                      const std::string& tag) {
    auto in = workDir() / (tag + ".payload.bin");
    auto out = workDir() / (tag + ".sig.bin");
    writeFile(in, data);
    EXPECT_EQ(0, sh("openssl pkeyutl -sign -rawin -inkey " + priv.string()
                    + " -in " + in.string() + " -out " + out.string()));
    return readFile(out);
}

std::string pemOf(const fs::path& pub) {
    std::string pem = readFile(pub);
    // JSON-escape the newlines so the PEM can sit in a string field.
    std::string out;
    for (char c : pem) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') continue;
        else out += c;
    }
    return out;
}

struct DocOptions {
    std::string organization = "dev.cajeta";
    std::string namespaces = R"(["dev.cajeta"])";
    std::string docNotAfter = "2030-01-01T00:00:00Z";
    // Each entry: {id, pubPath, notBefore, notAfter}
    std::vector<std::tuple<std::string, fs::path, std::string, std::string>> keys;
};

std::string payloadJson(const DocOptions& o) {
    std::ostringstream j;
    j << "{\"organization\":\"" << o.organization << "\","
      << "\"namespaces\":" << o.namespaces << ","
      << "\"not-after\":\"" << o.docNotAfter << "\","
      << "\"keys\":[";
    for (size_t i = 0; i < o.keys.size(); ++i) {
        const auto& [id, pub, nb, na] = o.keys[i];
        if (i) j << ",";
        j << "{\"id\":\"" << id << "\",\"algorithm\":\"ed25519\","
          << "\"public-key\":\"" << pemOf(pub) << "\","
          << "\"not-before\":\"" << nb << "\",\"not-after\":\"" << na << "\"}";
    }
    j << "]}";
    return j.str();
}

// Wrap a payload in an envelope signed by `signer`.
std::string envelope(const std::string& payload, const fs::path& signerPriv,
                     const std::string& tag,
                     const std::string& rootKeyId = "olla-root-test",
                     int format = 1) {
    std::string sig = signBytes(payload, signerPriv, tag);
    std::ostringstream j;
    j << "{\"format\":" << format << ","
      << "\"root-key-id\":\"" << rootKeyId << "\","
      << "\"payload\":\"" << llvm::encodeBase64(payload) << "\","
      << "\"signature\":\"" << llvm::encodeBase64(sig) << "\"}";
    return j.str();
}

std::time_t at(const std::string& stamp) {
    auto t = parseUtcTimestamp(stamp);
    EXPECT_TRUE(!!t) << "fixture timestamp did not parse: " << stamp;
    return t ? *t : 0;
}

}  // namespace

// 1.1.1 / spec 2.2 — a well-formed document parses.
TEST(OrgKeyDocumentTests, wellFormedDocumentParses) {
    auto root = keyPair("root");
    auto org = keyPair("org");
    DocOptions o;
    o.keys = {{"k1", org.pub, "2026-01-01T00:00:00Z", "2027-01-01T00:00:00Z"}};

    auto doc = loadOrgKeyDocument(envelope(payloadJson(o), root.priv, "wf"),
                                  {root.pub.string()},
                                  at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!doc) << llvm::toString(doc.takeError());
    EXPECT_EQ("dev.cajeta", doc->organization);
    ASSERT_EQ(1u, doc->namespaces.size());
    EXPECT_EQ("dev.cajeta", doc->namespaces[0]);
    ASSERT_EQ(1u, doc->keys.size());
    EXPECT_EQ("k1", doc->keys[0].id);
    EXPECT_EQ("ed25519", doc->keys[0].algorithm);
    EXPECT_EQ("olla-root-test", doc->rootKeyId);   // spec 6.3
}

// 1.1.2 / spec 2.3, 2.4 — the root signature decides.
TEST(OrgKeyDocumentTests, signatureByAnUntrustedRootIsRejected) {
    auto root = keyPair("root");
    auto other = keyPair("otherroot");
    auto org = keyPair("org");
    DocOptions o;
    o.keys = {{"k1", org.pub, "2026-01-01T00:00:00Z", "2027-01-01T00:00:00Z"}};
    auto now = at("2026-06-01T00:00:00Z");

    auto good = loadOrgKeyDocument(envelope(payloadJson(o), root.priv, "sg"),
                                   {root.pub.string()}, now);
    EXPECT_TRUE(!!good) << "a document signed by a trusted root must verify";

    // Signed by a real key that is simply not a trusted root. This is the
    // arm that matters: the bytes carry a VALID signature, just not one we
    // accept.
    auto bad = loadOrgKeyDocument(envelope(payloadJson(o), other.priv, "sb"),
                                  {root.pub.string()}, now);
    EXPECT_FALSE(!!bad) << "an untrusted signer must be refused";
    if (!bad) llvm::consumeError(bad.takeError());
}

// 1.1.2 (tamper) — the signature covers the payload, so altering it breaks.
TEST(OrgKeyDocumentTests, alteringThePayloadBreaksTheSignature) {
    auto root = keyPair("root");
    auto org = keyPair("org");
    DocOptions o;
    o.keys = {{"k1", org.pub, "2026-01-01T00:00:00Z", "2027-01-01T00:00:00Z"}};

    // Sign one payload, ship another — a mirror widening the namespaces it
    // serves, which is precisely what signing the document prevents.
    std::string signedPayload = payloadJson(o);
    DocOptions widened = o;
    widened.namespaces = R"(["dev.cajeta","com.victim"])";

    std::string sig = signBytes(signedPayload, root.priv, "tamper");
    std::ostringstream j;
    j << "{\"format\":1,\"root-key-id\":\"olla-root-test\","
      << "\"payload\":\"" << llvm::encodeBase64(payloadJson(widened)) << "\","
      << "\"signature\":\"" << llvm::encodeBase64(sig) << "\"}";

    auto doc = loadOrgKeyDocument(j.str(), {root.pub.string()},
                                  at("2026-06-01T00:00:00Z"));
    EXPECT_FALSE(!!doc) << "a payload that is not the signed one must fail";
    if (!doc) llvm::consumeError(doc.takeError());
}

// 1.1.3 / spec 2.5 — expiry is not advisory.
TEST(OrgKeyDocumentTests, anExpiredDocumentIsRejectedDespiteAValidSignature) {
    auto root = keyPair("root");
    auto org = keyPair("org");
    DocOptions o;
    o.docNotAfter = "2026-02-01T00:00:00Z";
    o.keys = {{"k1", org.pub, "2026-01-01T00:00:00Z", "2027-01-01T00:00:00Z"}};
    auto env = envelope(payloadJson(o), root.priv, "exp");

    auto live = loadOrgKeyDocument(env, {root.pub.string()},
                                   at("2026-01-15T00:00:00Z"));
    EXPECT_TRUE(!!live) << "inside its window the document is usable";

    auto dead = loadOrgKeyDocument(env, {root.pub.string()},
                                   at("2026-03-01T00:00:00Z"));
    EXPECT_FALSE(!!dead)
        << "an expired document must be refused even though it is validly "
           "signed — otherwise revocation-by-expiry means nothing";
    if (!dead) llvm::consumeError(dead.takeError());
}

// 1.1.4 / spec 2.2 — a key outside its own window is not usable, while a
// sibling inside its window is.
TEST(OrgKeyDocumentTests, keyValidityWindowsAreEnforcedPerKey) {
    auto root = keyPair("root");
    auto oldK = keyPair("oldkey");
    auto newK = keyPair("newkey");
    DocOptions o;
    o.keys = {{"old", oldK.pub, "2025-01-01T00:00:00Z", "2026-01-01T00:00:00Z"},
              {"new", newK.pub, "2026-01-01T00:00:00Z", "2027-01-01T00:00:00Z"}};

    auto doc = loadOrgKeyDocument(envelope(payloadJson(o), root.priv, "win"),
                                  {root.pub.string()},
                                  at("2026-06-01T00:00:00Z"));
    ASSERT_TRUE(!!doc) << llvm::toString(doc.takeError());

    auto usable = doc->usableKeys(at("2026-06-01T00:00:00Z"));
    ASSERT_EQ(1u, usable.size()) << "only the current key is usable";
    EXPECT_EQ("new", usable[0]->id);

    // Before either: none. A document can parse and still authorise nothing,
    // and the caller must read that as "cannot verify".
    EXPECT_TRUE(doc->usableKeys(at("2024-01-01T00:00:00Z")).empty());
}

// 1.1.5 / spec 2.6 — overlapping windows are what make rotation possible.
TEST(OrgKeyDocumentTests, overlappingWindowsAcceptEitherKey) {
    auto root = keyPair("root");
    auto a = keyPair("rota");
    auto b = keyPair("rotb");
    DocOptions o;
    o.keys = {{"outgoing", a.pub, "2025-01-01T00:00:00Z", "2026-07-01T00:00:00Z"},
              {"incoming", b.pub, "2026-06-01T00:00:00Z", "2027-01-01T00:00:00Z"}};

    auto doc = loadOrgKeyDocument(envelope(payloadJson(o), root.priv, "rot"),
                                  {root.pub.string()},
                                  at("2026-06-15T00:00:00Z"));
    ASSERT_TRUE(!!doc) << llvm::toString(doc.takeError());

    auto usable = doc->usableKeys(at("2026-06-15T00:00:00Z"));
    EXPECT_EQ(2u, usable.size())
        << "inside the overlap BOTH keys verify — without this, rotation "
           "needs a flag day where every publisher and consumer switches at "
           "one instant";
}

// 1.1.6 — malformed input is refused, never partially applied.
TEST(OrgKeyDocumentTests, malformedInputIsRefused) {
    auto root = keyPair("root");
    auto org = keyPair("org");
    auto now = at("2026-06-01T00:00:00Z");
    std::vector<std::string> roots{root.pub.string()};

    struct Case { const char* why; std::string envelope; };
    DocOptions ok;
    ok.keys = {{"k1", org.pub, "2026-01-01T00:00:00Z", "2027-01-01T00:00:00Z"}};

    DocOptions noNamespaces = ok;
    noNamespaces.namespaces = "[]";

    DocOptions badStamp = ok;
    badStamp.docNotAfter = "2026-01-01T00:00:00+01:00";   // offset, not Z

    std::vector<Case> cases = {
        {"not JSON", "{ this is not json"},
        {"unknown format version",
         envelope(payloadJson(ok), root.priv, "fmt", "olla-root-test", 99)},
        {"payload is not base64",
         R"({"format":1,"root-key-id":"r","payload":"!!!!","signature":"AA=="})"},
        {"no namespaces — a document owning nothing authorises nothing",
         envelope(payloadJson(noNamespaces), root.priv, "nons")},
        {"a timestamp with an offset rather than Z",
         envelope(payloadJson(badStamp), root.priv, "offs")},
    };

    for (auto& c : cases) {
        auto doc = loadOrgKeyDocument(c.envelope, roots, now);
        EXPECT_FALSE(!!doc) << "should have been refused: " << c.why;
        if (!doc) llvm::consumeError(doc.takeError());
    }
}

// The timestamp contract the schema states, tested directly: UTC only.
TEST(OrgKeyDocumentTests, timestampsAreUtcSecondsOrNothing) {
    EXPECT_TRUE(!!parseUtcTimestamp("2026-01-01T00:00:00Z"));
    EXPECT_LT(*parseUtcTimestamp("2026-01-01T00:00:00Z"),
              *parseUtcTimestamp("2026-01-01T00:00:01Z"));

    for (const char* bad : {"2026-01-01T00:00:00+01:00", "2026-01-01 00:00:00Z",
                            "2026-01-01T00:00:00.500Z", "2026-01-01", ""}) {
        auto t = parseUtcTimestamp(bad);
        EXPECT_FALSE(!!t) << "accepted a timestamp it should not: " << bad;
        if (!t) llvm::consumeError(t.takeError());
    }
}
