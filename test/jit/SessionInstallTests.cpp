// notebook-olla-install Unit 1 (spec 4.1-4.4, 2.2) — the mid-session
// splice, local archive. No network, no stdlib API: KernelSession grows
// installArchive(path) and these tests prove the accumulating-world seams
// before anything is built on top. The fixture archive is built by the
// CLI (--emit=cja), the same oracle the classpath tests use.

#include "gtest/gtest.h"

#include "cajeta/jit/LazyCodegen.h"
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
    // The suite runs from build/ under an IDE/gtest launch but from the
    // REPO ROOT under cajeta_tests.sh — probe both, else the fixture
    // archive silently fails to build and every test here goes red in
    // the sweep while passing solo (v0.21.1 release-gate find).
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

// Build a one-package library archive; empty path on failure.
fs::path buildDep(const fs::path& root, const std::string& stem,
                  const std::string& entryClass, const std::string& source) {
    // File named for the CLASS: the archive writer derives entry names from
    // the module canonical (path-derived), and the collision scan reads
    // those entry names.
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
    auto p = fs::temp_directory_path() / ("cajeta-sess-install-" + tag);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

}  // namespace

// 1.1.1 / spec 4.1, 2.2 — splice between cells; the NEXT cell imports a
// class from the archive and runs it.
TEST(SessionInstallTests, splicedArchiveImportsInTheNextCell) {
    auto root = freshRoot("basic");
    auto cja = buildDep(root, "Answer", "depx.Answer",
        "package depx;\n"
        "public class Answer {\n"
        "    public static int32 v() { return 42; }\n"
        "}\n");
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";

    auto s = session();
    ASSERT_NE(nullptr, s.get());
    CellResult before = s->execute("20 + 1;\n");
    ASSERT_TRUE(before.ok) << before.errorId << ": " << before.message;

    std::string err;
    ASSERT_TRUE(s->installArchive(cja.string(), &err)) << err;

    CellResult after = s->execute("import depx.Answer;\nAnswer.v();\n");
    ASSERT_TRUE(after.ok) << after.errorId << ": " << after.message;
    ASSERT_TRUE(after.hasResult);
    EXPECT_EQ("42", after.result);
    s->shutdown();
    fs::remove_all(root);
}

// 1.1.2 / spec 4.2 — the archive and the session instantiate the SAME
// stdlib specialization; dedupe holds across the splice (this is the
// two-worlds bitcast hazard 7.2.5 documented, measured on purpose).
TEST(SessionInstallTests, spliceDedupesSharedStdlibInstantiations) {
    auto root = freshRoot("dedupe");
    auto cja = buildDep(root, "Bag", "depx.Bag",
        "package depx;\n"
        "import cajeta.collection.ArrayList;\n"
        "public class Bag {\n"
        "    public static int32 sum2(int32 a, int32 b) {\n"
        "        ArrayList<int32> xs = heap ArrayList<int32>();\n"
        "        xs.add(a);\n"
        "        xs.add(b);\n"
        "        return xs.get(0) + xs.get(1);\n"
        "    }\n"
        "}\n");
    ASSERT_FALSE(cja.empty()) << "fixture archive failed to build";

    auto s = session();
    ASSERT_NE(nullptr, s.get());
    // The SESSION instantiates ArrayList<int32> first.
    CellResult mine = s->execute(
        "import cajeta.collection.ArrayList;\n"
        "ArrayList<int32> xs = heap ArrayList<int32>();\n"
        "xs.add(5);\n"
        "xs.get(0);\n");
    ASSERT_TRUE(mine.ok) << mine.errorId << ": " << mine.message;

    std::string err;
    ASSERT_TRUE(s->installArchive(cja.string(), &err)) << err;

    CellResult theirs = s->execute("import depx.Bag;\nBag.sum2(40, 2);\n");
    ASSERT_TRUE(theirs.ok) << theirs.errorId << ": " << theirs.message;
    EXPECT_EQ("42", theirs.result);
    s->shutdown();
    fs::remove_all(root);
}

// 1.1.3 / spec 4.3 — a second archive declaring an already-loaded
// canonical name is rejected; the session stays usable and the original
// class answers unchanged.
TEST(SessionInstallTests, collidingArchiveIsRejectedAndSessionSurvives) {
    auto root = freshRoot("collide");
    auto first = buildDep(root, "One", "depx.Answer",
        "package depx;\n"
        "public class Answer {\n"
        "    public static int32 v() { return 1; }\n"
        "}\n");
    auto second = buildDep(root, "Two", "depx.Answer",
        "package depx;\n"
        "public class Answer {\n"
        "    public static int32 v() { return 2; }\n"
        "}\n");
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());

    auto s = session();
    ASSERT_NE(nullptr, s.get());
    s->execute("20 + 1;\n");
    std::string err;
    ASSERT_TRUE(s->installArchive(first.string(), &err)) << err;
    EXPECT_FALSE(s->installArchive(second.string(), &err))
        << "a colliding archive was spliced";
    EXPECT_NE(std::string::npos, err.find("depx.Answer"))
        << "the collision error does not name the class: " << err;

    CellResult r = s->execute("import depx.Answer;\nAnswer.v();\n");
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
    EXPECT_EQ("1", r.result) << "the original class did not survive";
    s->shutdown();
    fs::remove_all(root);
}

// 1.1.4 / spec 4.4 — spliced bodies arrive through the lazy generator,
// not an eager wave at install time.
TEST(SessionInstallTests, splicedBodiesDeliverLazily) {
    // Lazy delivery is what this test measures, so force the mode on for its
    // lifetime: COFF and Mach-O hosts default to EAGER (LazyCodegen.cpp), and
    // under eager the dep body is generated at install and never crosses the
    // generator — the counter assertion below then fails for a reason that
    // has nothing to do with splicing (measured on Windows, 2026-09-06).
    struct ModeGuard {
        bool saved = cajeta::lazyCodegenEnabled();
        ~ModeGuard() { cajeta::setLazyCodegenEnabled(saved); }
    } modeGuard;
    cajeta::setLazyCodegenEnabled(true);
    auto root = freshRoot("lazy");
    auto cja = buildDep(root, "Answer", "depx.Answer",
        "package depx;\n"
        "public class Answer {\n"
        "    public static int32 v() { return 7; }\n"
        "}\n");
    ASSERT_FALSE(cja.empty());

    auto s = session();
    ASSERT_NE(nullptr, s.get());
    s->execute("20 + 1;\n");
    const auto beforeEager = s->stats().eagerBodiesGenerated;

    std::string err;
    ASSERT_TRUE(s->installArchive(cja.string(), &err)) << err;
    EXPECT_LT(s->stats().eagerBodiesGenerated - beforeEager, 500)
        << "the splice eagerly emitted a world";

    const auto beforeLazy = s->stats().lazyBodiesDelivered;
    CellResult r = s->execute("import depx.Answer;\nAnswer.v();\n");
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
    EXPECT_EQ("7", r.result);
    EXPECT_GT(s->stats().lazyBodiesDelivered, beforeLazy)
        << "no body came through the generator for the dep call";
    s->shutdown();
    fs::remove_all(root);
}

// 1.1.5 / spec 2.1's execution shape — the splice re-enters from WITHIN a
// running cell (the host-hook shape Unit 2 will use). The stream handler
// fires on the session thread mid-execute; installing there must not
// deadlock the gate, and the next cell sees the archive.
TEST(SessionInstallTests, installDuringACellDoesNotDeadlock) {
    auto root = freshRoot("reenter");
    auto cja = buildDep(root, "Answer", "depx.Answer",
        "package depx;\n"
        "public class Answer {\n"
        "    public static int32 v() { return 9; }\n"
        "}\n");
    ASSERT_FALSE(cja.empty());

    auto s = session();
    ASSERT_NE(nullptr, s.get());
    bool installed = false;
    std::string err;
    s->setStreamHandler([&](const std::string&) {
        if (!installed) {
            installed = s->installArchive(cja.string(), &err);
        }
    });
    CellResult printer = s->execute(
        "System.stdout.println(\"go\");\n"
        "20 + 1;\n");
    ASSERT_TRUE(printer.ok) << printer.errorId << ": " << printer.message;
    ASSERT_TRUE(installed) << "mid-cell install failed: " << err;

    CellResult r = s->execute("import depx.Answer;\nAnswer.v();\n");
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
    EXPECT_EQ("9", r.result);
    s->shutdown();
    fs::remove_all(root);
}
