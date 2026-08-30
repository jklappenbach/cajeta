// publisher-trust Unit 4 — the binding, end to end through a session
// install (spec §4.1–4.3, 9.2).
//
// PublisherVerificationTests covers the decision in isolation. These cover
// the wiring around it, which is where the notebook work kept finding
// defects: whether the org actually reaches the verifier, whether a cache
// hit takes the same path as a fetch, and whether the document really wins
// over the local trust store or merely runs beside it.
//
// The fixture is real throughout: a compiled `.cja`, an ed25519 signature
// from `cajeta archive sign`, a root-signed key document, and root-signed
// release metadata served from a filesystem repository.

#include "gtest/gtest.h"
#include "../PortableEnv.h"

#include "buildtool/OrgKeyFixture.h"

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/kernel/KernelSession.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using cajeta::kernel::CellResult;
using cajeta::kernel::KernelSession;
using cajeta::kernel::SessionOptions;
using namespace cajeta::buildtool::testing;

namespace fs = std::filesystem;

namespace {

std::string compilerBinary() {
    if (const char* env = std::getenv("CAJETA_BINARY")) return env;
    auto direct = fs::current_path() / "src" / "cajeta";
    if (fs::is_regular_file(direct)) return direct.string();
    auto fromRoot = fs::current_path() / "build" / "src" / "cajeta";
    if (fs::is_regular_file(fromRoot)) return fromRoot.string();
    return direct.string();
}

int runQuiet(const std::string& cmd, const fs::path& log) {
    int rc = std::system((cmd + " > " + log.string() + " 2>&1").c_str());
#ifdef _WIN32
    return rc;
#else
    return WEXITSTATUS(rc);
#endif
}

const char* kAnswerSrc =
    "package depx;\n"
    "public class Answer {\n"
    "    public static int32 v() { return 42; }\n"
    "}\n";

fs::path buildDep(const fs::path& root) {
    writeWholeFile(root / "dep" / "src" / "depx" / "Answer.cajeta", kAnswerSrc);
    auto out = root / "dep-out";
    fs::create_directories(out);
    std::string cmd = compilerBinary() + " depx.Answer "
        + (root / "dep" / "src").string() + " " + out.string() + " --emit=cja";
    if (runQuiet(cmd, root / "dep.log") != 0) return {};
    return out / "depx.cja";
}

// Everything a conformant repository serves for one release: the archive,
// its checksum, its signature, root-signed release metadata naming the
// owning organization, and that organization's key document.
struct Fixture {
    fs::path repo;
    fs::path trust;         // CAJETA_TRUST_KEYS_DIR
    TestKeyPair rootKey;
    TestKeyPair orgKey;
};

struct StageOptions {
    std::string packageName = "dev.cajeta.depx";
    std::string organization = "dev.cajeta";
    std::vector<std::string> namespaces = {"dev.cajeta"};
    // Put the signing key in the LOCAL trust store too, so a test can show
    // which of the two authorities actually decided.
    bool trustSignerLocally = false;
    bool serveKeyDocument = true;
};

Fixture stage(const fs::path& root, const fs::path& cja,
              const StageOptions& opt) {
    Fixture f;
    f.repo = root / "repo";
    f.trust = root / "trust";
    fs::create_directories(f.trust);
    f.rootKey = makeKeyPair(root / "keys", "root");
    f.orgKey = makeKeyPair(root / "keys", "org");

    auto dir = f.repo / opt.packageName / "1.0.0";
    fs::create_directories(dir);
    auto archive = dir / (opt.packageName + "-1.0.0.cja");
    fs::copy_file(cja, archive, fs::copy_options::overwrite_existing);

    std::string digest =
        cajeta::buildtool::ArtifactCache::sha256OfFile(archive.string());
    writeWholeFile(fs::path(archive.string() + ".sha256"), digest);

    // The signature the repository publishes, made by the ORG key.
    EXPECT_EQ(0, runQuiet(compilerBinary() + " archive sign " + archive.string()
                          + " --key " + f.orgKey.priv.string()
                          + " --out " + archive.string() + ".sig",
                          root / "sign.log"));

    // Root-signed release metadata: the hash and the owning organization
    // inside one signature (spec 5.1, 6.2).
    writeWholeFile(dir / (opt.packageName + "-1.0.0.release.json"),
                   envelopeAround(root / "keys",
                                  releasePayload(opt.packageName, "1.0.0",
                                                 digest, opt.organization),
                                  f.rootKey, "fixture-root", "rel"));

    if (opt.serveKeyDocument) {
        OrgDocumentSpec spec;
        spec.organization = opt.organization;
        spec.namespaces = opt.namespaces;
        writeWholeFile(f.repo / ".well-known" / "org-keys"
                           / (opt.organization + ".json"),
                       orgKeyDocument(root / "keys", spec, f.orgKey,
                                      f.rootKey, "fixture-root"));
    }

    // The root this client accepts, installed as an operator root.
    fs::create_directories(f.trust / "roots");
    fs::copy_file(f.rootKey.pub, f.trust / "roots" / "fixture-root.pem",
                  fs::copy_options::overwrite_existing);

    if (opt.trustSignerLocally) {
        fs::copy_file(f.orgKey.pub, f.trust / "org-signer.pem",
                      fs::copy_options::overwrite_existing);
    }

    setenv("CAJETA_TRUST_KEYS_DIR", f.trust.string().c_str(), 1);
    return f;
}

fs::path stageProject(const fs::path& root, const fs::path& repo) {
    auto project = root / "project";
    fs::create_directories(project);
    std::ostringstream json;
    json << "{\n"
         << "  \"details\": {\n"
         << "    \"name\": \"notebook\",\n"
         << "    \"version\": \"0.1.0\",\n"
         << "    \"cajeta-lang-version\": \"1.0\"\n"
         << "  },\n"
         << "  \"settings\": {\n"
         << "    \"repositories\": [\n"
         << "      { \"name\": \"fixture\", \"type\": \"filesystem\", "
         << "\"path\": \"" << repo.string() << "\", \"priority\": 0 }\n"
         << "    ]\n"
         << "  }\n"
         << "}\n";
    writeWholeFile(project / "cajeta.json", json.str());
    return project;
}

std::unique_ptr<KernelSession> sessionIn(const fs::path& project) {
    SessionOptions options;
    options.projectDir = project.string();
    std::string error;
    auto s = KernelSession::create(options, &error);
    EXPECT_NE(nullptr, s.get()) << "session create failed: " << error;
    return s;
}

fs::path freshRoot(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("cajeta-pubtrust-" + tag);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

std::string cell(const std::string& body) {
    return "import cajeta.session.Packages;\n" + body;
}

bool contains(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}

}  // namespace

// 4.1.1 + 4.1.3b / spec 4.1, 4.4, 6.2 — the ordinary path. The owning
// organization arrives in root-signed release metadata; nothing anywhere
// parses `dev.cajeta.depx` to guess where the org ends.
TEST(SessionPublisherTrustTests, anArtifactSignedByItsPublisherInstalls) {
    auto root = freshRoot("ok");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto f = stage(root, cja, StageOptions{});
    auto project = stageProject(root, f.repo);

    // The discriminator: the local trust store holds NO signer keys, so
    // the 9.1 fallback would refuse this outright. An install here can
    // only have come from the key document.
    for (const auto& e : fs::directory_iterator(f.trust)) {
        ASSERT_NE(".pem", e.path().extension())
            << "the trust store must be empty of signer keys for this test "
               "to prove anything: " << e.path();
    }

    auto s = sessionIn(project);
    ASSERT_NE(nullptr, s.get());

    CellResult r = s->execute(
        cell("Packages.install(\"dev.cajeta.depx\", \"1.*\");\n"));
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
    EXPECT_EQ("1.0.0", r.result);

    CellResult use = s->execute("import depx.Answer;\nAnswer.v();\n");
    ASSERT_TRUE(use.ok) << use.errorId << ": " << use.message;
    EXPECT_EQ("42", use.result);

    s->shutdown();
    fs::remove_all(root);
}

// 4.1.3 + 4.1.3a + 4.1.6 / spec 4.3, 4.3.1, 9.2 — three properties in one
// fixture, because they are the same moment.
//
// The artifact is named `dev.cajetaevil.depx` and the signed metadata says
// `dev.cajeta` owns it. The signature is genuine AND the signing key is in
// the machine's local trust store, so the pre-document path would have
// installed it. The key document refuses, which is what "the document
// decides" has to mean.
TEST(SessionPublisherTrustTests,
     theKeyDocumentDecidesEvenWhenTheTrustStoreWouldAccept) {
    auto root = freshRoot("evil");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";

    StageOptions opt;
    opt.packageName = "dev.cajetaevil.depx";   // NOT owned by dev.cajeta
    opt.organization = "dev.cajeta";
    opt.namespaces = {"dev.cajeta"};
    opt.trustSignerLocally = true;             // the trust store would say yes
    auto f = stage(root, cja, opt);
    auto project = stageProject(root, f.repo);

    auto s = sessionIn(project);
    ASSERT_NE(nullptr, s.get());

    CellResult r = s->execute(
        cell("Packages.install(\"dev.cajetaevil.depx\", \"1.*\");\n"));
    EXPECT_FALSE(r.ok)
        << "a name outside the organization's namespaces must not install, "
           "however good the signature";
    EXPECT_TRUE(contains(r.message, "outside the namespaces"))
        << "the message must name WHICH check failed; got: " << r.message;
    EXPECT_TRUE(contains(r.message, "dev.cajeta"))
        << "and name the organization; got: " << r.message;

    // Nothing spliced, session still serving.
    CellResult use = s->execute("import depx.Answer;\nAnswer.v();\n");
    EXPECT_FALSE(use.ok) << "a rejected install must splice nothing";
    CellResult alive = s->execute("1 + 1;\n");
    ASSERT_TRUE(alive.ok) << alive.errorId << ": " << alive.message;

    s->shutdown();
    fs::remove_all(root);
}

// 4.1.5 — a cache hit is a shortcut past the network, never past the
// checks. The first session installs and populates the artifact cache; the
// second finds the same bytes cached and must still refuse them once the
// repository's key document no longer authorises that name.
TEST(SessionPublisherTrustTests, aCachedArtifactIsHeldToTheSameVerification) {
    auto root = freshRoot("cached");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto f = stage(root, cja, StageOptions{});
    auto project = stageProject(root, f.repo);

    {
        auto s = sessionIn(project);
        ASSERT_NE(nullptr, s.get());
        CellResult r = s->execute(
            cell("Packages.install(\"dev.cajeta.depx\", \"1.*\");\n"));
        ASSERT_TRUE(r.ok) << "the first install should populate the cache: "
                          << r.errorId << ": " << r.message;
        s->shutdown();
    }

    // The organization narrows what it claims to own — a revocation in
    // everything but name. The bytes on disk have not changed, and the
    // cache still holds them.
    OrgDocumentSpec narrowed;
    narrowed.organization = "dev.cajeta";
    narrowed.namespaces = {"dev.cajeta.other"};
    writeWholeFile(f.repo / ".well-known" / "org-keys" / "dev.cajeta.json",
                   orgKeyDocument(root / "keys", narrowed, f.orgKey,
                                  f.rootKey, "fixture-root", "narrowed"));

    auto s = sessionIn(project);
    ASSERT_NE(nullptr, s.get());
    CellResult r = s->execute(
        cell("Packages.install(\"dev.cajeta.depx\", \"1.*\");\n"));
    EXPECT_FALSE(r.ok)
        << "a cached artifact must be verified again, not served on the "
           "strength of having been verified once";
    EXPECT_TRUE(contains(r.message, "outside the namespaces"))
        << "got: " << r.message;

    s->shutdown();
    fs::remove_all(root);
}

// Spec 5.4 / 9.1 — a repository that serves no key document still installs
// against the local trust store. This is the legacy path, and it is what
// keeps a machine configured before this landed working. Unit 6 is where
// the default flips; this test is the "before" it will have to change.
TEST(SessionPublisherTrustTests, aRepositoryWithNoDocumentUsesTheTrustStore) {
    auto root = freshRoot("legacy");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";

    StageOptions opt;
    opt.serveKeyDocument = false;
    opt.trustSignerLocally = true;
    auto f = stage(root, cja, opt);
    auto project = stageProject(root, f.repo);

    auto s = sessionIn(project);
    ASSERT_NE(nullptr, s.get());

    CellResult r = s->execute(
        cell("Packages.install(\"dev.cajeta.depx\", \"1.*\");\n"));
    EXPECT_TRUE(r.ok) << "the 9.1 fallback must keep working: "
                      << r.errorId << ": " << r.message;

    s->shutdown();
    fs::remove_all(root);
}

// 5.1.2 + 5.1.3 / spec 5.1, 5.2 — a mirror that rewrites the unsigned
// sidecar to match tampered bytes achieves nothing, because the hash the
// install is held to lives inside what the root signed.
//
// The discriminator is which failure comes back. Had the sidecar been read,
// the checksum would have MATCHED the tampered bytes and the refusal would
// have come later, from the signature. A checksum mismatch naming the
// signed metadata is only reachable if the signed hash is what was
// compared.
TEST(SessionPublisherTrustTests, aMirrorRewritingTheChecksumIsCaught) {
    auto root = freshRoot("tampered");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto f = stage(root, cja, StageOptions{});
    auto project = stageProject(root, f.repo);

    // The mirror swaps the bytes and updates the sidecar to match, which
    // is exactly as far as its powers go: the release metadata is signed
    // by a root it does not hold.
    auto archive = f.repo / "dev.cajeta.depx" / "1.0.0"
                 / "dev.cajeta.depx-1.0.0.cja";
    {
        std::ofstream out(archive, std::ios::binary | std::ios::app);
        out << "\ntampered";
    }
    writeWholeFile(fs::path(archive.string() + ".sha256"),
                   cajeta::buildtool::ArtifactCache::sha256OfFile(
                       archive.string()));

    auto s = sessionIn(project);
    ASSERT_NE(nullptr, s.get());

    CellResult r = s->execute(
        cell("Packages.install(\"dev.cajeta.depx\", \"1.*\");\n"));
    EXPECT_FALSE(r.ok) << "tampered bytes must not install";
    EXPECT_TRUE(contains(r.message, "checksum mismatch"))
        << "the SIGNED hash must be what the bytes were compared against; "
           "a signature failure here would mean the sidecar was used. Got: "
        << r.message;
    EXPECT_TRUE(contains(r.message, "root-signed release metadata"))
        << "the message must say where the expected hash came from; got: "
        << r.message;

    // Nothing spliced, session still serving.
    CellResult use = s->execute("import depx.Answer;\nAnswer.v();\n");
    EXPECT_FALSE(use.ok) << "a rejected install must splice nothing";
    CellResult alive = s->execute("1 + 1;\n");
    ASSERT_TRUE(alive.ok) << alive.errorId << ": " << alive.message;

    s->shutdown();
    fs::remove_all(root);
}
