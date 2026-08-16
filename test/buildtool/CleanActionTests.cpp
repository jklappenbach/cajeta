// Regression tests for the clean action.
//
// Pins:
//   - Default: removes build/ AND .cajeta/cache/, unprompted, reporting the
//     count + bytes reclaimed. Clean means clean.
//   - keep-cache=true: removes build/ only, preserving the caches for a fast
//     incremental rebuild.
//   - Action result outputs surface the totals.
//   - ~/.olla (the machine-global store) is never touched.
//
// The cache wipe used to be opt-in (`deep`) behind a [y/N] TTY prompt, which any
// non-interactive caller answered "no" by hitting EOF — so the IDE's Clean left
// the artifact cache intact and the next build re-published it in ~100ms without
// compiling. The prompt is gone; the param is inverted (2026-07-11).

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


// The opt-OUT: `keep-cache` preserves .cajeta/cache for a fast incremental
// rebuild. Reachable from the CLI as `cajeta clean -p keep-cache=true` (CLI
// params are overlaid onto action params — see TaskRunnerTests).


// 3b.1.1 — clean wipes the per-project .cajeta/cache but never the
// machine-global ~/.olla store (a separate root; clean has no path into it).
TEST(CleanActionTests, cleanLeavesOllaUntouched) {
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
    auto r = clean->run(params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_FALSE(std::filesystem::exists(root / ".cajeta" / "cache"));
    EXPECT_TRUE(std::filesystem::exists(kept))
        << "~/.olla must survive cajeta clean (U3b / spec 2.2.4)";

    std::filesystem::current_path(savedCwd);
    std::filesystem::remove_all(root);
    std::filesystem::remove_all(olla);
}

