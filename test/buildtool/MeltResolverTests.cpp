// Regression tests for the Phase 6c melt resolver:
//   - resolveMelts fetches melts through the repo machinery,
//     reads each sidecar manifest, validates it's melt-shaped,
//     parses the typed melt, and recurses post-order.
//   - Constraint-table merge follows declaration order (later
//     melts override earlier on conflict).
//   - applyMeltLookups substitutes `"*"` dep constraints from the
//     melt table and errors when no melt provides a dep.
//   - Cycle detection: a melt importing itself transitively fails.
//   - Manifest end-to-end: resolveProjectDependencies wires the
//     full pipe.

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Melt.h"
#include "cajeta/buildtool/Repository.h"
#include "cajeta/buildtool/Resolver.h"
#include "cajeta/buildtool/repo/FilesystemRepository.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

using cajeta::buildtool::applyMeltLookups;
using cajeta::buildtool::ArtifactCache;
using cajeta::buildtool::DependencySpec;
using cajeta::buildtool::FilesystemRepository;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::Manifest;
using cajeta::buildtool::MeltResolution;
using cajeta::buildtool::RepositoryPtr;
using cajeta::buildtool::resolveMelts;
using cajeta::buildtool::resolveProjectDependencies;

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
                 ("cajeta-melt-test-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    Manifest mustLoad(const std::string& src) {
        auto m = loadManifestString(src);
        if (!m) {
            ADD_FAILURE() << errorText(m.takeError());
            return {};
        }
        return std::move(*m);
    }

    // Stage a filesystem-repo entry for a melt package: writes a
    // `.cja` stub plus a sidecar `cajeta.json` carrying the melt's
    // `details` + `melt` blocks. Returns the staged repo root.
    struct MeltPkg {
        std::string name;
        std::string version;
        std::string meltJson;  // raw `melt` block body, e.g. `"dependencies": {"a.b": "1.0.0"}`
    };
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

    // Stage a regular (non-melt) package with optional deps.
    void stagePkg(const std::filesystem::path& root,
                  const std::string& name,
                  const std::string& version,
                  const std::vector<std::pair<std::string, std::string>>& deps = {}) {
        auto dir = root / name / version;
        std::filesystem::create_directories(dir);
        { std::ofstream o(dir / (name + "-" + version + ".cja"),
                          std::ios::binary);
          o << "stub"; }
        std::ostringstream js;
        js << "{\"details\":{\"name\":\"" << name
           << "\",\"version\":\"" << version << "\"}";
        if (!deps.empty()) {
            js << ",\"settings\":{\"dependencies\":{";
            for (size_t i = 0; i < deps.size(); ++i) {
                if (i) js << ",";
                js << "\"" << deps[i].first << "\":\""
                   << deps[i].second << "\"";
            }
            js << "}}";
        }
        js << "}";
        std::ofstream o(dir / "cajeta.json", std::ios::binary);
        o << js.str();
    }

} // namespace

// ─── resolveMelts: single melt with one curated dep ───────────────

TEST(MeltResolverTests, singleMeltAggregatesConstraints) {
    auto root = makeTempDir("repo");
    stageMelt(root, "platform.melt", "1.0.0",
              "\"dependencies\":{\"acme.lib\":\"1.2.5\"},"
              "\"properties\":{\"platform-version\":\"2024.1\"}");

    auto src = R"({
        "details": { "name": "consumer", "version": "0.1.0" },
        "settings": {
            "melts": [ "platform.melt@1.0.0" ]
        }
    })";
    auto m = mustLoad(src);

    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    ArtifactCache cache(projectDir.string(), homeDir.string());

    auto melts = resolveMelts(m, repos, cache);
    ASSERT_TRUE((bool)melts) << errorText(melts.takeError());

    ASSERT_EQ(melts->resolvedMelts.size(), 1u);
    EXPECT_EQ(melts->resolvedMelts[0].name, "platform.melt");
    EXPECT_EQ(melts->resolvedMelts[0].version, "1.0.0");

    EXPECT_EQ(melts->depConstraints["acme.lib"], "1.2.5");
    EXPECT_EQ(melts->depProvidedBy["acme.lib"], "platform.melt@1.0.0");
    EXPECT_EQ(melts->properties["platform-version"], "2024.1");
    EXPECT_EQ(melts->propertyProvidedBy["platform-version"],
              "platform.melt@1.0.0");
}

// ─── declaration order: later melt wins on conflict ───────────────

TEST(MeltResolverTests, laterMeltOverridesEarlierOnConflict) {
    auto root = makeTempDir("repo");
    stageMelt(root, "a.melt", "1.0.0",
              "\"dependencies\":{\"acme.lib\":\"1.0.0\"}");
    stageMelt(root, "b.melt", "1.0.0",
              "\"dependencies\":{\"acme.lib\":\"1.5.0\"}");

    auto m = mustLoad(R"({
        "details": { "name": "c", "version": "0.1.0" },
        "settings": {
            "melts": [ "a.melt@1.0.0", "b.melt@1.0.0" ]
        }
    })");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    ArtifactCache cache(projectDir.string(), homeDir.string());

    auto melts = resolveMelts(m, repos, cache);
    ASSERT_TRUE((bool)melts) << errorText(melts.takeError());
    EXPECT_EQ(melts->depConstraints["acme.lib"], "1.5.0");
    EXPECT_EQ(melts->depProvidedBy["acme.lib"], "b.melt@1.0.0");
}

// ─── transitive imports: post-order ───────────────────────────────

TEST(MeltResolverTests, transitiveMeltImportsResolvePostOrder) {
    auto root = makeTempDir("repo");
    // outer.melt imports inner.melt
    stageMelt(root, "inner.melt", "1.0.0",
              "\"dependencies\":{\"common.lib\":\"1.0.0\"}");
    stageMelt(root, "outer.melt", "1.0.0",
              "\"dependencies\":{\"common.lib\":\"2.0.0\"},"
              "\"melts\":[\"inner.melt@1.0.0\"]");

    auto m = mustLoad(R"({
        "details": { "name": "c", "version": "0.1.0" },
        "settings": { "melts": [ "outer.melt@1.0.0" ] }
    })");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    ArtifactCache cache(projectDir.string(), homeDir.string());

    auto melts = resolveMelts(m, repos, cache);
    ASSERT_TRUE((bool)melts) << errorText(melts.takeError());

    // Post-order: inner is processed first, then outer's own deps
    // override. The outer melt's `2.0.0` wins.
    EXPECT_EQ(melts->depConstraints["common.lib"], "2.0.0");
    EXPECT_EQ(melts->depProvidedBy["common.lib"], "outer.melt@1.0.0");

    // Both melts appear in resolvedMelts list (inner before outer
    // per post-order).
    ASSERT_EQ(melts->resolvedMelts.size(), 2u);
    EXPECT_EQ(melts->resolvedMelts[0].name, "inner.melt");
    EXPECT_EQ(melts->resolvedMelts[1].name, "outer.melt");
    EXPECT_EQ(melts->resolvedMelts[1].transitiveMelts.size(), 1u);
}

// ─── cycle detection ──────────────────────────────────────────────

TEST(MeltResolverTests, cycleDetectedAndCited) {
    auto root = makeTempDir("repo");
    stageMelt(root, "a.melt", "1.0.0",
              "\"melts\":[\"b.melt@1.0.0\"]");
    stageMelt(root, "b.melt", "1.0.0",
              "\"melts\":[\"a.melt@1.0.0\"]");

    auto m = mustLoad(R"({
        "details": { "name": "c", "version": "0.1.0" },
        "settings": { "melts": [ "a.melt@1.0.0" ] }
    })");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    ArtifactCache cache(projectDir.string(), homeDir.string());

    auto melts = resolveMelts(m, repos, cache);
    ASSERT_FALSE((bool)melts);
    auto msg = errorText(melts.takeError());
    EXPECT_NE(msg.find("cycle"), std::string::npos);
    EXPECT_NE(msg.find("a.melt"), std::string::npos);
    EXPECT_NE(msg.find("b.melt"), std::string::npos);
}

// ─── melt-shaped manifest required ────────────────────────────────

TEST(MeltResolverTests, importingANonMeltPackageFails) {
    auto root = makeTempDir("repo");
    stagePkg(root, "not.a.melt", "1.0.0", {});

    auto m = mustLoad(R"({
        "details": { "name": "c", "version": "0.1.0" },
        "settings": { "melts": [ "not.a.melt@1.0.0" ] }
    })");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    ArtifactCache cache(projectDir.string(), homeDir.string());

    auto melts = resolveMelts(m, repos, cache);
    ASSERT_FALSE((bool)melts);
    EXPECT_NE(errorText(melts.takeError()).find("no 'melt' block"),
              std::string::npos);
}

TEST(MeltResolverTests, missingMeltFailsClearly) {
    auto root = makeTempDir("repo");
    // empty repo

    auto m = mustLoad(R"({
        "details": { "name": "c", "version": "0.1.0" },
        "settings": { "melts": [ "ghost.melt@9.9.9" ] }
    })");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    ArtifactCache cache(projectDir.string(), homeDir.string());

    auto melts = resolveMelts(m, repos, cache);
    ASSERT_FALSE((bool)melts);
    EXPECT_NE(errorText(melts.takeError()).find("not found"),
              std::string::npos);
}

// ─── applyMeltLookups ─────────────────────────────────────────────

TEST(MeltResolverTests, applyMeltLookupsSubstitutesStarConstraint) {
    MeltResolution melts;
    melts.depConstraints["acme.lib"] = "1.2.5";
    melts.depProvidedBy["acme.lib"]  = "platform.melt@1.0.0";

    std::vector<DependencySpec> deps;
    { DependencySpec d; d.name = "acme.lib"; d.versionConstraint = "*";
      deps.push_back(d); }

    std::map<std::string, std::string> providedBy;
    std::vector<std::string> warnings;
    auto e = applyMeltLookups(deps, melts, providedBy, warnings);
    ASSERT_FALSE((bool)e);
    EXPECT_EQ(deps[0].versionConstraint, "1.2.5");
    EXPECT_EQ(providedBy["acme.lib"], "platform.melt@1.0.0");
    EXPECT_TRUE(warnings.empty());
}

TEST(MeltResolverTests, applyMeltLookupsErrorsWhenNoMeltProvides) {
    MeltResolution melts;  // empty

    std::vector<DependencySpec> deps;
    { DependencySpec d; d.name = "ghost.lib"; d.versionConstraint = "*";
      deps.push_back(d); }

    std::map<std::string, std::string> providedBy;
    std::vector<std::string> warnings;
    auto e = applyMeltLookups(deps, melts, providedBy, warnings);
    ASSERT_TRUE((bool)e);
    std::string msg;
    llvm::raw_string_ostream os(msg);
    os << e;
    consumeError(std::move(e));
    EXPECT_NE(msg.find("ghost.lib"), std::string::npos);
    EXPECT_NE(msg.find("no imported melt"), std::string::npos);
}

TEST(MeltResolverTests, applyMeltLookupsLeavesExplicitConstraintsAlone) {
    MeltResolution melts;
    melts.depConstraints["acme.lib"] = "1.2.5";

    std::vector<DependencySpec> deps;
    // Explicit version — overrides melt-provided constraint.
    { DependencySpec d; d.name = "acme.lib"; d.versionConstraint = "1.4.0";
      deps.push_back(d); }

    std::map<std::string, std::string> providedBy;
    std::vector<std::string> warnings;
    auto e = applyMeltLookups(deps, melts, providedBy, warnings);
    ASSERT_FALSE((bool)e);
    EXPECT_EQ(deps[0].versionConstraint, "1.4.0");
    EXPECT_TRUE(providedBy.empty());
    // Divergence is captured as a warning even though the consumer's
    // explicit version takes precedence.
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("acme.lib"), std::string::npos);
    EXPECT_NE(warnings[0].find("1.4.0"), std::string::npos);
    EXPECT_NE(warnings[0].find("1.2.5"), std::string::npos);
}

TEST(MeltResolverTests, applyMeltLookupsExplicitMatchingMeltEmitsNoWarning) {
    // When the consumer's explicit version equals what the melt
    // curates, there's no divergence to warn about.
    MeltResolution melts;
    melts.depConstraints["acme.lib"] = "1.2.5";
    melts.depProvidedBy["acme.lib"]  = "platform.melt@1.0.0";

    std::vector<DependencySpec> deps;
    { DependencySpec d; d.name = "acme.lib"; d.versionConstraint = "1.2.5";
      deps.push_back(d); }

    std::map<std::string, std::string> providedBy;
    std::vector<std::string> warnings;
    auto e = applyMeltLookups(deps, melts, providedBy, warnings);
    ASSERT_FALSE((bool)e);
    EXPECT_TRUE(warnings.empty());
}

// ─── end-to-end: resolveProjectDependencies wires the full pipe ───

TEST(MeltResolverTests, projectResolutionAppliesStarFromMelt) {
    auto root = makeTempDir("repo");
    stagePkg(root, "acme.lib", "1.2.5", {});
    stageMelt(root, "platform.melt", "1.0.0",
              "\"dependencies\":{\"acme.lib\":\"1.2.5\"}");

    std::ostringstream src;
    src << R"({
        "details": { "name": "consumer", "version": "0.1.0" },
        "settings": {
            "repositories": [
                { "name": "local", "type": "filesystem",
                  "path": ")" << root.generic_string() << R"(" }
            ],
            "melts": [ "platform.melt@1.0.0" ],
            "dependencies": {
                "acme.lib": "*"
            }
        }
    })";
    auto m = mustLoad(src.str());

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");

    auto resolved = resolveProjectDependencies(
        m, projectDir.string(), homeDir.string());
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 1u);
    EXPECT_EQ((*resolved)[0].name, "acme.lib");
    EXPECT_EQ((*resolved)[0].version, "1.2.5");
}

TEST(MeltResolverTests, projectResolutionStarWithNoMeltFails) {
    auto root = makeTempDir("repo");
    stagePkg(root, "acme.lib", "1.2.5", {});

    std::ostringstream src;
    src << R"({
        "details": { "name": "consumer", "version": "0.1.0" },
        "settings": {
            "repositories": [
                { "name": "local", "type": "filesystem",
                  "path": ")" << root.generic_string() << R"(" }
            ],
            "dependencies": {
                "acme.lib": "*"
            }
        }
    })";
    auto m = mustLoad(src.str());

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");

    auto resolved = resolveProjectDependencies(
        m, projectDir.string(), homeDir.string());
    ASSERT_FALSE((bool)resolved);
    EXPECT_NE(errorText(resolved.takeError()).find("no imported melt"),
              std::string::npos);
}

TEST(MeltResolverTests, projectResolutionExplicitOverridesMeltConstraint) {
    auto root = makeTempDir("repo");
    stagePkg(root, "acme.lib", "1.2.5", {});
    stagePkg(root, "acme.lib", "1.4.0", {});
    stageMelt(root, "platform.melt", "1.0.0",
              "\"dependencies\":{\"acme.lib\":\"1.2.5\"}");

    std::ostringstream src;
    src << R"({
        "details": { "name": "consumer", "version": "0.1.0" },
        "settings": {
            "repositories": [
                { "name": "local", "type": "filesystem",
                  "path": ")" << root.generic_string() << R"(" }
            ],
            "melts": [ "platform.melt@1.0.0" ],
            "dependencies": {
                "acme.lib": "1.4.0"
            }
        }
    })";
    auto m = mustLoad(src.str());

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");

    auto resolved = resolveProjectDependencies(
        m, projectDir.string(), homeDir.string());
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 1u);
    EXPECT_EQ((*resolved)[0].version, "1.4.0");
}

TEST(MeltResolverTests, meltRepositoriesAppendedToResolutionList) {
    // Consumer declares ONLY a melt; the melt declares the repo
    // that hosts both itself and the dep. We have to bootstrap with
    // the local repo (so the melt can be fetched), and the melt's
    // contributed repo is what subsequently serves the dep.
    auto consumerRepo = makeTempDir("crepo");
    auto meltRepo     = makeTempDir("mrepo");

    stagePkg(meltRepo, "from.melt.repo", "1.0.0", {});
    std::ostringstream meltBody;
    meltBody << "\"dependencies\":{\"from.melt.repo\":\"1.0.0\"},"
             << "\"repositories\":[{\"name\":\"melt-repo\","
             << "\"type\":\"filesystem\","
             << "\"path\":\"" << meltRepo.generic_string() << "\","
             << "\"priority\":100}]";
    stageMelt(consumerRepo, "platform.melt", "1.0.0", meltBody.str());

    std::ostringstream src;
    src << R"({
        "details": { "name": "consumer", "version": "0.1.0" },
        "settings": {
            "repositories": [
                { "name": "local", "type": "filesystem",
                  "path": ")" << consumerRepo.generic_string() << R"(" }
            ],
            "melts": [ "platform.melt@1.0.0" ],
            "dependencies": {
                "from.melt.repo": "*"
            }
        }
    })";
    auto m = mustLoad(src.str());

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    auto resolved = resolveProjectDependencies(
        m, projectDir.string(), homeDir.string());
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 1u);
    EXPECT_EQ((*resolved)[0].name, "from.melt.repo");
    EXPECT_EQ((*resolved)[0].resolvedFromRepo, "melt-repo");
}
