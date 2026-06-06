// Tests for the `cajeta upgrade` planner. Uses filesystem-backed
// repositories laid out under temp dirs (same fixture pattern as
// DependencyTests).

#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Upgrader.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

using cajeta::buildtool::applyUpgradePlan;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::planUpgrade;
using cajeta::buildtool::UpgradePlan;

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
                 ("cajeta-up-test-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    // Lay out a filesystem-repo with packages + versions, writing a
    // `.cja` stub for each. Pulled from DependencyTests' helper.
    struct PkgVer {
        std::string pkg;
        std::string version;
        std::string content;
    };
    std::filesystem::path makeFsRepo(const std::vector<PkgVer>& contents) {
        auto root = makeTempDir("fsrepo");
        for (const auto& pv : contents) {
            auto dir = root / pv.pkg / pv.version;
            std::filesystem::create_directories(dir);
            auto file = dir / (pv.pkg + "-" + pv.version + ".cja");
            std::ofstream o(file, std::ios::binary);
            o << pv.content;
        }
        return root;
    }

    // Drop a sidecar with a `settings.capabilities` array (so the
    // planner has something to diff). `caps` may be empty.
    void writeCapsSidecar(
        const std::filesystem::path& repoRoot,
        const std::string& pkg, const std::string& version,
        const std::vector<std::string>& caps) {
        std::ostringstream js;
        js << "{\"details\":{\"name\":\"" << pkg
           << "\",\"version\":\"" << version << "\"}";
        if (!caps.empty()) {
            js << ",\"settings\":{\"capabilities\":[";
            for (size_t i = 0; i < caps.size(); ++i) {
                if (i) js << ",";
                js << "\"" << caps[i] << "\"";
            }
            js << "]}";
        }
        js << "}";
        auto path = repoRoot / pkg / version / "cajeta.json";
        std::ofstream o(path, std::ios::binary);
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

    // Build a manifest source string that references the given repo
    // path and dep map. Each `deps` entry is name → constraint.
    std::string makeManifestSrc(
        const std::filesystem::path& repoRoot,
        const std::vector<std::pair<std::string, std::string>>& deps) {
        std::ostringstream out;
        out << R"({
  "details": { "name": "demo.app", "version": "0.1.0" },
  "settings": {
    "repositories": [
      { "name": "local", "type": "filesystem", "path": ")"
            << repoRoot.generic_string() << R"(" }
    ],
    "dependencies": {)";
        for (size_t i = 0; i < deps.size(); ++i) {
            out << (i ? ",\n      " : "\n      ")
                << "\"" << deps[i].first << "\": \""
                << deps[i].second << "\"";
        }
        out << "\n    }\n  }\n}\n";
        return out.str();
    }

} // namespace

// ─── plan: empty deps → error ─────────────────────────────────────────

TEST(UpgraderTests, errorsWhenNoDepsDeclared) {
    auto repoRoot = makeFsRepo({});
    auto proj = makeTempDir("plan-nodeps");
    auto m = mustLoad(R"({
        "details": { "name": "d", "version": "0.1.0" },
        "settings": { "repositories": [
            { "name": "l", "type": "filesystem", "path": "/tmp" }
        ]}
    })");
    auto plan = planUpgrade(m, proj.string(), {}, {});
    ASSERT_FALSE(static_cast<bool>(plan));
    EXPECT_NE(errorText(plan.takeError()).find("nothing to upgrade"),
              std::string::npos);
}

// ─── plan: single upgrade target available ────────────────────────────

TEST(UpgraderTests, picksHighestAvailableVersion) {
    auto repoRoot = makeFsRepo({
        {"acme.lib", "1.0.0", "v1"},
        {"acme.lib", "1.2.0", "v2"},
        {"acme.lib", "1.5.3", "v3"},
    });
    writeCapsSidecar(repoRoot, "acme.lib", "1.0.0", {});
    writeCapsSidecar(repoRoot, "acme.lib", "1.2.0", {});
    writeCapsSidecar(repoRoot, "acme.lib", "1.5.3", {});

    auto src = makeManifestSrc(repoRoot, {{"acme.lib", "1.0.0"}});
    auto m = mustLoad(src);
    auto proj = makeTempDir("plan-single");
    auto plan = planUpgrade(m, proj.string(), {}, {});
    ASSERT_TRUE(static_cast<bool>(plan)) << errorText(plan.takeError());
    ASSERT_EQ(plan->entries.size(), 1u);
    EXPECT_EQ(plan->entries[0].name, "acme.lib");
    EXPECT_EQ(plan->entries[0].oldVersion, "1.0.0");
    EXPECT_EQ(plan->entries[0].newVersion, "1.5.3");
    EXPECT_EQ(plan->entries[0].newConstraint, "1.5.3");
    EXPECT_TRUE(plan->entries[0].changed);
    EXPECT_TRUE(plan->entries[0].capDelta.empty());
    EXPECT_TRUE(plan->anyChange());
    EXPECT_FALSE(plan->anyCapabilityChange());
}

// ─── plan: explicit @version ─────────────────────────────────────────

TEST(UpgraderTests, honorsExplicitTargetVersion) {
    auto repoRoot = makeFsRepo({
        {"acme.lib", "1.0.0", "v1"},
        {"acme.lib", "1.2.0", "v2"},
        {"acme.lib", "1.5.3", "v3"},
    });
    writeCapsSidecar(repoRoot, "acme.lib", "1.0.0", {});
    writeCapsSidecar(repoRoot, "acme.lib", "1.2.0", {});
    writeCapsSidecar(repoRoot, "acme.lib", "1.5.3", {});

    auto src = makeManifestSrc(repoRoot, {{"acme.lib", "1.0.0"}});
    auto m = mustLoad(src);
    auto proj = makeTempDir("plan-explicit");
    std::map<std::string, std::string> explicits = {{"acme.lib", "1.2.0"}};
    auto plan = planUpgrade(m, proj.string(),
                             {"acme.lib"}, explicits);
    ASSERT_TRUE(static_cast<bool>(plan)) << errorText(plan.takeError());
    ASSERT_EQ(plan->entries.size(), 1u);
    EXPECT_EQ(plan->entries[0].newVersion, "1.2.0");
    EXPECT_EQ(plan->entries[0].newConstraint, "1.2.0");
}

// ─── plan: explicit @version not in any repo → error ──────────────────

TEST(UpgraderTests, errorsWhenExplicitVersionMissing) {
    auto repoRoot = makeFsRepo({
        {"acme.lib", "1.0.0", "v1"},
        {"acme.lib", "1.2.0", "v2"},
    });
    writeCapsSidecar(repoRoot, "acme.lib", "1.0.0", {});
    writeCapsSidecar(repoRoot, "acme.lib", "1.2.0", {});

    auto src = makeManifestSrc(repoRoot, {{"acme.lib", "1.0.0"}});
    auto m = mustLoad(src);
    auto proj = makeTempDir("plan-missing");
    std::map<std::string, std::string> explicits = {{"acme.lib", "9.9.9"}};
    auto plan = planUpgrade(m, proj.string(),
                             {"acme.lib"}, explicits);
    ASSERT_FALSE(static_cast<bool>(plan));
    EXPECT_NE(errorText(plan.takeError()).find("no repository carries"),
              std::string::npos);
}

// ─── plan: dep not declared → error ──────────────────────────────────

TEST(UpgraderTests, errorsWhenDepNotDeclared) {
    auto repoRoot = makeFsRepo({{"acme.lib", "1.0.0", "v1"}});
    writeCapsSidecar(repoRoot, "acme.lib", "1.0.0", {});

    auto src = makeManifestSrc(repoRoot, {{"acme.lib", "1.0.0"}});
    auto m = mustLoad(src);
    auto proj = makeTempDir("plan-undecl");
    auto plan = planUpgrade(m, proj.string(),
                             {"not.declared"}, {});
    ASSERT_FALSE(static_cast<bool>(plan));
    EXPECT_NE(errorText(plan.takeError()).find("not declared"),
              std::string::npos);
}

// ─── plan: already at highest → no change row ─────────────────────────

TEST(UpgraderTests, marksAlreadyAtHighest) {
    auto repoRoot = makeFsRepo({
        {"acme.lib", "1.0.0", "v1"},
        {"acme.lib", "1.5.3", "v3"},
    });
    writeCapsSidecar(repoRoot, "acme.lib", "1.0.0", {});
    writeCapsSidecar(repoRoot, "acme.lib", "1.5.3", {});

    auto src = makeManifestSrc(repoRoot, {{"acme.lib", "1.5.3"}});
    auto m = mustLoad(src);
    auto proj = makeTempDir("plan-noop");
    auto plan = planUpgrade(m, proj.string(), {}, {});
    ASSERT_TRUE(static_cast<bool>(plan)) << errorText(plan.takeError());
    ASSERT_EQ(plan->entries.size(), 1u);
    EXPECT_FALSE(plan->entries[0].changed);
    EXPECT_EQ(plan->entries[0].newVersion, "1.5.3");
    EXPECT_FALSE(plan->anyChange());
}

// ─── plan: multi-dep upgrade ──────────────────────────────────────────

TEST(UpgraderTests, planMultipleDepsAtOnce) {
    auto repoRoot = makeFsRepo({
        {"acme.lib", "1.0.0", "a1"},
        {"acme.lib", "2.0.0", "a2"},
        {"acme.util", "0.1.0", "u1"},
        {"acme.util", "0.3.5", "u2"},
    });
    writeCapsSidecar(repoRoot, "acme.lib", "1.0.0", {});
    writeCapsSidecar(repoRoot, "acme.lib", "2.0.0", {});
    writeCapsSidecar(repoRoot, "acme.util", "0.1.0", {});
    writeCapsSidecar(repoRoot, "acme.util", "0.3.5", {});

    auto src = makeManifestSrc(repoRoot, {
        {"acme.lib", "1.0.0"},
        {"acme.util", "0.1.0"},
    });
    auto m = mustLoad(src);
    auto proj = makeTempDir("plan-multi");
    auto plan = planUpgrade(m, proj.string(), {}, {});
    ASSERT_TRUE(static_cast<bool>(plan)) << errorText(plan.takeError());
    ASSERT_EQ(plan->entries.size(), 2u);
    // Both upgrade.
    EXPECT_TRUE(plan->entries[0].changed);
    EXPECT_TRUE(plan->entries[1].changed);
}

// ─── plan: capability addition flagged ─────────────────────────────────

TEST(UpgraderTests, flagsAddedCapabilities) {
    auto repoRoot = makeFsRepo({
        {"acme.lib", "1.0.0", "v1"},
        {"acme.lib", "2.0.0", "v2"},
    });
    writeCapsSidecar(repoRoot, "acme.lib", "1.0.0", {"filesystem"});
    writeCapsSidecar(repoRoot, "acme.lib", "2.0.0",
                     {"filesystem", "network"});

    auto src = makeManifestSrc(repoRoot, {{"acme.lib", "1.0.0"}});
    auto m = mustLoad(src);
    auto proj = makeTempDir("plan-capadd");
    auto plan = planUpgrade(m, proj.string(), {}, {});
    ASSERT_TRUE(static_cast<bool>(plan)) << errorText(plan.takeError());
    ASSERT_EQ(plan->entries.size(), 1u);
    EXPECT_TRUE(plan->entries[0].changed);
    ASSERT_EQ(plan->entries[0].capDelta.added.size(), 1u);
    EXPECT_EQ(plan->entries[0].capDelta.added[0], "network");
    EXPECT_TRUE(plan->entries[0].capDelta.removed.empty());
    EXPECT_TRUE(plan->anyCapabilityChange());
}

// ─── plan: capability removal flagged ─────────────────────────────────

TEST(UpgraderTests, flagsRemovedCapabilities) {
    auto repoRoot = makeFsRepo({
        {"acme.lib", "1.0.0", "v1"},
        {"acme.lib", "2.0.0", "v2"},
    });
    writeCapsSidecar(repoRoot, "acme.lib", "1.0.0",
                     {"filesystem", "network"});
    writeCapsSidecar(repoRoot, "acme.lib", "2.0.0", {"filesystem"});

    auto src = makeManifestSrc(repoRoot, {{"acme.lib", "1.0.0"}});
    auto m = mustLoad(src);
    auto proj = makeTempDir("plan-caprem");
    auto plan = planUpgrade(m, proj.string(), {}, {});
    ASSERT_TRUE(static_cast<bool>(plan)) << errorText(plan.takeError());
    ASSERT_EQ(plan->entries.size(), 1u);
    EXPECT_TRUE(plan->entries[0].capDelta.added.empty());
    ASSERT_EQ(plan->entries[0].capDelta.removed.size(), 1u);
    EXPECT_EQ(plan->entries[0].capDelta.removed[0], "network");
    EXPECT_TRUE(plan->anyCapabilityChange());
}

// ─── apply: rewrites manifest constraint ─────────────────────────────

TEST(UpgraderTests, applyRewritesConstraintInSource) {
    auto repoRoot = makeFsRepo({
        {"acme.lib", "1.0.0", "v1"},
        {"acme.lib", "1.5.3", "v3"},
    });
    writeCapsSidecar(repoRoot, "acme.lib", "1.0.0", {});
    writeCapsSidecar(repoRoot, "acme.lib", "1.5.3", {});

    auto src = makeManifestSrc(repoRoot, {{"acme.lib", "1.0.0"}});
    auto m = mustLoad(src);
    auto proj = makeTempDir("apply");
    auto plan = planUpgrade(m, proj.string(), {}, {});
    ASSERT_TRUE(static_cast<bool>(plan)) << errorText(plan.takeError());
    auto rewritten = applyUpgradePlan(src, *plan);
    ASSERT_TRUE(static_cast<bool>(rewritten))
        << errorText(rewritten.takeError());
    EXPECT_NE(rewritten->find("\"acme.lib\": \"1.5.3\""),
              std::string::npos)
        << "rewritten manifest:\n" << *rewritten;
    EXPECT_EQ(rewritten->find("\"acme.lib\": \"1.0.0\""),
              std::string::npos);
}

// ─── apply: no-change plan is a no-op ─────────────────────────────────

TEST(UpgraderTests, applyNoopWhenNothingChanged) {
    auto repoRoot = makeFsRepo({{"acme.lib", "1.0.0", "v1"}});
    writeCapsSidecar(repoRoot, "acme.lib", "1.0.0", {});
    auto src = makeManifestSrc(repoRoot, {{"acme.lib", "1.0.0"}});
    auto m = mustLoad(src);
    auto proj = makeTempDir("apply-noop");
    auto plan = planUpgrade(m, proj.string(), {}, {});
    ASSERT_TRUE(static_cast<bool>(plan)) << errorText(plan.takeError());
    auto rewritten = applyUpgradePlan(src, *plan);
    ASSERT_TRUE(static_cast<bool>(rewritten))
        << errorText(rewritten.takeError());
    EXPECT_EQ(*rewritten, src);
}

// ─── plan: no version of dep in repo → error ─────────────────────────

TEST(UpgraderTests, errorsWhenRepoLacksThePackage) {
    auto repoRoot = makeFsRepo({{"other.lib", "1.0.0", "v"}});
    writeCapsSidecar(repoRoot, "other.lib", "1.0.0", {});

    auto src = makeManifestSrc(repoRoot, {{"acme.lib", "1.0.0"}});
    auto m = mustLoad(src);
    auto proj = makeTempDir("plan-missingpkg");
    auto plan = planUpgrade(m, proj.string(), {}, {});
    ASSERT_FALSE(static_cast<bool>(plan));
    EXPECT_NE(errorText(plan.takeError()).find(
                  "no repository has any version"),
              std::string::npos);
}

// ─── plan: empty repository list → error ─────────────────────────────

TEST(UpgraderTests, errorsWhenNoRepositoriesConfigured) {
    auto src = R"({
        "details": { "name": "demo", "version": "0.1.0" },
        "settings": {
            "dependencies": { "acme.lib": "1.0.0" }
        }
    })";
    auto m = mustLoad(src);
    auto proj = makeTempDir("plan-norepos");
    auto plan = planUpgrade(m, proj.string(), {}, {});
    ASSERT_FALSE(static_cast<bool>(plan));
    EXPECT_NE(errorText(plan.takeError()).find(
                  "settings.repositories is empty"),
              std::string::npos);
}
