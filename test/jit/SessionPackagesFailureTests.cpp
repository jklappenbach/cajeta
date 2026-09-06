// notebook-olla-install Unit 6 (spec 6.2; 4.3) — failure containment.
//
// The property under test is one property, checked against every way an
// install can be rejected: the kernel keeps serving. A dead kernel costs a
// notebook every binding in the session, so a bad install must cost the
// user the install and nothing else.
//
// 6.1.2 additionally pins WHERE a collision is reported. The splice queues
// to the cell boundary, so a rejection found at drain would arrive after
// `install` had already returned a version — invisible to the notebook.

#include "gtest/gtest.h"
#include "../PortableEnv.h"   // setenv/unsetenv — absent from the MinGW CRT

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

// `checksum` is "real" or "bad"; "bad" is the 3.2 failure class.
fs::path stageRepo(const fs::path& root, const fs::path& cja,
                   const std::string& checksum) {
    auto repo = root / "repo";
    auto dir = repo / "depx" / "1.0.0";
    fs::create_directories(dir);
    auto archive = dir / "depx-1.0.0.cja";
    fs::copy_file(cja, archive, fs::copy_options::overwrite_existing);
    writeFile(dir / "depx-1.0.0.cja.sha256",
              checksum == "bad"
                  ? "sha256:" + std::string(64, 'a')
                  : cajeta::buildtool::ArtifactCache::sha256OfFile(
                        archive.string()));
    return repo;
}

fs::path stageProject(const fs::path& root, const fs::path& repo,
                      bool requireSignatures = false) {
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
    auto p = fs::temp_directory_path() / ("cajeta-sess-fail-" + tag);
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

// The property every case below shares: after the rejection, the session
// still compiles and runs a cell, and still holds the bindings made before
// it. A kernel that survives but has forgotten the session is not serving.
void expectStillServing(KernelSession& s, const char* what) {
    CellResult alive = s.execute("earlier + 1;\n");
    ASSERT_TRUE(alive.ok) << what << ": the kernel stopped serving — "
                          << alive.errorId << ": " << alive.message;
    EXPECT_EQ("42", alive.result)
        << what << ": the session lost bindings made before the failure";
}

// Bind a value the survival check reads back, so "still serving" means the
// session is intact, not merely that a fresh cell compiles.
void seedBinding(KernelSession& s) {
    CellResult r = s.execute("int32 earlier = 41;\n");
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
}

}  // namespace

// 6.1.1 (2.6) — nothing satisfies the constraint.
TEST(SessionPackagesFailureTests, unresolvableConstraintLeavesTheKernelServing) {
    auto root = freshRoot("unresolvable");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto s = sessionIn(stageProject(root, stageRepo(root, cja, "real")));
    ASSERT_NE(nullptr, s.get());
    seedBinding(*s);

    CellResult r = s->execute(cell("Packages.install(\"depx\", \"9.*\");\n"));
    EXPECT_FALSE(r.ok);
    expectStillServing(*s, "unresolvable constraint");

    s->shutdown();
    fs::remove_all(root);
}

// 6.1.1 (3.2) — the bytes do not match the published checksum.
TEST(SessionPackagesFailureTests, checksumMismatchLeavesTheKernelServing) {
    auto root = freshRoot("checksum");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto s = sessionIn(stageProject(root, stageRepo(root, cja, "bad")));
    ASSERT_NE(nullptr, s.get());
    seedBinding(*s);

    CellResult r = s->execute(cell("Packages.install(\"depx\", \"1.*\");\n"));
    EXPECT_FALSE(r.ok);
    expectStillServing(*s, "checksum mismatch");

    s->shutdown();
    fs::remove_all(root);
}

// 6.1.1 (3.5) — the require-signatures floor rejects an unsigned archive.
TEST(SessionPackagesFailureTests, requireSignaturesLeavesTheKernelServing) {
    auto root = freshRoot("requiresig");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto s = sessionIn(stageProject(root, stageRepo(root, cja, "real"), true));
    ASSERT_NE(nullptr, s.get());
    seedBinding(*s);

    CellResult r = s->execute(cell("Packages.install(\"depx\", \"1.*\");\n"));
    EXPECT_FALSE(r.ok);
    expectStillServing(*s, "require-signatures");

    s->shutdown();
    fs::remove_all(root);
}

// 6.1.1 (2.5) — a constraint excluding the loaded version.
TEST(SessionPackagesFailureTests, versionConflictLeavesTheKernelServing) {
    auto root = freshRoot("conflict");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto s = sessionIn(stageProject(root, stageRepo(root, cja, "real")));
    ASSERT_NE(nullptr, s.get());
    seedBinding(*s);

    CellResult first = s->execute(cell("Packages.install(\"depx\", \"1.*\");\n"));
    ASSERT_TRUE(first.ok) << first.errorId << ": " << first.message;

    CellResult clash = s->execute(cell("Packages.install(\"depx\", \"2.*\");\n"));
    EXPECT_FALSE(clash.ok);
    expectStillServing(*s, "version conflict");

    s->shutdown();
    fs::remove_all(root);
}

// 6.1.1 (5.3) — installAndSave with no governing project.
TEST(SessionPackagesFailureTests, saveWithNoProjectLeavesTheKernelServing) {
    auto root = freshRoot("nosave");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";

    std::string error;
    auto s = KernelSession::create(&error);   // no project
    ASSERT_NE(nullptr, s.get()) << error;
    seedBinding(*s);

    CellResult r = s->execute(
        cell("Packages.installAndSave(\"" + cja.generic_string() + "\", \"*\");\n"));
    EXPECT_FALSE(r.ok);
    expectStillServing(*s, "installAndSave with no project");

    s->shutdown();
    fs::remove_all(root);
}

// 6.1.2 / spec 4.3 — the OTHER collision arm: a class an earlier CELL
// declared. This silently never fired until 2026-08-28.
//
// Cause, measured: a cell's classes do not keep the package they declare.
// The script-unit pass rewrites them into the reserved `cajeta.script`
// package, so `package depx; class Answer` in a cell registers as
// `cajeta.script.Answer` — never `depx.Answer`. The scan compared
// canonicals only, so an archive declaring `depx.Answer` matched nothing
// and spliced straight over the session's class.
//
// The scan now also compares the simple name under `cajeta.script`, which
// is the one package that holds cell declarations.
TEST(SessionPackagesFailureTests, collisionWithACellDefinedClassIsRejected) {
    auto root = freshRoot("collision");
    auto cja = buildDep(root);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";
    auto s = sessionIn(stageProject(root, stageRepo(root, cja, "real")));
    ASSERT_NE(nullptr, s.get());
    seedBinding(*s);

    CellResult defined = s->execute(
        "package depx;\n"
        "public class Answer { public static int32 v() { return 7; } }\n");
    ASSERT_TRUE(defined.ok) << defined.errorId << ": " << defined.message;

    CellResult r = s->execute(cell("Packages.install(\"depx\", \"1.*\");\n"));
    EXPECT_FALSE(r.ok)
        << "an install must never shadow a class the session already holds";
    EXPECT_TRUE(contains(r.message, "declared by an earlier cell"))
        << "must say WHICH kind of collision this is — the archive arm's "
           "\"already loaded\" would leave the reader hunting for another "
           "archive; got: " << r.message;
    EXPECT_TRUE(contains(r.message, "Answer"))
        << "must name the class; got: " << r.message;

    CellResult still = s->execute("Answer.v();\n");
    ASSERT_TRUE(still.ok) << still.errorId << ": " << still.message;
    EXPECT_EQ("7", still.result) << "the install shadowed session state";
    expectStillServing(*s, "collision");

    s->shutdown();
    fs::remove_all(root);
}
