// notebook-olla-install Unit 4 (spec 3.3, 3.5) — signature verification on
// the install path.
//
// A notebook installing code is a supply-chain surface, so the archive is
// checked against a key the MACHINE already trusts, not one the repository
// hands over with the artifact. The fixture therefore does the real thing:
// openssl generates an ed25519 keypair, `cajeta archive sign` produces the
// detached signature, and the public key is planted in a trust store the
// session reads through CAJETA_TRUST_KEYS_DIR.

#include "gtest/gtest.h"

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

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << text;
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
    writeFile(root / "dep" / "src" / "depx" / "Answer.cajeta", kAnswerSrc);
    auto out = root / "dep-out";
    fs::create_directories(out);
    std::string cmd = compilerBinary() + " depx.Answer "
        + (root / "dep" / "src").string() + " " + out.string() + " --emit=cja";
    if (runQuiet(cmd, root / "dep.log") != 0) return {};
    return out / "depx.cja";
}

// `signing` is "good" (sign with the trusted key), "bad" (sign with an
// UNTRUSTED key, so the bytes are a real signature that must still be
// rejected), or "none" (no .sig published at all).
fs::path stageRepo(const fs::path& root, const fs::path& cja,
                   const std::string& signing) {
    auto repo = root / "repo";
    auto dir = repo / "depx" / "1.0.0";
    fs::create_directories(dir);
    auto archive = dir / "depx-1.0.0.cja";
    fs::copy_file(cja, archive, fs::copy_options::overwrite_existing);
    writeFile(dir / "depx-1.0.0.cja.sha256",
              cajeta::buildtool::ArtifactCache::sha256OfFile(archive.string()));

    if (signing == "none") return repo;

    // The trusted keypair always exists; an "bad" run signs with a second,
    // untrusted one instead.
    auto keys = root / "keys";
    fs::create_directories(keys);
    auto trustedPriv = keys / "trusted.pem";
    EXPECT_EQ(0, runQuiet("openssl genpkey -algorithm ED25519 -out "
                          + trustedPriv.string(), root / "genkey.log"));
    auto trustDir = root / "trust";
    fs::create_directories(trustDir);
    EXPECT_EQ(0, runQuiet("openssl pkey -in " + trustedPriv.string()
                          + " -pubout -out "
                          + (trustDir / "notebook-test.pem").string(),
                          root / "pubkey.log"));

    fs::path signWith = trustedPriv;
    if (signing == "bad") {
        signWith = keys / "untrusted.pem";
        EXPECT_EQ(0, runQuiet("openssl genpkey -algorithm ED25519 -out "
                              + signWith.string(), root / "genkey2.log"));
    }
    EXPECT_EQ(0, runQuiet(compilerBinary() + " archive sign " + archive.string()
                          + " --key " + signWith.string()
                          + " --out " + archive.string() + ".sig",
                          root / "sign.log"));
    return repo;
}

fs::path stageProject(const fs::path& root, const fs::path& repo,
                      bool requireSignatures) {
    auto project = root / "project";
    fs::create_directories(project);
    std::ostringstream json;
    json << "{\n"
         << "  \"details\": {\n"
         << "    \"name\": \"notebook\",\n"
         << "    \"version\": \"0.1.0\",\n"
         << "    \"cajeta-lang-version\": \"1.0\"\n"
         << "  },\n"
         << "  \"settings\": {\n";
    if (requireSignatures) json << "    \"require-signatures\": true,\n";
    json << "    \"repositories\": [\n"
         << "      { \"name\": \"fixture\", \"type\": \"filesystem\", "
         << "\"path\": \"" << repo.string() << "\", \"priority\": 0 }\n"
         << "    ]\n"
         << "  }\n"
         << "}\n";
    writeFile(project / "cajeta.json", json.str());
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
    auto p = fs::temp_directory_path() / ("cajeta-sess-sig-" + tag);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

// Point the trust store at the fixture's key directory. Each test runs in
// its own process under the harness, so this cannot leak between tests.
void trustFixtureKeys(const fs::path& root) {
    setenv("CAJETA_TRUST_KEYS_DIR", (root / "trust").c_str(), 1);
}

std::string cell(const std::string& body) {
    return "import cajeta.session.Packages;\n" + body;
}

bool contains(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}

}  // namespace

// 4.1.1 (accept) / spec 3.3 — a signature made by a TRUSTED key verifies,
// and the install proceeds.
TEST(SessionPackagesSignatureTests, trustedSignatureVerifiesAndInstalls) {
    auto root = freshRoot("good");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto project = stageProject(root, stageRepo(root, cja, "good"), false);
    trustFixtureKeys(root);

    auto s = sessionIn(project);
    ASSERT_NE(nullptr, s.get());

    CellResult r = s->execute(cell("Packages.install(\"depx\", \"1.*\");\n"));
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
    EXPECT_EQ("1.0.0", r.result);

    CellResult use = s->execute("import depx.Answer;\nAnswer.v();\n");
    ASSERT_TRUE(use.ok) << use.errorId << ": " << use.message;
    EXPECT_EQ("42", use.result);

    s->shutdown();
    fs::remove_all(root);
}

// 4.1.1 (reject) / spec 3.3 — a real, well-formed signature from a key the
// machine does NOT trust is rejected. This is the interesting arm: the
// bytes verify against *a* key, just not one we accept.
TEST(SessionPackagesSignatureTests, signatureFromAnUntrustedKeyIsRejected) {
    auto root = freshRoot("untrusted");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto project = stageProject(root, stageRepo(root, cja, "bad"), false);
    trustFixtureKeys(root);

    auto s = sessionIn(project);
    ASSERT_NE(nullptr, s.get());

    CellResult r = s->execute(cell("Packages.install(\"depx\", \"1.*\");\n"));
    EXPECT_FALSE(r.ok) << "an untrusted signer must not install";
    EXPECT_TRUE(contains(r.message, "signature"))
        << "must say the signature was the problem; got: " << r.message;

    // Nothing spliced, session still serving.
    CellResult use = s->execute("import depx.Answer;\nAnswer.v();\n");
    EXPECT_FALSE(use.ok) << "a rejected install must splice nothing";
    CellResult alive = s->execute("1 + 1;\n");
    ASSERT_TRUE(alive.ok) << alive.errorId << ": " << alive.message;

    s->shutdown();
    fs::remove_all(root);
}

// 4.1.2 (floor) / spec 3.5 — `require-signatures: true` rejects an archive
// that publishes no signature at all, even with a good checksum.
TEST(SessionPackagesSignatureTests, requireSignaturesRejectsAnUnsignedArchive) {
    auto root = freshRoot("required");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto project = stageProject(root, stageRepo(root, cja, "none"), true);
    trustFixtureKeys(root);

    auto s = sessionIn(project);
    ASSERT_NE(nullptr, s.get());

    CellResult r = s->execute(cell("Packages.install(\"depx\", \"1.*\");\n"));
    EXPECT_FALSE(r.ok) << "require-signatures must reject an unsigned archive";
    EXPECT_TRUE(contains(r.message, "require-signatures"))
        << "must name the setting that caused the rejection; got: "
        << r.message;

    s->shutdown();
    fs::remove_all(root);
}

// 4.1.2 (default) / spec 3.5 — WITHOUT the setting, an unsigned archive
// with a valid checksum installs. Signatures are opportunistic by default;
// the floor is opt-in.
TEST(SessionPackagesSignatureTests, unsignedArchiveInstallsWhenNotRequired) {
    auto root = freshRoot("optional");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto project = stageProject(root, stageRepo(root, cja, "none"), false);
    trustFixtureKeys(root);

    auto s = sessionIn(project);
    ASSERT_NE(nullptr, s.get());

    CellResult r = s->execute(cell("Packages.install(\"depx\", \"1.*\");\n"));
    ASSERT_TRUE(r.ok) << "unsigned + valid checksum is the default arm: "
                      << r.errorId << ": " << r.message;
    EXPECT_EQ("1.0.0", r.result);

    s->shutdown();
    fs::remove_all(root);
}
