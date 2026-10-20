// publisher-trust Unit 6 — verification is the default at fetch (spec 5.3-5.5,
// 9.1-9.3; plan 6.1.x).
//
// The resolver used to trust on first use: it fetched, hashed whatever arrived,
// and mirrored it into ~/.olla. Only the notebook kernel consulted the signed
// release metadata. These drive `resolveProjectDependencies` against a
// filesystem "remote" carrying real ed25519 envelopes, with a fixture root
// planted in the test home's trust store.
//
//   - a signed release whose bytes match resolves, and ~/.olla keeps its
//     envelope beside it                                         [6.1.1]
//   - signed metadata naming other bytes is refused, nothing written [6.1.1]
//   - an unsigned release is refused by default, naming the fix     [6.1.1]
//   - `"verification": "checksum"` on that repository admits it     [6.1.2]
//   - the relaxation covers only the repository that declares it   [6.1.3]
//   - an envelope that does not verify is refused even when relaxed
//   - an existing ~/.olla entry keeps resolving under the default   [6.1.4]
//   - `require-signatures` is retired with a warning               [6.2.2]

#include "gtest/gtest.h"

#include "../PortableEnv.h"
#include "OrgKeyFixture.h"
#include "TempTeardown.h"

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/FetchVerification.h"
#include "cajeta/buildtool/OrgKeyCache.h"
#include "cajeta/buildtool/repo/FilesystemRepository.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/OllaStore.h"
#include "cajeta/buildtool/Resolver.h"
#include "cajeta/buildtool/RootTrust.h"
#include "cajeta/cli/TrustStore.h"

#include <llvm/Support/Error.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

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

// OLLA_HOME and CAJETA_TRUST_KEYS_DIR would redirect the store and the trust
// anchors away from the test home, so both are cleared for the test's life.
struct EnvCleared {
    std::string name;
    bool had = false;
    std::string old;
    explicit EnvCleared(const char* n) : name(n) {
        if (const char* v = ::getenv(n)) { had = true; old = v; }
        ::unsetenv(n);
    }
    ~EnvCleared() { if (had) ::setenv(name.c_str(), old.c_str(), 1); }
};

// What a signed fixture release serves besides its envelope (spec 4.1): the
// organization's key document and an archive signature, by default made by the
// organization's key.
struct Publisher {
    bool keyDocument = true;
    bool archiveSignature = true;
    const TestKeyPair* signWith = nullptr;
};

struct Fixture {
    EnvCleared olla{"OLLA_HOME"};
    EnvCleared trust{"CAJETA_TRUST_KEYS_DIR"};
    fs::path dir;
    fs::path home;
    fs::path proj;
    TestKeyPair root;
    TestKeyPair orgKey;

    explicit Fixture(const std::string& tag) {
        dir = fs::temp_directory_path()
            / ("cajeta-fetchverify-" + tag + "-" + std::to_string(::getpid()));
        rmTree(dir);
        home = dir / "home";
        proj = dir / "proj";
        fs::create_directories(home);
        fs::create_directories(proj);
        root = makeKeyPair(dir / "keys", "root");
        orgKey = makeKeyPair(dir / "keys", "org");
        auto layout = cajeta::cli::rootTrustLayoutOf(
            cajeta::cli::resolveTrustStoreLayout(
                home.string(), (dir / "no-system").string(), std::string()));
        if (auto e = addRootKey(layout, "fixture-root", root.pub.string())) {
            ADD_FAILURE() << errText(std::move(e));
        }
    }
    ~Fixture() { rmTree(dir); }

    // A repository directory holding `name`@`version` with `bytes`, and,
    // when `signedHash` is set, an envelope naming it signed by `signer`.
    fs::path repo(const std::string& tag, const std::string& name,
                  const std::string& version, const std::string& bytes,
                  const std::string& signedHash,
                  const TestKeyPair* signer = nullptr,
                  Publisher pub = Publisher{}) {
        auto root_ = dir / tag;
        auto rel = root_ / name / version;
        fs::create_directories(rel);
        writeWholeFile(rel / (name + "-" + version + ".cja"), bytes);
        if (!signedHash.empty()) {
            const TestKeyPair& k = signer ? *signer : root;
            writeWholeFile(rel / (name + "-" + version + ".release.json"),
                           envelopeAround(dir / "keys",
                                          releasePayload(name, version,
                                                         signedHash, "dev.cajeta"),
                                          k, "fixture-root", tag + "-" + name));
            if (pub.keyDocument) {
                OrgDocumentSpec spec;
                writeWholeFile(root_ / ".well-known" / "org-keys" / "dev.cajeta.json",
                               orgKeyDocument(dir / "keys", spec, orgKey, root,
                                              "fixture-root", tag + "-doc"));
            }
            if (pub.archiveSignature) {
                const TestKeyPair& sk = pub.signWith ? *pub.signWith : orgKey;
                writeWholeFile(rel / (name + "-" + version + ".cja.sig"),
                               signWithKey(dir / "keys", bytes, sk.priv,
                                           tag + "-" + name + "-archive"));
            }
        }
        return root_;
    }

    std::string sha(const std::string& bytes) {
        auto f = dir / "sha-input";
        writeWholeFile(f, bytes);
        return ArtifactCache::sha256OfFile(f.string());
    }

    llvm::Expected<std::vector<ResolvedDependency>> resolve(const std::string& settings) {
        auto m = loadManifestString(
            "{\"details\":{\"name\":\"c\",\"version\":\"0.1.0\"},"
            "\"settings\":{" + settings + "}}");
        if (!m) return m.takeError();
        return resolveProjectDependencies(*m, proj.string(), home.string());
    }

    fs::path ollaEntry(const std::string& name, const std::string& version) {
        return home / ".olla" / name / version;
    }
};

std::string repoJson(const std::string& name, const fs::path& path,
                     const std::string& extra = "") {
    return "{\"name\":\"" + name + "\",\"type\":\"filesystem\",\"path\":\""
         + path.generic_string() + "\"" + extra + "}";
}

}  // namespace

// 6.1.1 — the conformant case: signed metadata naming exactly these bytes. The
// mirror into ~/.olla keeps the envelope, so the store holds what was checked.
TEST(FetchVerificationTests, aSignedReleaseWhoseBytesMatchResolvesWithItsEnvelope) {
    Fixture fx("match");
    std::string bytes = "SIGNED-BYTES";
    auto remote = fx.repo("remote", "dev.cajeta.a", "1.0.0", bytes, fx.sha(bytes));
    auto r = fx.resolve(
        "\"dependencies\":{\"dev.cajeta.a\":\"1.0.0\"},"
        "\"repositories\":[" + repoJson("remote", remote) + "]");
    ASSERT_TRUE(!!r) << errText(r.takeError());
    auto entry = fx.ollaEntry("dev.cajeta.a", "1.0.0");
    EXPECT_TRUE(fs::exists(entry / "dev.cajeta.a-1.0.0.cja"));
    EXPECT_TRUE(fs::exists(entry / "dev.cajeta.a-1.0.0.release.json"))
        << "~/.olla must keep the envelope the fetch was checked against";
}

// 6.1.1 / spec 5.2 — bytes that do not hash to the signed value are discarded
// and nothing reaches ~/.olla.
TEST(FetchVerificationTests, signedMetadataNamingOtherBytesIsRefusedAndNothingIsWritten) {
    Fixture fx("mismatch");
    auto remote = fx.repo("remote", "dev.cajeta.a", "1.0.0", "TAMPERED-BYTES",
                          fx.sha("THE-RELEASED-BYTES"));
    auto r = fx.resolve(
        "\"dependencies\":{\"dev.cajeta.a\":\"1.0.0\"},"
        "\"repositories\":[" + repoJson("remote", remote) + "]");
    ASSERT_FALSE(!!r) << "a hash mismatch against signed metadata must refuse";
    std::string msg = errText(r.takeError());
    EXPECT_NE(std::string::npos, msg.find("dev.cajeta.a")) << msg;
    EXPECT_FALSE(fs::exists(fx.ollaEntry("dev.cajeta.a", "1.0.0")))
        << "nothing is written on a refusal (spec 4.2)";
}

// 6.1.1 / spec 5.3, 5.4 — no signed metadata, no configuration: refused, and
// the message says which repository and how to relax it.
TEST(FetchVerificationTests, anUnsignedReleaseIsRefusedByDefaultAndTheErrorNamesTheFix) {
    Fixture fx("unsigned");
    auto remote = fx.repo("remote", "dev.cajeta.a", "1.0.0", "BYTES", "");
    auto r = fx.resolve(
        "\"dependencies\":{\"dev.cajeta.a\":\"1.0.0\"},"
        "\"repositories\":[" + repoJson("vendor", remote) + "]");
    ASSERT_FALSE(!!r) << "an unverifiable release must be refused by default";
    std::string msg = errText(r.takeError());
    EXPECT_NE(std::string::npos, msg.find("vendor")) << msg;
    EXPECT_NE(std::string::npos, msg.find("\"verification\": \"checksum\"")) << msg;
    EXPECT_FALSE(fs::exists(fx.ollaEntry("dev.cajeta.a", "1.0.0")));
}

// 6.1.2 / spec 5.4 — the explicit relaxation on that repository admits it.
TEST(FetchVerificationTests, checksumVerificationOnTheRepositoryAdmitsAnUnsignedRelease) {
    Fixture fx("relaxed");
    auto remote = fx.repo("remote", "dev.cajeta.a", "1.0.0", "BYTES", "");
    auto r = fx.resolve(
        "\"dependencies\":{\"dev.cajeta.a\":\"1.0.0\"},"
        "\"repositories\":[" + repoJson("vendor", remote,
                                        ",\"verification\":\"checksum\"") + "]");
    ASSERT_TRUE(!!r) << errText(r.takeError());
    EXPECT_TRUE(fs::exists(fx.ollaEntry("dev.cajeta.a", "1.0.0")));
}

// 6.1.3 / spec 5.5 — relaxing one repository does not weaken another.
TEST(FetchVerificationTests, theRelaxationCoversOnlyTheRepositoryThatDeclaresIt) {
    Fixture fx("perrepo");
    auto local = fx.repo("local", "dev.cajeta.a", "1.0.0", "A-BYTES", "");
    auto central = fx.repo("central", "dev.cajeta.b", "1.0.0", "B-BYTES", "");
    auto r = fx.resolve(
        "\"dependencies\":{\"dev.cajeta.a\":\"1.0.0\",\"dev.cajeta.b\":\"1.0.0\"},"
        "\"repositories\":[" + repoJson("local", local,
                                        ",\"verification\":\"checksum\"")
        + "," + repoJson("central", central) + "]");
    ASSERT_FALSE(!!r) << "central's unsigned release must still be refused";
    std::string msg = errText(r.takeError());
    EXPECT_NE(std::string::npos, msg.find("dev.cajeta.b")) << msg;
    EXPECT_NE(std::string::npos, msg.find("central")) << msg;
}

// Metadata that is present and does not verify is tampering, not absence: a
// relaxation for missing metadata must not admit a forged envelope.
TEST(FetchVerificationTests, anEnvelopeThatDoesNotVerifyIsRefusedEvenWhenRelaxed) {
    Fixture fx("forged");
    auto stranger = makeKeyPair(fx.dir / "keys", "stranger");
    std::string bytes = "BYTES";
    auto remote = fx.repo("remote", "dev.cajeta.a", "1.0.0", bytes,
                          fx.sha(bytes), &stranger);
    auto r = fx.resolve(
        "\"dependencies\":{\"dev.cajeta.a\":\"1.0.0\"},"
        "\"repositories\":[" + repoJson("vendor", remote,
                                        ",\"verification\":\"checksum\"") + "]");
    ASSERT_FALSE(!!r) << "a forged envelope must refuse under any policy";
    consumeError(r.takeError());
    EXPECT_FALSE(fs::exists(fx.ollaEntry("dev.cajeta.a", "1.0.0")));
}

// 6.1.4 / spec 9.1 — what is already in ~/.olla (here a local `cajeta install`
// build, which carries no envelope) keeps resolving with no remote at all.
TEST(FetchVerificationTests, anExistingOllaEntryKeepsResolvingUnderTheDefault) {
    Fixture fx("existing");
    auto src = fx.dir / "local-build.cja";
    writeWholeFile(src, "LOCAL-BUILD");
    OllaStore store((fx.home / ".olla").string());
    auto w = store.write("dev.cajeta.a", "1.0.0", src.string(), std::nullopt);
    ASSERT_TRUE(!!w) << errText(w.takeError());
    auto r = fx.resolve("\"dependencies\":{\"dev.cajeta.a\":\"1.0.0\"},"
                        "\"repositories\":[]");
    ASSERT_TRUE(!!r) << errText(r.takeError());
}

// The setting is parsed strictly: a typo must not silently mean the default.
TEST(FetchVerificationTests, anUnknownVerificationValueIsAnError) {
    Fixture fx("typo");
    auto remote = fx.repo("remote", "dev.cajeta.a", "1.0.0", "BYTES", "");
    auto r = fx.resolve(
        "\"dependencies\":{\"dev.cajeta.a\":\"1.0.0\"},"
        "\"repositories\":[" + repoJson("vendor", remote,
                                        ",\"verification\":\"checksums\"") + "]");
    ASSERT_FALSE(!!r);
    std::string msg = errText(r.takeError());
    EXPECT_NE(std::string::npos, msg.find("verification")) << msg;
    EXPECT_NE(std::string::npos, msg.find("checksums")) << msg;
}

// 6.2.2 — `require-signatures` is retired: still parsed, no effect, and a
// warning that publisher verification is now the default.
TEST(FetchVerificationTests, requireSignaturesIsRetiredWithAWarning) {
    Fixture fx("retired");
    std::string bytes = "SIGNED-BYTES";
    auto remote = fx.repo("remote", "dev.cajeta.a", "1.0.0", bytes, fx.sha(bytes));
    ::testing::internal::CaptureStderr();
    auto r = fx.resolve(
        "\"require-signatures\":true,"
        "\"dependencies\":{\"dev.cajeta.a\":\"1.0.0\"},"
        "\"repositories\":[" + repoJson("remote", remote) + "]");
    std::string err = ::testing::internal::GetCapturedStderr();
    ASSERT_TRUE(!!r) << errText(r.takeError());
    EXPECT_NE(std::string::npos, err.find("require-signatures")) << err;
}

namespace {

// Runs the shared check directly on `name`@1.0.0 in `repoRoot` under the
// default policy, against `legacy`.
llvm::Expected<VerifiedRelease> verifyAgainst(Fixture& fx, const fs::path& repoRoot,
                                              const std::string& name,
                                              const std::vector<LegacyRelease>& legacy) {
    FilesystemRepository repo("central", repoRoot.string());
    auto trust = cajeta::cli::resolveTrustStoreLayout(
        fx.home.string(), (fx.dir / "no-system").string(), std::string());
    OrgKeyCache orgKeys(cajeta::cli::rootTrustLayoutOf(trust));
    auto path = repoRoot / name / "1.0.0" / (name + "-1.0.0.cja");
    return verifyFetchedRelease(repo, name, "1.0.0", path.string(),
                                Verification::Publisher, trust, orgKeys,
                                std::time(nullptr), &legacy);
}

}  // namespace

// A release olla published before it signed metadata is admitted under the
// default when the toolchain's list names it with these exact bytes.
TEST(FetchVerificationTests, aListedLegacyReleaseIsAdmittedUnderTheDefault) {
    Fixture fx("legacy-ok");
    std::string bytes = "LEGACY-BYTES";
    auto remote = fx.repo("remote", "dev.cajeta.old", "1.0.0", bytes, "");
    auto v = verifyAgainst(fx, remote, "dev.cajeta.old",
                           {{"dev.cajeta.old", "1.0.0", fx.sha(bytes)}});
    ASSERT_TRUE(!!v) << errText(v.takeError());
    EXPECT_FALSE(v->fromSignedMetadata) << "a listed release is not signed";
}

// The list pins bytes, not names: other bytes under a listed name are refused.
TEST(FetchVerificationTests, aLegacyEntryWithOtherBytesIsRefused) {
    Fixture fx("legacy-bytes");
    auto remote = fx.repo("remote", "dev.cajeta.old", "1.0.0", "SWAPPED", "");
    auto v = verifyAgainst(fx, remote, "dev.cajeta.old",
                           {{"dev.cajeta.old", "1.0.0", fx.sha("LEGACY-BYTES")}});
    ASSERT_FALSE(!!v);
    consumeError(v.takeError());
}

// An entry covers its own version only.
TEST(FetchVerificationTests, aLegacyEntryCoversOnlyItsVersion) {
    Fixture fx("legacy-version");
    std::string bytes = "LEGACY-BYTES";
    auto remote = fx.repo("remote", "dev.cajeta.old", "1.0.0", bytes, "");
    auto v = verifyAgainst(fx, remote, "dev.cajeta.old",
                           {{"dev.cajeta.old", "0.9.0", fx.sha(bytes)}});
    ASSERT_FALSE(!!v);
    consumeError(v.takeError());
}

// The embedded list is the 49 releases measured against live olla 2026-10-20.
TEST(FetchVerificationTests, theEmbeddedListPinsTheMeasuredLegacyReleases) {
    const auto& list = legacyReleases();
    EXPECT_EQ(49u, list.size());
    bool graph = false;
    for (const auto& r : list) {
        if (r.name == "dev.cajeta.graph" && r.version == "0.1.0") graph = true;
        EXPECT_EQ(0u, r.sha256.rfind("sha256:", 0)) << r.name << " " << r.version;
        EXPECT_EQ(71u, r.sha256.size()) << r.name << " " << r.version;
    }
    EXPECT_TRUE(graph) << "dev.cajeta.graph 0.1.0 is a measured legacy release";
}

// spec 4.1 — signed metadata alone binds the repository's word, not the
// publisher's: the archive must carry the organization's own signature.
TEST(FetchVerificationTests, aSignedReleaseWithNoArchiveSignatureIsRefused) {
    Fixture fx("nosig");
    std::string bytes = "SIGNED-BYTES";
    Publisher pub;
    pub.archiveSignature = false;
    auto remote = fx.repo("remote", "dev.cajeta.a", "1.0.0", bytes, fx.sha(bytes),
                          nullptr, pub);
    auto r = fx.resolve(
        "\"dependencies\":{\"dev.cajeta.a\":\"1.0.0\"},"
        "\"repositories\":[" + repoJson("remote", remote) + "]");
    ASSERT_FALSE(!!r) << "no publisher signature must refuse under the default";
    std::string msg = errText(r.takeError());
    EXPECT_NE(std::string::npos, msg.find("signature")) << msg;
    EXPECT_FALSE(fs::exists(fx.ollaEntry("dev.cajeta.a", "1.0.0")));
}

// spec 4.6 — an organization that serves no key document cannot be verified.
TEST(FetchVerificationTests, aSignedReleaseWhoseOrganizationHasNoKeyDocumentIsRefused) {
    Fixture fx("nodoc");
    std::string bytes = "SIGNED-BYTES";
    Publisher pub;
    pub.keyDocument = false;
    auto remote = fx.repo("remote", "dev.cajeta.a", "1.0.0", bytes, fx.sha(bytes),
                          nullptr, pub);
    auto r = fx.resolve(
        "\"dependencies\":{\"dev.cajeta.a\":\"1.0.0\"},"
        "\"repositories\":[" + repoJson("remote", remote) + "]");
    ASSERT_FALSE(!!r) << "no key document must refuse under the default";
    std::string msg = errText(r.takeError());
    EXPECT_NE(std::string::npos, msg.find("key document")) << msg;
}

// A signature by a key outside the organization's document is refused.
TEST(FetchVerificationTests, anArchiveSignatureByAnotherKeyIsRefused) {
    Fixture fx("otherkey");
    auto stranger = makeKeyPair(fx.dir / "keys", "stranger");
    std::string bytes = "SIGNED-BYTES";
    Publisher pub;
    pub.signWith = &stranger;
    auto remote = fx.repo("remote", "dev.cajeta.a", "1.0.0", bytes, fx.sha(bytes),
                          nullptr, pub);
    auto r = fx.resolve(
        "\"dependencies\":{\"dev.cajeta.a\":\"1.0.0\"},"
        "\"repositories\":[" + repoJson("remote", remote) + "]");
    ASSERT_FALSE(!!r);
    consumeError(r.takeError());
}
