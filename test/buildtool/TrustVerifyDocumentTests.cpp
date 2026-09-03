// `cajeta trust verify-document` — publisher-trust §2, §2.7, §2.8.
//
// An operator signs a document offline and has to know a client will accept
// it BEFORE serving it. A signature check does not answer that: these
// documents also carry a type discriminator, an origin they must match,
// expiry, and per-key validity windows, and any of those can reject a
// perfectly signed file.
//
// The test that matters most is `aRevocationIsNotMisreportedAsAnOrgDocument`.
// Kind detection has to read the payload WITHOUT verifying first, because a
// revocation is signed by a delegated key and never opens against a root —
// verify-then-route misidentified every revocation, and misidentified every
// failing document at exactly the moment the operator needs the kind.

#include "gtest/gtest.h"

#include "OrgKeyFixture.h"
#include "TempTeardown.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>

using namespace cajeta::buildtool::testing;
namespace fs = std::filesystem;

namespace {

std::string compilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r;
    if (envRoot && *envRoot) r = envRoot;
    else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
    return r + "/build/src/cajeta";
}

struct Run { int code; std::string out; };

Run runCli(const fs::path& trustDir, const std::string& args) {
    std::string cmd = "CAJETA_TRUST_KEYS_DIR=" + trustDir.string() + " " +
                      compilerBinary() + " trust " + args + " 2>&1";
    Run r{-1, {}};
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return r;
    std::array<char, 512> buf{};
    while (fgets(buf.data(), buf.size(), p)) r.out += buf.data();
    r.code = pclose(p) / 256;
    return r;
}

// A scratch world: its own root, an org key, a release key, and a trust dir
// holding only that root — so nothing here depends on the machine's store.
struct World {
    fs::path dir;
    TestKeyPair root, org, release;

    explicit World(const std::string& tag)
        : dir(fs::temp_directory_path() / ("cajeta-vdoc-" + tag)) {
        rmTree(dir);
        fs::create_directories(dir / "trust" / "roots");
        root = makeKeyPair(dir, "root");
        org = makeKeyPair(dir, "org");
        release = makeKeyPair(dir, "release");
        writeWholeFile(dir / "trust" / "roots" / "t-root.pem",
                       readWholeFile(root.pub));
    }
    ~World() { rmTree(dir); }

    fs::path trust() const { return dir / "trust"; }

    fs::path write(const std::string& name, const std::string& body) {
        auto p = dir / name;
        writeWholeFile(p, body);
        return p;
    }

    std::string delegation(const std::string& origin) {
        std::ostringstream p;
        p << "{\"type\":\"repository-delegation\","
          << "\"issued-at\":\"2026-01-01T00:00:00Z\","
          << "\"repository\":\"" << origin
          << "\",\"not-after\":\"2030-01-01T00:00:00Z\",\"keys\":[{"
          << "\"id\":\"release-1\",\"algorithm\":\"ed25519\",\"public-key\":\""
          << jsonEscapePem(readWholeFile(release.pub))
          << "\",\"not-before\":\"2020-01-01T00:00:00Z\","
          << "\"not-after\":\"2030-01-01T00:00:00Z\"}]}";
        return envelopeAround(dir, p.str(), root, "t-root", "d");
    }

    std::string revocation(const std::string& origin) {
        std::ostringstream p;
        p << "{\"type\":\"key-revocation\",\"repository\":\"" << origin
          << "\",\"issued-at\":\"2026-01-01T00:00:00Z\","
          << "\"not-after\":\"2099-01-01T00:00:00Z\",\"revoked\":[]}";
        // Signed by the RELEASE key, which is the whole point.
        return envelopeAround(dir, p.str(), release, "release-1", "r");
    }
};

constexpr const char* kOrigin = "https://olla.test";

}  // namespace

TEST(TrustVerifyDocumentTests, aValidOrgDocumentIsAccepted) {
    World w("org");
    OrgDocumentSpec spec;
    spec.securityContactUri = "mailto:sec@dev.cajeta";
    auto f = w.write("org.json",
                     orgKeyDocument(w.dir, spec, w.org, w.root, "t-root"));

    auto r = runCli(w.trust(), "verify-document " + f.string());
    EXPECT_EQ(0, r.code) << r.out;
    EXPECT_NE(std::string::npos, r.out.find("OK  organization key document"))
        << r.out;
    EXPECT_NE(std::string::npos, r.out.find("mailto:sec@dev.cajeta")) << r.out;
}

TEST(TrustVerifyDocumentTests, aDelegationNeedsItsOriginAndIsCheckedAgainstIt) {
    World w("deleg");
    auto f = w.write("deleg.json", w.delegation(kOrigin));

    auto missing = runCli(w.trust(), "verify-document " + f.string());
    EXPECT_EQ(2, missing.code) << missing.out;
    EXPECT_NE(std::string::npos, missing.out.find("--origin is required"))
        << missing.out;

    auto ok = runCli(w.trust(),
                     "verify-document " + f.string() + " --origin " + kOrigin);
    EXPECT_EQ(0, ok.code) << ok.out;
    EXPECT_NE(std::string::npos, ok.out.find("OK  repository delegation"))
        << ok.out;

    // The same file, a different origin. This is the replay the binding
    // exists to stop, and the command has to surface it.
    auto wrong = runCli(w.trust(), "verify-document " + f.string() +
                                       " --origin https://elsewhere.test");
    EXPECT_EQ(1, wrong.code) << wrong.out;
    EXPECT_NE(std::string::npos, wrong.out.find("REFUSED")) << wrong.out;
}

// THE test. A revocation is signed by a delegated key, so opening it against
// the roots always fails. Detecting the kind by verifying first therefore
// reported every revocation as an organization key document.
TEST(TrustVerifyDocumentTests, aRevocationIsNotMisreportedAsAnOrgDocument) {
    World w("revoc");
    auto rev = w.write("revoc.json", w.revocation(kOrigin));
    auto del = w.write("deleg.json", w.delegation(kOrigin));

    auto noDel = runCli(w.trust(),
                        "verify-document " + rev.string() + " --origin " + kOrigin);
    EXPECT_EQ(2, noDel.code) << noDel.out;
    EXPECT_NE(std::string::npos, noDel.out.find("revocation statement"))
        << "the kind must be named even when the document cannot be checked: "
        << noDel.out;
    EXPECT_EQ(std::string::npos, noDel.out.find("organization key document"))
        << "misreported as an org document — kind detection verified first: "
        << noDel.out;

    auto ok = runCli(w.trust(), "verify-document " + rev.string() +
                                    " --origin " + kOrigin +
                                    " --delegation " + del.string());
    EXPECT_EQ(0, ok.code) << ok.out;
    EXPECT_NE(std::string::npos, ok.out.find("OK  revocation statement"))
        << ok.out;
    EXPECT_NE(std::string::npos, ok.out.find("asserts nothing is revoked"))
        << "an empty list is a statement, not an absence: " << ok.out;
}

TEST(TrustVerifyDocumentTests, aDocumentSignedByAnUntrustedRootIsRefused) {
    World w("untrusted");
    auto impostor = makeKeyPair(w.dir, "impostor");
    OrgDocumentSpec spec;
    auto f = w.write("bad.json",
                     orgKeyDocument(w.dir, spec, w.org, impostor, "t-root"));

    auto r = runCli(w.trust(), "verify-document " + f.string());
    EXPECT_EQ(1, r.code) << r.out;
    EXPECT_NE(std::string::npos, r.out.find("REFUSED")) << r.out;
}
