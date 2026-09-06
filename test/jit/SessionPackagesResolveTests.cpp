// notebook-olla-install Unit 3 (spec 3.1, 3.2, 3.4, 2.6, 6.1) — resolution,
// fetch, checksum, and cache behind `Packages.install`.
//
// Every test here runs OFFLINE against a filesystem repository fixture.
// That is deliberate: the one real network install is 3.3.1, run by hand
// and recorded in the plan, never in CI.

#include "gtest/gtest.h"

#include "cajeta/buildtool/ArtifactCache.h"
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

int exitCodeOf(int rc) {
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

// Build depx.cja once per fixture root.
fs::path buildDep(const fs::path& root) {
    writeFile(root / "dep" / "src" / "depx" / "Answer.cajeta", kAnswerSrc);
    auto out = root / "dep-out";
    fs::create_directories(out);
    auto log = root / "dep.log";
    std::string cmd = compilerBinary() + " depx.Answer "
        + (root / "dep" / "src").string() + " " + out.string()
        + " --emit=cja > " + log.string() + " 2>&1";
    if (exitCodeOf(std::system(cmd.c_str())) != 0) return {};
    return out / "depx.cja";
}

// Lay the archive out the way FilesystemRepository expects:
//   <repo>/<name>/<version>/<name>-<version>.cja
// `checksum` writes the sha256 sidecar: "" = none published,
// "bad" = a deliberately wrong one, "real" = the true hash.
fs::path stageRepo(const fs::path& root, const fs::path& cja,
                   const std::string& checksum) {
    auto repo = root / "repo";
    auto dir = repo / "depx" / "1.0.0";
    fs::create_directories(dir);
    fs::copy_file(cja, dir / "depx-1.0.0.cja",
                  fs::copy_options::overwrite_existing);
    if (checksum == "real") {
        writeFile(dir / "depx-1.0.0.cja.sha256",
                  cajeta::buildtool::ArtifactCache::sha256OfFile(
                      (dir / "depx-1.0.0.cja").string()));
    } else if (checksum == "bad") {
        writeFile(dir / "depx-1.0.0.cja.sha256",
                  "sha256:" + std::string(64, 'a'));
    }
    return repo;
}

// A project whose manifest points at the fixture repository — this is what
// makes the install resolve against "the governing manifest's repositories"
// rather than the default central.
fs::path stageProject(const fs::path& root, const fs::path& repo) {
    auto project = root / "project";
    fs::create_directories(project);
    // `name`/`version` live under `details` — the loader rejects unknown
    // top-level blocks outright.
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
         << "\"path\": \"" << repo.generic_string() << "\", \"priority\": 0 }\n"
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
    auto p = fs::temp_directory_path() / ("cajeta-sess-resolve-" + tag);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

std::string cell(const std::string& body) {
    return "import cajeta.session.Packages;\n" + body;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Accumulates the cell's stream output so the narration (6.1) is testable.
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

// 3.1.1 / spec 3.1 — a constraint resolves against the GOVERNING MANIFEST's
// repositories, by name, with no path anywhere in the cell.
TEST(SessionPackagesResolveTests, constraintResolvesAgainstProjectRepositories) {
    auto root = freshRoot("resolve");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto project = stageProject(root, stageRepo(root, cja, "real"));

    auto s = sessionIn(project);
    ASSERT_NE(nullptr, s.get());

    CellResult r = s->execute(cell("Packages.install(\"depx\", \"1.*\");\n"));
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
    ASSERT_TRUE(r.hasResult);
    EXPECT_EQ("1.0.0", r.result);

    CellResult use = s->execute("import depx.Answer;\nAnswer.v();\n");
    ASSERT_TRUE(use.ok) << use.errorId << ": " << use.message;
    EXPECT_EQ("42", use.result);

    s->shutdown();
    fs::remove_all(root);
}

// 3.1.2 / spec 3.2 — a checksum mismatch discards the bytes and fails
// located. Nothing is spliced, and the session stays usable.
TEST(SessionPackagesResolveTests, checksumMismatchInstallsNothingAndKeepsServing) {
    auto root = freshRoot("checksum");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto project = stageProject(root, stageRepo(root, cja, "bad"));

    auto s = sessionIn(project);
    ASSERT_NE(nullptr, s.get());

    CellResult r = s->execute(cell("Packages.install(\"depx\", \"1.*\");\n"));
    EXPECT_FALSE(r.ok) << "a mismatched checksum must not install";
    EXPECT_TRUE(contains(r.message, "checksum mismatch"))
        << "must say what was wrong; got: " << r.message;

    // No half-installed state: the archive is NOT importable.
    CellResult use = s->execute("import depx.Answer;\nAnswer.v();\n");
    EXPECT_FALSE(use.ok) << "nothing should have been spliced";

    // And the session is still serving (spec 6.2).
    CellResult alive = s->execute("1 + 1;\n");
    ASSERT_TRUE(alive.ok) << alive.errorId << ": " << alive.message;
    EXPECT_EQ("2", alive.result);

    s->shutdown();
    fs::remove_all(root);
}

// 3.1.3 / spec 3.4 — a cached artifact installs with the REPOSITORY GONE,
// and a miss with no repository names the cache it looked in.
TEST(SessionPackagesResolveTests, cacheHitServesWithTheRepositoryRemoved) {
    auto root = freshRoot("cache");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto repo = stageRepo(root, cja, "real");
    auto project = stageProject(root, repo);

    {   // First session populates the artifact cache under the project.
        auto warm = sessionIn(project);
        ASSERT_NE(nullptr, warm.get());
        CellResult r = warm->execute(
            cell("Packages.install(\"depx\", \"1.*\");\n"));
        ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
        warm->shutdown();
    }

    // The repository is now unavailable — only the cache can answer.
    fs::remove_all(repo / "depx" / "1.0.0" / "depx-1.0.0.cja");

    auto s = sessionIn(project);
    ASSERT_NE(nullptr, s.get());
    CellResult r = s->execute(cell("Packages.install(\"depx\", \"1.*\");\n"));
    ASSERT_TRUE(r.ok) << "a cache hit must not need the repository: "
                      << r.errorId << ": " << r.message;
    EXPECT_EQ("1.0.0", r.result);

    CellResult use = s->execute("import depx.Answer;\nAnswer.v();\n");
    ASSERT_TRUE(use.ok) << use.errorId << ": " << use.message;
    EXPECT_EQ("42", use.result);

    s->shutdown();
    fs::remove_all(root);
}

// 3.1.4 / spec 2.6 — an unmatched constraint names the constraint AND every
// repository consulted, so the reader knows which to fix.
TEST(SessionPackagesResolveTests, unmatchedConstraintNamesItAndEveryRepository) {
    auto root = freshRoot("unmatched");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto project = stageProject(root, stageRepo(root, cja, "real"));

    auto s = sessionIn(project);
    ASSERT_NE(nullptr, s.get());

    CellResult r = s->execute(cell("Packages.install(\"depx\", \"9.*\");\n"));
    EXPECT_FALSE(r.ok) << "no 9.x exists in the fixture";
    EXPECT_TRUE(contains(r.message, "9.*"))
        << "must name the constraint; got: " << r.message;
    EXPECT_TRUE(contains(r.message, "fixture"))
        << "must name the repository consulted; got: " << r.message;

    s->shutdown();
    fs::remove_all(root);
}

// 3.1.5 / spec 6.1 — the install narrates its phases on the cell's stream,
// so a fetch is never a silent stall.
TEST(SessionPackagesResolveTests, installNarratesItsPhasesOnTheCellStream) {
    auto root = freshRoot("narrate");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto project = stageProject(root, stageRepo(root, cja, "real"));

    auto s = sessionIn(project);
    ASSERT_NE(nullptr, s.get());
    Streamed streamed;
    s->setStreamHandler(streamed.handler());

    CellResult r = s->execute(cell("Packages.install(\"depx\", \"1.*\");\n"));
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;

    std::string out = streamed.get();
    EXPECT_TRUE(contains(out, "resolving depx"))
        << "resolve phase missing; stream was:\n" << out;
    EXPECT_TRUE(contains(out, "depx 1.0.0"))
        << "no phase named the resolved version; stream was:\n" << out;
    EXPECT_TRUE(contains(out, "splicing"))
        << "splice phase missing; stream was:\n" << out;

    s->shutdown();
    fs::remove_all(root);
}
