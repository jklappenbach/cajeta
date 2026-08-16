// Regression tests for the build-tool manifest loader.
// See src/cajeta/buildtool/Manifest.h and
// plans/buildtool/build-tool-plan.md Phase 0.

#include "cajeta/buildtool/Manifest.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <string>

using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::Manifest;
using cajeta::buildtool::ManifestDetails;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

} // namespace

// --- happy path ----------------------------------------------------------





// --- error cases (each documented validation has a regression test) ------




TEST(ManifestTests, errorsOnNonStringDetailsName) {
    auto m = loadManifestString(R"({"details":{"name":42,"version":"0.1"}})");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("'name' must be a string"), std::string::npos);
}

TEST(ManifestTests, errorsOnUnknownTopLevelBlock) {
    // Typo: "settigns" instead of "settings".
    auto m = loadManifestString(
        R"({"details":{"name":"a.b","version":"0.1"},"settigns":{}})");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("unknown top-level block 'settigns'"),
              std::string::npos);
    // The error lists the valid block names so the developer can find
    // the typo without consulting the docs.
    EXPECT_NE(msg.find("settings"), std::string::npos);
}






// ─── Melt mutual exclusion (Phase 6c acceptance) ──────────────────

TEST(ManifestTests, errorsWhenMeltDeclaredAlongsideTasks) {
    auto m = loadManifestString(R"({
        "details": { "name": "p.melt", "version": "1.0.0" },
        "melt": { "dependencies": {} },
        "tasks": { "build": { "actions": [] } }
    })");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("'melt' and 'tasks'"), std::string::npos);
}

TEST(ManifestTests, meltAloneLoadsSuccessfully) {
    // Sanity: melt without tasks/workspace is the canonical melt
    // package shape and must parse cleanly.
    auto m = loadManifestString(R"({
        "details": { "name": "p.melt", "version": "1.0.0" },
        "melt": { "dependencies": {} }
    })");
    ASSERT_TRUE((bool)m) << errorText(m.takeError());
    EXPECT_TRUE(m->hasMelt);
}
