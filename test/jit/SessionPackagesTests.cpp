// notebook-olla-install Unit 2 (spec 2.1, 2.3-2.5, 2.7) — the stdlib API
// and the host bridge. Unit 1 proved the splice from C++; these drive it
// from a CELL through `cajeta.session.Packages`, which is the surface the
// spec settles on (a stdlib API, not a magic — spec §1).
//
// Resolution is stubbed here on purpose (plan 2.1.1): `name` is a local
// .cja path and the constraint is matched against the archive's own
// version. Unit 3 swaps in NativeResolver behind the same call.

#include "gtest/gtest.h"

#include "cajeta/kernel/KernelSession.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using cajeta::kernel::CellResult;
using cajeta::kernel::KernelSession;

namespace fs = std::filesystem;

namespace {

std::string compilerBinary() {
    if (const char* env = std::getenv("CAJETA_BINARY")) return env;
    // Suite runs from build/ under an IDE launch but from the REPO ROOT
    // under cajeta_tests.sh — probe both (the v0.21.1 release-gate find).
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

// Build a one-package library archive; empty path on failure. The direct
// --emit=cja path stamps version 1.0.0 (Compiler.cpp), which every
// constraint assertion below is written against.
fs::path buildDep(const fs::path& root, const std::string& stem,
                  const std::string& entryClass, const std::string& source) {
    std::string cls = entryClass.substr(entryClass.rfind('.') + 1);
    writeFile(root / stem / "src" / "depx" / (cls + ".cajeta"), source);
    auto out = root / (stem + "-out");
    fs::create_directories(out);
    auto log = root / (stem + ".log");
    std::string cmd = compilerBinary() + " " + entryClass + " "
        + (root / stem / "src").string() + " " + out.string()
        + " --emit=cja > " + log.string() + " 2>&1";
    if (exitCodeOf(std::system(cmd.c_str())) != 0) return {};
    return out / "depx.cja";
}

std::unique_ptr<KernelSession> session() {
    std::string error;
    auto s = KernelSession::create(&error);
    EXPECT_NE(nullptr, s.get()) << "session create failed: " << error;
    return s;
}

fs::path freshRoot(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("cajeta-sess-pkg-" + tag);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

const char* kAnswerSrc =
    "package depx;\n"
    "public class Answer {\n"
    "    public static int32 v() { return 42; }\n"
    "}\n";

// Every cell that calls the API needs the import; keep it in one place so
// the tests read as the notebook would.
std::string cell(const std::string& body) {
    return "import cajeta.session.Packages;\n" + body;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// 2.1.1 / spec 2.1 — install from a CELL, returning the resolved version.
TEST(SessionPackagesTests, installFromACellReturnsTheResolvedVersion) {
    auto root = freshRoot("returns-version");
    auto cja = buildDep(root, "basic", "depx.Answer", kAnswerSrc);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";

    auto s = session();
    ASSERT_NE(nullptr, s.get());

    CellResult r = s->execute(
        cell("Packages.install(\"" + cja.string() + "\", \"*\");\n"));
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
    ASSERT_TRUE(r.hasResult);
    EXPECT_EQ("1.0.0", r.result);

    // And the install actually spliced: the NEXT cell imports it (spec 2.2).
    CellResult use = s->execute("import depx.Answer;\nAnswer.v();\n");
    ASSERT_TRUE(use.ok) << use.errorId << ": " << use.message;
    EXPECT_EQ("42", use.result);

    s->shutdown();
    fs::remove_all(root);
}

// 2.1.2 / spec 2.3 — the SAME cell that installs cannot import it (the
// cell was compiled before the install ran). The hint must name the fix.
TEST(SessionPackagesTests, sameCellImportFailsAndTheHintNamesTheNextCell) {
    auto root = freshRoot("same-cell");
    auto cja = buildDep(root, "samecell", "depx.Answer", kAnswerSrc);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";

    auto s = session();
    ASSERT_NE(nullptr, s.get());

    CellResult r = s->execute(
        "import cajeta.session.Packages;\n"
        "import depx.Answer;\n"
        "Packages.install(\"" + cja.string() + "\", \"*\");\n"
        "Answer.v();\n");
    EXPECT_FALSE(r.ok) << "same-cell import must not resolve";
    EXPECT_TRUE(contains(r.message, "next cell"))
        << "diagnostic must name the next-cell fix; got: " << r.message;

    // Nothing was installed — the cell failed to COMPILE, so its install
    // never ran. The session is still serving (spec 6.2)...
    CellResult alive = s->execute("1 + 1;\n");
    ASSERT_TRUE(alive.ok) << alive.errorId << ": " << alive.message;

    // ...and following the hint actually works, which is what makes the
    // hint worth printing: install in one cell, import in the next.
    CellResult put = s->execute(
        cell("Packages.install(\"" + cja.string() + "\", \"*\");\n"));
    ASSERT_TRUE(put.ok) << put.errorId << ": " << put.message;

    CellResult use = s->execute("import depx.Answer;\nAnswer.v();\n");
    ASSERT_TRUE(use.ok) << use.errorId << ": " << use.message;
    EXPECT_EQ("42", use.result);

    s->shutdown();
    fs::remove_all(root);
}

// 2.1.3 / spec 2.4 — re-install at a SATISFYING version is a no-op that
// returns the loaded version, so run-all top-to-bottom is safe.
TEST(SessionPackagesTests, reinstallAtASatisfyingVersionIsANoOp) {
    auto root = freshRoot("reinstall");
    auto cja = buildDep(root, "reinst", "depx.Answer", kAnswerSrc);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";

    auto s = session();
    ASSERT_NE(nullptr, s.get());

    CellResult first = s->execute(
        cell("Packages.install(\"" + cja.string() + "\", \"*\");\n"));
    ASSERT_TRUE(first.ok) << first.errorId << ": " << first.message;
    EXPECT_EQ("1.0.0", first.result);

    CellResult again = s->execute(
        cell("Packages.install(\"" + cja.string() + "\", \"1.*\");\n"));
    ASSERT_TRUE(again.ok) << again.errorId << ": " << again.message;
    EXPECT_EQ("1.0.0", again.result);

    // Still usable after the no-op.
    CellResult use = s->execute("import depx.Answer;\nAnswer.v();\n");
    ASSERT_TRUE(use.ok) << use.errorId << ": " << use.message;
    EXPECT_EQ("42", use.result);

    s->shutdown();
    fs::remove_all(root);
}

// 2.1.4 / spec 2.5 — a constraint that EXCLUDES the loaded version is a
// located error naming both versions and stating that a restart is needed.
TEST(SessionPackagesTests, installExcludedByTheLoadedVersionNamesBothAndSaysRestart) {
    auto root = freshRoot("conflict");
    auto cja = buildDep(root, "conflict", "depx.Answer", kAnswerSrc);
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";

    auto s = session();
    ASSERT_NE(nullptr, s.get());

    CellResult first = s->execute(
        cell("Packages.install(\"" + cja.string() + "\", \"*\");\n"));
    ASSERT_TRUE(first.ok) << first.errorId << ": " << first.message;

    CellResult clash = s->execute(
        cell("Packages.install(\"" + cja.string() + "\", \"2.*\");\n"));
    EXPECT_FALSE(clash.ok) << "an excluded version must not install";
    EXPECT_TRUE(contains(clash.message, "1.0.0"))
        << "must name the LOADED version; got: " << clash.message;
    EXPECT_TRUE(contains(clash.message, "2.*"))
        << "must name the REQUESTED constraint; got: " << clash.message;
    EXPECT_TRUE(contains(clash.message, "restart"))
        << "must state that a session restart is required; got: " << clash.message;

    // The session survives the rejection (spec 6.2).
    CellResult after = s->execute("1 + 1;\n");
    ASSERT_TRUE(after.ok) << after.errorId << ": " << after.message;
    EXPECT_EQ("2", after.result);

    s->shutdown();
    fs::remove_all(root);
}

// 2.1.5 / spec 2.7 — in a host with no live session the call throws a
// located recoverable error rather than crashing or silently no-opping.
TEST(SessionPackagesTests, noLiveSessionThrowsTheLocatedHostError) {
    auto root = freshRoot("no-session");
    writeFile(root / "src" / "app" / "Main.cajeta",
        "package app;\n"
        "import cajeta.session.Packages;\n"
        "public class Main {\n"
        "    public static void run() {\n"
        "        Packages.install(\"depx\", \"*\");\n"
        "    }\n"
        "}\n");

    auto log = root / "jit.log";
    std::string cmd = compilerBinary() + " jit-run " + (root / "src").string()
        + " app.Main.run > " + log.string() + " 2>&1";
    int rc = exitCodeOf(std::system(cmd.c_str()));

    std::ifstream in(log);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());

    EXPECT_NE(0, rc) << "install with no session must fail the run; log:\n" << out;
    EXPECT_TRUE(contains(out, "no live session"))
        << "must name the absent session; got:\n" << out;

    fs::remove_all(root);
}
