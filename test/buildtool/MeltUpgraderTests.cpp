// Regression tests for the Phase 6c melt-upgrade path:
//   - planMeltUpgrade computes the (oldVersion, newVersion) pair
//     for each declared melt + diffs the curated dep tables.
//   - applyMeltUpgradePlan rewrites the settings.melts[] entry in
//     place via setMeltImportInManifest.
//   - Edge cases: explicit version, name not declared, no melts,
//     already at highest, multi-melt manifest.

#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/ManifestEditor.h"
#include "cajeta/buildtool/Upgrader.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

using cajeta::buildtool::applyMeltUpgradePlan;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::MeltUpgradePlan;
using cajeta::buildtool::planMeltUpgrade;
using cajeta::buildtool::setMeltImportInManifest;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    std::filesystem::path makeTempDir(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-meltup-test-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    void stageMelt(const std::filesystem::path& root,
                   const std::string& name,
                   const std::string& version,
                   const std::string& meltBody) {
        auto dir = root / name / version;
        std::filesystem::create_directories(dir);
        { std::ofstream o(dir / (name + "-" + version + ".cja"),
                          std::ios::binary);
          o << "stub"; }
        std::ostringstream js;
        js << "{\"details\":{\"name\":\"" << name
           << "\",\"version\":\"" << version << "\"},"
           << "\"melt\":{" << meltBody << "}}";
        std::ofstream o(dir / "cajeta.json", std::ios::binary);
        o << js.str();
    }

    cajeta::buildtool::Manifest mustLoad(const std::string& src) {
        auto m = loadManifestString(src);
        if (!m) {
            ADD_FAILURE() << errorText(m.takeError());
            return {};
        }
        return std::move(*m);
    }

} // namespace

// ─── planMeltUpgrade ──────────────────────────────────────────────


TEST(MeltUpgraderTests, planExplicitVersionPicksThatVersion) {
    auto root = makeTempDir("repo");
    stageMelt(root, "platform.melt", "1.0.0",
              "\"dependencies\":{}");
    stageMelt(root, "platform.melt", "1.2.0",
              "\"dependencies\":{}");
    stageMelt(root, "platform.melt", "1.5.0",
              "\"dependencies\":{}");

    std::ostringstream src;
    src << R"({
        "details": { "name": "c", "version": "0.1.0" },
        "settings": {
            "repositories": [
                { "name": "local", "type": "filesystem",
                  "path": ")" << root.generic_string() << R"(" }
            ],
            "melts": [ "platform.melt@1.0.0" ]
        }
    })";
    auto m = mustLoad(src.str());
    auto proj = makeTempDir("proj");

    auto plan = planMeltUpgrade(
        m, proj.string(), {"platform.melt"},
        {{"platform.melt", "1.2.0"}});
    ASSERT_TRUE((bool)plan) << errorText(plan.takeError());
    ASSERT_EQ(plan->entries.size(), 1u);
    EXPECT_EQ(plan->entries[0].newVersion, "1.2.0");
}


TEST(MeltUpgraderTests, planErrorsForEmptyMeltsBlock) {
    auto m = mustLoad(R"({
        "details": { "name": "c", "version": "0.1.0" }
    })");
    auto proj = makeTempDir("proj");
    auto plan = planMeltUpgrade(m, proj.string(), {});
    ASSERT_FALSE((bool)plan);
    EXPECT_NE(errorText(plan.takeError()).find("settings.melts is empty"),
              std::string::npos);
}

TEST(MeltUpgraderTests, planNoChangeWhenAlreadyAtHighest) {
    auto root = makeTempDir("repo");
    stageMelt(root, "p.melt", "1.0.0", "\"dependencies\":{}");

    std::ostringstream src;
    src << R"({
        "details": { "name": "c", "version": "0.1.0" },
        "settings": {
            "repositories": [
                { "name": "local", "type": "filesystem",
                  "path": ")" << root.generic_string() << R"(" }
            ],
            "melts": [ "p.melt@1.0.0" ]
        }
    })";
    auto m = mustLoad(src.str());
    auto proj = makeTempDir("proj");
    auto plan = planMeltUpgrade(m, proj.string(), {});
    ASSERT_TRUE((bool)plan) << errorText(plan.takeError());
    EXPECT_FALSE(plan->entries[0].changed);
    EXPECT_FALSE(plan->anyChange());
}

// ─── dep-table diff surfaces ──────────────────────────────────────

TEST(MeltUpgraderTests, depDeltaSurfacesAddedRemovedChanged) {
    auto root = makeTempDir("repo");
    stageMelt(root, "p.melt", "1.0.0",
              "\"dependencies\":{"
              "\"keep.same\":\"1.0.0\","
              "\"will.change\":\"1.0.0\","
              "\"will.remove\":\"0.5.0\"}");
    stageMelt(root, "p.melt", "2.0.0",
              "\"dependencies\":{"
              "\"keep.same\":\"1.0.0\","
              "\"will.change\":\"2.0.0\","
              "\"newly.added\":\"3.0.0\"}");

    std::ostringstream src;
    src << R"({
        "details": { "name": "c", "version": "0.1.0" },
        "settings": {
            "repositories": [
                { "name": "local", "type": "filesystem",
                  "path": ")" << root.generic_string() << R"(" }
            ],
            "melts": [ "p.melt@1.0.0" ]
        }
    })";
    auto m = mustLoad(src.str());
    auto proj = makeTempDir("proj");
    auto plan = planMeltUpgrade(m, proj.string(), {});
    ASSERT_TRUE((bool)plan) << errorText(plan.takeError());
    ASSERT_EQ(plan->entries.size(), 1u);

    const auto& d = plan->entries[0].depDelta;
    ASSERT_EQ(d.added.size(), 1u);
    EXPECT_EQ(d.added[0].first, "newly.added");
    EXPECT_EQ(d.added[0].second, "3.0.0");

    ASSERT_EQ(d.removed.size(), 1u);
    EXPECT_EQ(d.removed[0], "will.remove");

    ASSERT_EQ(d.changed.size(), 1u);
    EXPECT_EQ(std::get<0>(d.changed[0]), "will.change");
    EXPECT_EQ(std::get<1>(d.changed[0]), "1.0.0");
    EXPECT_EQ(std::get<2>(d.changed[0]), "2.0.0");
}

// ─── apply: rewrites settings.melts in place ──────────────────────

TEST(MeltUpgraderTests, applyRewritesMeltsEntry) {
    auto root = makeTempDir("repo");
    stageMelt(root, "p.melt", "1.0.0", "\"dependencies\":{}");
    stageMelt(root, "p.melt", "1.5.0", "\"dependencies\":{}");

    std::ostringstream src;
    src << R"({
        "details": { "name": "c", "version": "0.1.0" },
        "settings": {
            "repositories": [
                { "name": "local", "type": "filesystem",
                  "path": ")" << root.generic_string() << R"(" }
            ],
            "melts": [ "p.melt@1.0.0" ]
        }
    })";
    std::string srcText = src.str();
    auto m = mustLoad(srcText);
    auto proj = makeTempDir("proj");

    auto plan = planMeltUpgrade(m, proj.string(), {});
    ASSERT_TRUE((bool)plan);
    auto rewritten = applyMeltUpgradePlan(srcText, *plan);
    ASSERT_TRUE((bool)rewritten) << errorText(rewritten.takeError());

    EXPECT_NE(rewritten->find("\"p.melt@1.5.0\""), std::string::npos);
    EXPECT_EQ(rewritten->find("\"p.melt@1.0.0\""), std::string::npos);
}


// ─── setMeltImportInManifest directly ─────────────────────────────


TEST(MeltUpgraderTests, setMeltImportErrorsForMissingEntry) {
    std::string src = R"({
    "details": { "name": "c", "version": "0.1.0" },
    "settings": {
        "melts": [ "other.melt@1.0.0" ]
    }
})";
    auto out = setMeltImportInManifest(
        src, "ghost.melt", "1.0.0", "2.0.0");
    ASSERT_FALSE((bool)out);
    EXPECT_NE(errorText(out.takeError()).find("not declared"),
              std::string::npos);
}

