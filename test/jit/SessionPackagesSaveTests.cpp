// notebook-olla-install Unit 5 (spec 5.1-5.4) — `installAndSave`.
//
// `install` affects only the running session; the manifest is the
// reproducibility record. `installAndSave` is the separate, named act that
// graduates a session install into `cajeta.json` — a distinct method on
// purpose, so the call site says what it did and there is no boolean to
// misread.

#include "gtest/gtest.h"

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/ManifestEditor.h"
#include "cajeta/kernel/KernelSession.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
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

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
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

fs::path stageRepo(const fs::path& root, const fs::path& cja) {
    auto repo = root / "repo";
    auto dir = repo / "depx" / "1.0.0";
    fs::create_directories(dir);
    auto archive = dir / "depx-1.0.0.cja";
    fs::copy_file(cja, archive, fs::copy_options::overwrite_existing);
    writeFile(dir / "depx-1.0.0.cja.sha256",
              cajeta::buildtool::ArtifactCache::sha256OfFile(archive.string()));
    return repo;
}

// The manifest carries a COMMENT and deliberate indentation so the
// format-preserving property is observable, not merely asserted.
std::string manifestText(const fs::path& repo, const std::string& pinned) {
    std::ostringstream json;
    json << "{\n"
         << "    // The notebook's project. This comment must survive an\n"
         << "    // installAndSave — that is the whole point of the editor.\n"
         << "    \"details\": {\n"
         << "        \"name\":    \"notebook\",\n"
         << "        \"version\": \"0.1.0\",\n"
         << "        \"cajeta-lang-version\": \"1.0\"\n"
         << "    },\n"
         << "\n"
         << "    \"settings\": {\n";
    if (!pinned.empty()) {
        json << "        \"dependencies\": {\n"
             << "            \"depx\": \"" << pinned << "\"\n"
             << "        },\n";
    }
    json << "        \"repositories\": [\n"
         << "            { \"name\": \"fixture\", \"type\": \"filesystem\", "
         << "\"path\": \"" << repo.string() << "\", \"priority\": 0 }\n"
         << "        ]\n"
         << "    }\n"
         << "}\n";
    return json.str();
}

fs::path stageProject(const fs::path& root, const fs::path& repo,
                      const std::string& pinned = "") {
    auto project = root / "project";
    fs::create_directories(project);
    writeFile(project / "cajeta.json", manifestText(repo, pinned));
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

std::unique_ptr<KernelSession> bareSession() {
    std::string error;
    auto s = KernelSession::create(&error);
    EXPECT_NE(nullptr, s.get()) << "session create failed: " << error;
    return s;
}

fs::path freshRoot(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("cajeta-sess-save-" + tag);
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

struct Streamed {
    std::mutex mutex;
    std::string text;
    KernelSession::StreamHandler handler() {
        return [this](const std::string& chunk) {
            std::lock_guard<std::mutex> lock(mutex);
            text += chunk;
        };
    }
    std::string get() {
        std::lock_guard<std::mutex> lock(mutex);
        return text;
    }
};

}  // namespace

// 5.1.1 / spec 5.2 — the write preserves comments and formatting. The
// oracle is the ManifestEditor itself: what installAndSave leaves on disk
// must be byte-identical to what `cajeta add` would have produced from the
// same starting manifest.
TEST(SessionPackagesSaveTests, installAndSaveWritesTheManifestLikeCajetaAdd) {
    auto root = freshRoot("write");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto repo = stageRepo(root, cja);
    auto project = stageProject(root, repo);
    std::string before = readFile(project / "cajeta.json");

    auto s = sessionIn(project);
    ASSERT_NE(nullptr, s.get());
    CellResult r = s->execute(
        cell("Packages.installAndSave(\"depx\", \"1.*\");\n"));
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
    EXPECT_EQ("1.0.0", r.result);

    std::string after = readFile(project / "cajeta.json");
    auto expected = cajeta::buildtool::addDependencyToManifest(
        before, "depx", "1.*");
    ASSERT_TRUE(!!expected) << "editor oracle failed";
    EXPECT_EQ(*expected, after)
        << "installAndSave must write exactly what `cajeta add` writes";

    // And the properties that matter, stated directly rather than only
    // implied by the oracle.
    EXPECT_TRUE(contains(after, "that is the whole point of the editor"))
        << "the comment was lost";
    EXPECT_TRUE(contains(after, "depx")) << "the dependency was not written";

    s->shutdown();
    fs::remove_all(root);
}

// 5.1.2 / spec 5.3 — no governing project is a located error that names
// the way forward, not a silent no-op.
TEST(SessionPackagesSaveTests, noGoverningProjectSuggestsInitNotebook) {
    auto root = freshRoot("noproject");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";

    auto s = bareSession();     // no projectDir at all
    ASSERT_NE(nullptr, s.get());

    CellResult r = s->execute(
        cell("Packages.installAndSave(\"" + cja.string() + "\", \"*\");\n"));
    EXPECT_FALSE(r.ok) << "there is no manifest to write";
    EXPECT_TRUE(contains(r.message, "cajeta init notebook"))
        << "must suggest the fix; got: " << r.message;

    s->shutdown();
    fs::remove_all(root);
}

// 5.1.3 / spec 5.4 — an unchanged constraint writes nothing at all; a
// changed one rewrites and says so on the stream.
TEST(SessionPackagesSaveTests, unchangedPinWritesNothingChangedPinSaysSo) {
    auto root = freshRoot("pin");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto repo = stageRepo(root, cja);
    auto project = stageProject(root, repo, "1.*");   // already pinned

    {   // Same constraint: the file must not be touched, byte for byte.
        auto s = sessionIn(project);
        ASSERT_NE(nullptr, s.get());
        std::string before = readFile(project / "cajeta.json");
        CellResult r = s->execute(
            cell("Packages.installAndSave(\"depx\", \"1.*\");\n"));
        ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
        EXPECT_EQ(before, readFile(project / "cajeta.json"))
            << "an unchanged constraint must not rewrite the manifest";
        s->shutdown();
    }

    {   // Different constraint: rewritten, and the stream says so.
        auto s = sessionIn(project);
        ASSERT_NE(nullptr, s.get());
        Streamed streamed;
        s->setStreamHandler(streamed.handler());
        CellResult r = s->execute(
            cell("Packages.installAndSave(\"depx\", \"1.0.*\");\n"));
        ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
        std::string after = readFile(project / "cajeta.json");
        EXPECT_TRUE(contains(after, "1.0.*"))
            << "the changed constraint was not written; manifest:\n" << after;
        EXPECT_TRUE(contains(streamed.get(), "cajeta.json"))
            << "a manifest rewrite must be announced; stream was:\n"
            << streamed.get();
        s->shutdown();
    }

    fs::remove_all(root);
}

// 5.1.4 / spec 5.1 — the difference the two methods exist to express.
// A session-only install is gone after a restart; a saved one loads at
// session start with no install call in the notebook at all.
TEST(SessionPackagesSaveTests, savedDependencyLoadsAtSessionStartPlainDoesNot) {
    auto root = freshRoot("restart");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto repo = stageRepo(root, cja);
    auto project = stageProject(root, repo);

    {   // Session-only install, then throw the session away.
        auto s = sessionIn(project);
        ASSERT_NE(nullptr, s.get());
        CellResult r = s->execute(
            cell("Packages.install(\"depx\", \"1.*\");\n"));
        ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
        s->shutdown();
    }
    {   // Restart: the library is gone, because nothing recorded it.
        auto s = sessionIn(project);
        ASSERT_NE(nullptr, s.get());
        CellResult use = s->execute("import depx.Answer;\nAnswer.v();\n");
        EXPECT_FALSE(use.ok)
            << "a session-only install must not survive a restart";
        s->shutdown();
    }
    {   // Now save it.
        auto s = sessionIn(project);
        ASSERT_NE(nullptr, s.get());
        CellResult r = s->execute(
            cell("Packages.installAndSave(\"depx\", \"1.*\");\n"));
        ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
        s->shutdown();
    }
    {   // Restart: it is on the classpath at start, no install call.
        auto s = sessionIn(project);
        ASSERT_NE(nullptr, s.get());
        CellResult use = s->execute("import depx.Answer;\nAnswer.v();\n");
        ASSERT_TRUE(use.ok) << "a saved dependency must load at session "
                               "start: " << use.errorId << ": " << use.message;
        EXPECT_EQ("42", use.result);
        s->shutdown();
    }

    fs::remove_all(root);
}
