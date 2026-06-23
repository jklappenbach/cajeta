// Regression tests for the Phase 5b clean action.
//
// Pins:
//   - Default: removes build/, reports the count + bytes reclaimed.
//   - deep=true with yes=true: also removes .cajeta/cache/.
//   - Action result outputs surface the totals.
//
// Confirmation-prompt behavior on TTY is covered by the manual CLI
// flow rather than these tests — gtest doesn't run on a TTY so the
// `yes=true` path is the relevant code path.

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Properties.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using cajeta::buildtool::ActionRegistry;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::Manifest;
using cajeta::buildtool::resolveProperties;
using cajeta::buildtool::TaskContext;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    std::filesystem::path tempProject(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-clean-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    void writeFile(const std::filesystem::path& path,
                   const std::string& contents) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << contents;
    }

    Manifest makeManifest() {
        auto m = loadManifestString(
            R"({"details":{"name":"a.b","version":"0.1"}})");
        EXPECT_TRUE((bool)m);
        return std::move(*m);
    }

} // namespace

TEST(CleanActionTests, defaultRemovesBuildDir) {
    auto root = tempProject("default");
    auto savedCwd = std::filesystem::current_path();
    std::filesystem::current_path(root);

    writeFile(root / "build" / "a.bc", "12345");
    writeFile(root / "build" / "sub" / "b.bc", "678");
    writeFile(root / ".cajeta" / "cache" / "ir" / "x.bc", "should-stay");

    ActionRegistry reg;
    const auto* clean = reg.get("clean");
    ASSERT_NE(clean, nullptr);

    auto m = makeManifest();
    auto props = resolveProperties(m);
    ASSERT_TRUE((bool)props);
    TaskContext ctx(*props, &m);

    llvm::json::Object params;
    auto r = clean->run(params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_FALSE(std::filesystem::exists(root / "build"));
    EXPECT_TRUE(std::filesystem::exists(root / ".cajeta" / "cache"));
    EXPECT_EQ(r->outputs["deep"], "false");
    EXPECT_EQ(r->outputs["removed-entries"], "2");
    EXPECT_EQ(r->outputs["removed-bytes"], "8");

    std::filesystem::current_path(savedCwd);
}

TEST(CleanActionTests, deepWithYesAlsoRemovesCache) {
    auto root = tempProject("deep");
    auto savedCwd = std::filesystem::current_path();
    std::filesystem::current_path(root);

    writeFile(root / "build" / "a.bc", "12");
    writeFile(root / ".cajeta" / "cache" / "ir" / "x.bc", "abc");
    writeFile(root / ".cajeta" / "cache" / "ir" / "y.bc", "de");

    ActionRegistry reg;
    const auto* clean = reg.get("clean");
    ASSERT_NE(clean, nullptr);

    auto m = makeManifest();
    auto props = resolveProperties(m);
    ASSERT_TRUE((bool)props);
    TaskContext ctx(*props, &m);

    llvm::json::Object params;
    params["deep"] = true;
    params["yes"]  = true;
    auto r = clean->run(params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_FALSE(std::filesystem::exists(root / "build"));
    EXPECT_FALSE(std::filesystem::exists(root / ".cajeta" / "cache"));
    EXPECT_EQ(r->outputs["deep"], "true");
    // 1 build file (2 bytes) + 2 cache files (3 + 2 bytes) = 3 entries / 7 bytes
    EXPECT_EQ(r->outputs["removed-entries"], "3");
    EXPECT_EQ(r->outputs["removed-bytes"], "7");

    std::filesystem::current_path(savedCwd);
}

// 3b.1.1 — a deep clean wipes the per-project .cajeta/cache but never the
// machine-global ~/.olla store (a separate root; clean has no path into it).
TEST(CleanActionTests, deepCleanLeavesOllaUntouched) {
    auto root = tempProject("olla-safe");
    auto olla = tempProject("olla-store");
    auto savedCwd = std::filesystem::current_path();
    std::filesystem::current_path(root);

    writeFile(root / ".cajeta" / "cache" / "ir" / "x.bc", "ir");
    auto kept = olla / "dev.codec" / "0.5.0" / "dev.codec-0.5.0.cja";
    writeFile(kept, "OLLA-ARTIFACT");

    ActionRegistry reg;
    const auto* clean = reg.get("clean");
    ASSERT_NE(clean, nullptr);

    auto m = makeManifest();
    auto props = resolveProperties(m);
    ASSERT_TRUE((bool)props);
    TaskContext ctx(*props, &m);

    llvm::json::Object params;
    params["deep"] = true;
    params["yes"]  = true;
    auto r = clean->run(params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_FALSE(std::filesystem::exists(root / ".cajeta" / "cache"));
    EXPECT_TRUE(std::filesystem::exists(kept))
        << "~/.olla must survive cajeta clean (U3b / spec 2.2.4)";

    std::filesystem::current_path(savedCwd);
    std::filesystem::remove_all(root);
    std::filesystem::remove_all(olla);
}

TEST(CleanActionTests, defaultOnEmptyProjectIsBenign) {
    auto root = tempProject("empty");
    auto savedCwd = std::filesystem::current_path();
    std::filesystem::current_path(root);

    ActionRegistry reg;
    const auto* clean = reg.get("clean");
    ASSERT_NE(clean, nullptr);

    auto m = makeManifest();
    auto props = resolveProperties(m);
    ASSERT_TRUE((bool)props);
    TaskContext ctx(*props, &m);

    llvm::json::Object params;
    auto r = clean->run(params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->outputs["removed-entries"], "0");
    EXPECT_EQ(r->outputs["removed-bytes"], "0");

    std::filesystem::current_path(savedCwd);
}
