// Regression tests for the Phase 6c path-replacement override.
//
// `settings.overrides`: `{ "name": { "path": "..." } }` — replaces
// a transitive dep with a local directory tree. The resolver reads
// the directory's cajeta.json for (name, version) + transitive deps
// and serves a pre-built `.cja` from `<path>/build/archive/`.
//
// These tests cover:
//   - Happy path: transitive resolves to the override's local version.
//   - Direct-vs-override precedence: a path-overridden name that's
//     also a direct root dep ignores the override.
//   - Missing cajeta.json / missing artifact errors are clear.
//   - Name mismatch in the override's cajeta.json is caught.
//   - Manifest end-to-end through `resolveProjectDependencies`.

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/Manifest.h"
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

using cajeta::buildtool::ArtifactCache;
using cajeta::buildtool::DependencySpec;
using cajeta::buildtool::FilesystemRepository;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::OverrideSpec;
using cajeta::buildtool::RepositoryPtr;
using cajeta::buildtool::resolveMvs;
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
                 ("cajeta-pathov-test-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    struct PkgVer { std::string pkg; std::string version; };
    std::filesystem::path makeFsRepo(const std::vector<PkgVer>& pkgs) {
        auto root = makeTempDir("fsrepo");
        for (const auto& pv : pkgs) {
            auto dir = root / pv.pkg / pv.version;
            std::filesystem::create_directories(dir);
            auto file = dir / (pv.pkg + "-" + pv.version + ".cja");
            std::ofstream o(file, std::ios::binary);
            o << "stub";
        }
        return root;
    }

    void writeSidecar(
        const std::filesystem::path& root,
        const std::string& pkg, const std::string& version,
        const std::vector<std::pair<std::string, std::string>>& deps) {
        std::ostringstream js;
        js << "{\"details\":{\"name\":\"" << pkg
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
        std::ofstream o(root / pkg / version / "cajeta.json",
                        std::ios::binary);
        o << js.str();
    }

    // Build a directory laid out as a path-override target: a
    // cajeta.json at the root + a pre-built artifact under
    // build/archive/. Returns the directory path.
    std::filesystem::path makePathOverrideDir(
        const std::string& tag,
        const std::string& pkg, const std::string& version,
        const std::vector<std::pair<std::string, std::string>>& deps = {}) {
        auto dir = makeTempDir("ovdir-" + tag);

        std::ostringstream js;
        js << "{\"details\":{\"name\":\"" << pkg
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
        {
            std::ofstream o(dir / "cajeta.json", std::ios::binary);
            o << js.str();
        }
        auto art = dir / "build" / "archive";
        std::filesystem::create_directories(art);
        std::ofstream o(art / (pkg + "-" + version + ".cja"),
                        std::ios::binary);
        o << "stub";
        return dir;
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

// ─── happy path: transitive replaced by local ─────────────────────

TEST(PathOverrideTests, transitiveResolvesFromLocalPath) {
    // app -> direct -> deep   (direct@1.0.0 declares deep "1.0.0")
    // override deep with a local path at a different version (1.5.0)
    auto repoRoot = makeFsRepo({
        {"a.direct", "1.0.0"},
        {"a.deep",   "1.0.0"},
    });
    writeSidecar(repoRoot, "a.direct", "1.0.0", {{"a.deep", "1.0.0"}});
    writeSidecar(repoRoot, "a.deep",   "1.0.0", {});

    auto overrideDir = makePathOverrideDir(
        "deep", "a.deep", "1.0.0", {});

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", repoRoot.string()),
    };

    std::vector<DependencySpec> deps;
    {
        DependencySpec d;
        d.name = "a.direct";
        d.versionConstraint = "1.0.0";
        deps.push_back(d);
    }

    std::vector<OverrideSpec> overrides;
    {
        OverrideSpec o;
        o.name = "a.deep";
        o.path = overrideDir.string();
        overrides.push_back(o);
    }

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());

    ASSERT_EQ(resolved->size(), 2u);
    std::string deepVer;
    for (const auto& r : *resolved) {
        if (r.name == "a.deep") deepVer = r.version;
    }
    EXPECT_EQ(deepVer, "1.0.0");
}

TEST(PathOverrideTests, overrideVersionCanDifferFromRepoVersion) {
    // Repo has a.deep@1.0.0; override path provides a.deep@1.5.0.
    // Resolver should pick the override's 1.5.0.
    auto repoRoot = makeFsRepo({
        {"a.direct", "1.0.0"},
        {"a.deep",   "1.0.0"},
    });
    writeSidecar(repoRoot, "a.direct", "1.0.0", {{"a.deep", "*"}});
    writeSidecar(repoRoot, "a.deep",   "1.0.0", {});

    auto overrideDir = makePathOverrideDir(
        "deepver", "a.deep", "1.5.0", {});

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", repoRoot.string()),
    };
    std::vector<DependencySpec> deps;
    {
        DependencySpec d;
        d.name = "a.direct"; d.versionConstraint = "1.0.0";
        deps.push_back(d);
    }
    std::vector<OverrideSpec> overrides;
    {
        OverrideSpec o;
        o.name = "a.deep"; o.path = overrideDir.string();
        overrides.push_back(o);
    }

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());

    std::string deepVer;
    for (const auto& r : *resolved) {
        if (r.name == "a.deep") deepVer = r.version;
    }
    EXPECT_EQ(deepVer, "1.5.0");
}

// ─── direct-vs-override precedence ────────────────────────────────

TEST(PathOverrideTests, directDepWinsOverPathOverride) {
    // a.deep is BOTH a direct dep AND has a path override.
    // Direct should win: resolver picks from the repo, not the path.
    auto repoRoot = makeFsRepo({{"a.deep", "1.0.0"}});
    writeSidecar(repoRoot, "a.deep", "1.0.0", {});

    auto overrideDir = makePathOverrideDir(
        "shadow", "a.deep", "9.9.9", {});

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", repoRoot.string()),
    };
    std::vector<DependencySpec> deps;
    {
        DependencySpec d;
        d.name = "a.deep"; d.versionConstraint = "1.0.0";
        deps.push_back(d);
    }
    std::vector<OverrideSpec> overrides;
    {
        OverrideSpec o;
        o.name = "a.deep"; o.path = overrideDir.string();
        overrides.push_back(o);
    }

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 1u);
    EXPECT_EQ((*resolved)[0].version, "1.0.0");
    EXPECT_NE((*resolved)[0].resolvedFromRepo, "<path-override>");
}

// ─── transitive deps from override propagate ──────────────────────

TEST(PathOverrideTests, overrideTransitivesPropagate) {
    // a.direct -> a.deep   (declared in repo: deep is leaf)
    // override a.deep with a local that declares its own a.extra dep
    auto repoRoot = makeFsRepo({
        {"a.direct", "1.0.0"},
        {"a.extra",  "0.3.0"},
    });
    writeSidecar(repoRoot, "a.direct", "1.0.0", {{"a.deep", "*"}});
    writeSidecar(repoRoot, "a.extra",  "0.3.0", {});

    auto overrideDir = makePathOverrideDir(
        "deepex", "a.deep", "2.0.0",
        {{"a.extra", "0.3.0"}});

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", repoRoot.string()),
    };
    std::vector<DependencySpec> deps;
    {
        DependencySpec d;
        d.name = "a.direct"; d.versionConstraint = "1.0.0";
        deps.push_back(d);
    }
    std::vector<OverrideSpec> overrides;
    {
        OverrideSpec o;
        o.name = "a.deep"; o.path = overrideDir.string();
        overrides.push_back(o);
    }

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());

    std::vector<std::string> names;
    for (const auto& r : *resolved) names.push_back(r.name);
    EXPECT_NE(std::find(names.begin(), names.end(), "a.extra"),
              names.end());
}

// ─── error paths ──────────────────────────────────────────────────

TEST(PathOverrideTests, missingDirectoryErrorsClearly) {
    auto repoRoot = makeFsRepo({
        {"a.direct", "1.0.0"},
        {"a.deep",   "1.0.0"},
    });
    writeSidecar(repoRoot, "a.direct", "1.0.0", {{"a.deep", "1.0.0"}});
    writeSidecar(repoRoot, "a.deep",   "1.0.0", {});

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", repoRoot.string()),
    };
    std::vector<DependencySpec> deps;
    {
        DependencySpec d;
        d.name = "a.direct"; d.versionConstraint = "1.0.0";
        deps.push_back(d);
    }
    std::vector<OverrideSpec> overrides;
    {
        OverrideSpec o;
        o.name = "a.deep";
        o.path = "/nonexistent/path/that/does/not/exist";
        overrides.push_back(o);
    }

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides);
    ASSERT_FALSE((bool)resolved);
    EXPECT_NE(errorText(resolved.takeError()).find(
                  "is not a directory"),
              std::string::npos);
}

TEST(PathOverrideTests, missingCajetaJsonErrorsClearly) {
    auto repoRoot = makeFsRepo({
        {"a.direct", "1.0.0"},
        {"a.deep",   "1.0.0"},
    });
    writeSidecar(repoRoot, "a.direct", "1.0.0", {{"a.deep", "1.0.0"}});
    writeSidecar(repoRoot, "a.deep",   "1.0.0", {});

    auto emptyDir = makeTempDir("empty");

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", repoRoot.string()),
    };
    std::vector<DependencySpec> deps;
    {
        DependencySpec d;
        d.name = "a.direct"; d.versionConstraint = "1.0.0";
        deps.push_back(d);
    }
    std::vector<OverrideSpec> overrides;
    {
        OverrideSpec o;
        o.name = "a.deep"; o.path = emptyDir.string();
        overrides.push_back(o);
    }

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides);
    ASSERT_FALSE((bool)resolved);
    EXPECT_NE(errorText(resolved.takeError()).find(
                  "no cajeta.json"),
              std::string::npos);
}

TEST(PathOverrideTests, nameMismatchErrorsClearly) {
    auto repoRoot = makeFsRepo({
        {"a.direct", "1.0.0"},
        {"a.deep",   "1.0.0"},
    });
    writeSidecar(repoRoot, "a.direct", "1.0.0", {{"a.deep", "1.0.0"}});
    writeSidecar(repoRoot, "a.deep",   "1.0.0", {});

    // Override directory declares a different name than the
    // overridden dep — should fail loudly rather than silently
    // pick up the wrong package.
    auto wrongDir = makePathOverrideDir(
        "wrong", "totally.different.name", "1.0.0", {});

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", repoRoot.string()),
    };
    std::vector<DependencySpec> deps;
    {
        DependencySpec d;
        d.name = "a.direct"; d.versionConstraint = "1.0.0";
        deps.push_back(d);
    }
    std::vector<OverrideSpec> overrides;
    {
        OverrideSpec o;
        o.name = "a.deep"; o.path = wrongDir.string();
        overrides.push_back(o);
    }

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides);
    ASSERT_FALSE((bool)resolved);
    auto msg = errorText(resolved.takeError());
    EXPECT_NE(msg.find("declares name"), std::string::npos);
    EXPECT_NE(msg.find("totally.different.name"), std::string::npos);
}

TEST(PathOverrideTests, missingArtifactErrorsClearly) {
    auto repoRoot = makeFsRepo({
        {"a.direct", "1.0.0"},
        {"a.deep",   "1.0.0"},
    });
    writeSidecar(repoRoot, "a.direct", "1.0.0", {{"a.deep", "1.0.0"}});
    writeSidecar(repoRoot, "a.deep",   "1.0.0", {});

    // Override directory has cajeta.json but NO .cja
    auto dir = makeTempDir("noartifact");
    {
        std::ofstream o(dir / "cajeta.json", std::ios::binary);
        o << R"({"details":{"name":"a.deep","version":"1.0.0"}})";
    }

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", repoRoot.string()),
    };
    std::vector<DependencySpec> deps;
    {
        DependencySpec d;
        d.name = "a.direct"; d.versionConstraint = "1.0.0";
        deps.push_back(d);
    }
    std::vector<OverrideSpec> overrides;
    {
        OverrideSpec o;
        o.name = "a.deep"; o.path = dir.string();
        overrides.push_back(o);
    }

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides);
    ASSERT_FALSE((bool)resolved);
    EXPECT_NE(errorText(resolved.takeError()).find(
                  "expected pre-built artifact"),
              std::string::npos);
}

// ─── major-downgrade audit still fires ────────────────────────────

TEST(PathOverrideTests, majorDowngradeAuditFiresOnPathOverride) {
    // direct -> deep >=2.0.0 ; path override pins deep to 1.5.0
    auto repoRoot = makeFsRepo({
        {"a.direct", "1.0.0"},
        {"a.deep",   "2.0.0"},
    });
    writeSidecar(repoRoot, "a.direct", "1.0.0",
                 {{"a.deep", ">=2.0.0"}});
    writeSidecar(repoRoot, "a.deep",   "2.0.0", {});

    auto overrideDir = makePathOverrideDir(
        "downgrade", "a.deep", "1.5.0", {});

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", repoRoot.string()),
    };
    std::vector<DependencySpec> deps;
    {
        DependencySpec d;
        d.name = "a.direct"; d.versionConstraint = "1.0.0";
        deps.push_back(d);
    }
    std::vector<OverrideSpec> overrides;
    {
        OverrideSpec o;
        o.name = "a.deep"; o.path = overrideDir.string();
        overrides.push_back(o);
    }

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides);
    ASSERT_FALSE((bool)resolved);
    EXPECT_NE(errorText(resolved.takeError()).find(
                  "allow-major-downgrade"),
              std::string::npos);
}

TEST(PathOverrideTests, allowMajorDowngradeBypassesAudit) {
    auto repoRoot = makeFsRepo({
        {"a.direct", "1.0.0"},
        {"a.deep",   "2.0.0"},
    });
    writeSidecar(repoRoot, "a.direct", "1.0.0",
                 {{"a.deep", ">=2.0.0"}});
    writeSidecar(repoRoot, "a.deep",   "2.0.0", {});

    auto overrideDir = makePathOverrideDir(
        "downgrade-ok", "a.deep", "1.5.0", {});

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", repoRoot.string()),
    };
    std::vector<DependencySpec> deps;
    {
        DependencySpec d;
        d.name = "a.direct"; d.versionConstraint = "1.0.0";
        deps.push_back(d);
    }
    std::vector<OverrideSpec> overrides;
    {
        OverrideSpec o;
        o.name = "a.deep"; o.path = overrideDir.string();
        o.allowMajorDowngrade = true;
        overrides.push_back(o);
    }

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());

    std::string deepVer;
    for (const auto& r : *resolved) {
        if (r.name == "a.deep") deepVer = r.version;
    }
    EXPECT_EQ(deepVer, "1.5.0");
}

// ─── end-to-end via resolveProjectDependencies ────────────────────

TEST(PathOverrideTests, projectResolutionWiresPathOverride) {
    auto repoRoot = makeFsRepo({
        {"e.direct", "1.0.0"},
        {"e.deep",   "1.0.0"},
    });
    writeSidecar(repoRoot, "e.direct", "1.0.0", {{"e.deep", "1.0.0"}});
    writeSidecar(repoRoot, "e.deep",   "1.0.0", {});
    auto overrideDir = makePathOverrideDir(
        "e2e", "e.deep", "1.0.0", {});

    std::ostringstream src;
    src << R"({
        "details": { "name": "demo.e2e", "version": "0.1.0" },
        "settings": {
            "repositories": [
                { "name": "local", "type": "filesystem",
                  "path": ")" << repoRoot.string() << R"(" }
            ],
            "dependencies": {
                "e.direct": "1.0.0"
            },
            "overrides": {
                "e.deep": { "path": ")" << overrideDir.string() << R"(" }
            }
        }
    })";
    auto m = mustLoad(src.str());
    auto projectDir = makeTempDir("proj-e2e");
    auto homeDir    = makeTempDir("home-e2e");

    auto resolved = resolveProjectDependencies(
        m, projectDir.string(), homeDir.string());
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 2u);
    bool foundDeepFromOverride = false;
    for (const auto& r : *resolved) {
        if (r.name == "e.deep" &&
            r.resolvedFromRepo == "<path-override>") {
            foundDeepFromOverride = true;
        }
    }
    EXPECT_TRUE(foundDeepFromOverride);
}
