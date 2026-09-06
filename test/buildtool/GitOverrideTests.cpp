// Regression tests for the Phase 6c git-replacement override.
//
// `settings.overrides`: `{ "name": { "git": "...", "rev": "..." } }`
// — replaces a transitive dep with a git checkout. The resolver
// clones into the stage dir, checks out the rev, reads the
// dep's cajeta.json + pre-built `.cja` from the checkout, and
// splices it into the resolution graph.
//
// These tests cover happy path, direct-vs-override precedence, the
// stage-dir guard, end-to-end manifest wiring, and the bad-ref
// failure path. The fixture creates a local git repo and points
// the override at `file://...` so no network is touched. Tests
// skip gracefully when `git` isn't on PATH.

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Repository.h"
#include "cajeta/buildtool/Resolver.h"
#include "cajeta/buildtool/repo/FilesystemRepository.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include "../PortableEnv.h"

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

    bool gitOnPath() {
        return std::system("git --version >" CAJETA_PORTABLE_DEVNULL " 2>&1") == 0;
    }

    std::filesystem::path makeTempDir(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-gitov-test-" + tag + "-" +
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
            std::ofstream o(dir / (pv.pkg + "-" + pv.version + ".cja"),
                            std::ios::binary);
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

    struct Upstream {
        std::filesystem::path dir;
        std::string url;
        std::string tag;
    };
    // Create a tiny upstream git repo at `tag` containing the
    // override's cajeta.json + pre-built `.cja`. Mirrors the
    // helper in GitRepositoryTests; kept local so each test
    // file's fixtures live alongside the assertions they support.
    Upstream makeUpstream(
        const std::string& pkg, const std::string& version,
        const std::vector<std::pair<std::string, std::string>>& deps = {}) {
        auto dir = makeTempDir("upstream");
        auto run = [&](const std::string& cmd) {
            std::string full = CAJETA_PORTABLE_CD + dir.string() + " && " +
                               cmd + " >" CAJETA_PORTABLE_DEVNULL " 2>&1";
            EXPECT_EQ(0, std::system(full.c_str())) << full;
        };
        run("git init -q -b main");
        run("git config user.email test@cajeta");
        run("git config user.name CajetaTest");

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
        { std::ofstream o(dir / "cajeta.json"); o << js.str(); }

        auto art = dir / "build" / "archive";
        std::filesystem::create_directories(art);
        { std::ofstream o(art / (pkg + "-" + version + ".cja"));
          o << "stub"; }

        run("git add -A");
        run("git commit -q -m initial");
        std::string tag = "v" + version;
        run("git tag " + tag);

        Upstream u;
        u.dir = dir;
        u.url = "file://" + dir.string();
        u.tag = tag;
        return u;
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

// ─── happy path ────────────────────────────────────────────────────

TEST(GitOverrideTests, transitiveResolvesFromGitClone) {
    if (!gitOnPath()) GTEST_SKIP() << "git not on PATH";

    auto repoRoot = makeFsRepo({
        {"g.direct", "1.0.0"},
        {"g.deep",   "1.0.0"},
    });
    writeSidecar(repoRoot, "g.direct", "1.0.0", {{"g.deep", "1.0.0"}});
    writeSidecar(repoRoot, "g.deep",   "1.0.0", {});

    auto up = makeUpstream("g.deep", "1.0.0");

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    auto stage      = makeTempDir("stage").string();
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", repoRoot.string()),
    };

    std::vector<DependencySpec> deps;
    {
        DependencySpec d;
        d.name = "g.direct"; d.versionConstraint = "1.0.0";
        deps.push_back(d);
    }
    std::vector<OverrideSpec> overrides;
    {
        OverrideSpec o;
        o.name = "g.deep"; o.git = up.url; o.rev = up.tag;
        overrides.push_back(o);
    }

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides, nullptr, stage);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());

    std::string deepFrom;
    for (const auto& r : *resolved) {
        if (r.name == "g.deep") deepFrom = r.resolvedFromRepo;
    }
    EXPECT_EQ(deepFrom, "<git-override>");
}

// ─── direct-vs-override precedence ────────────────────────────────

TEST(GitOverrideTests, directDepWinsOverGitOverride) {
    if (!gitOnPath()) GTEST_SKIP() << "git not on PATH";

    auto repoRoot = makeFsRepo({{"g.deep", "1.0.0"}});
    writeSidecar(repoRoot, "g.deep", "1.0.0", {});

    auto up = makeUpstream("g.deep", "9.9.9");

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    auto stage      = makeTempDir("stage").string();
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", repoRoot.string()),
    };
    std::vector<DependencySpec> deps;
    {
        DependencySpec d;
        d.name = "g.deep"; d.versionConstraint = "1.0.0";
        deps.push_back(d);
    }
    std::vector<OverrideSpec> overrides;
    {
        OverrideSpec o;
        o.name = "g.deep"; o.git = up.url; o.rev = up.tag;
        overrides.push_back(o);
    }

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides, nullptr, stage);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 1u);
    EXPECT_EQ((*resolved)[0].version, "1.0.0");
    EXPECT_NE((*resolved)[0].resolvedFromRepo, "<git-override>");
}

// ─── stage-dir guard ──────────────────────────────────────────────

TEST(GitOverrideTests, missingStageDirErrorsClearlyForTransitive) {
    if (!gitOnPath()) GTEST_SKIP() << "git not on PATH";

    auto repoRoot = makeFsRepo({
        {"g.direct", "1.0.0"},
        {"g.deep",   "1.0.0"},
    });
    writeSidecar(repoRoot, "g.direct", "1.0.0", {{"g.deep", "1.0.0"}});
    writeSidecar(repoRoot, "g.deep",   "1.0.0", {});
    auto up = makeUpstream("g.deep", "1.0.0");

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", repoRoot.string()),
    };
    std::vector<DependencySpec> deps;
    {
        DependencySpec d;
        d.name = "g.direct"; d.versionConstraint = "1.0.0";
        deps.push_back(d);
    }
    std::vector<OverrideSpec> overrides;
    {
        OverrideSpec o;
        o.name = "g.deep"; o.git = up.url; o.rev = up.tag;
        overrides.push_back(o);
    }

    ArtifactCache cache(projectDir.string(), homeDir.string());
    // No stage dir passed — must error rather than crash.
    auto resolved = resolveMvs(deps, repos, cache, overrides);
    ASSERT_FALSE((bool)resolved);
    EXPECT_NE(errorText(resolved.takeError()).find(
                  "stage directory"),
              std::string::npos);
}

// ─── bad ref ──────────────────────────────────────────────────────

TEST(GitOverrideTests, badRefSurfacesAsCheckoutFailure) {
    if (!gitOnPath()) GTEST_SKIP() << "git not on PATH";

    auto repoRoot = makeFsRepo({
        {"g.direct", "1.0.0"},
        {"g.deep",   "1.0.0"},
    });
    writeSidecar(repoRoot, "g.direct", "1.0.0", {{"g.deep", "1.0.0"}});
    writeSidecar(repoRoot, "g.deep",   "1.0.0", {});
    auto up = makeUpstream("g.deep", "1.0.0");

    auto projectDir = makeTempDir("proj");
    auto homeDir    = makeTempDir("home");
    auto stage      = makeTempDir("stage").string();
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", repoRoot.string()),
    };
    std::vector<DependencySpec> deps;
    {
        DependencySpec d;
        d.name = "g.direct"; d.versionConstraint = "1.0.0";
        deps.push_back(d);
    }
    std::vector<OverrideSpec> overrides;
    {
        OverrideSpec o;
        o.name = "g.deep"; o.git = up.url; o.rev = "does-not-exist";
        overrides.push_back(o);
    }

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides, nullptr, stage);
    ASSERT_FALSE((bool)resolved);
    EXPECT_NE(errorText(resolved.takeError()).find("checkout"),
              std::string::npos);
}

// ─── end-to-end via manifest ──────────────────────────────────────

TEST(GitOverrideTests, projectResolutionWiresGitOverride) {
    if (!gitOnPath()) GTEST_SKIP() << "git not on PATH";

    auto repoRoot = makeFsRepo({
        {"e.direct", "1.0.0"},
        {"e.deep",   "1.0.0"},
    });
    writeSidecar(repoRoot, "e.direct", "1.0.0", {{"e.deep", "1.0.0"}});
    writeSidecar(repoRoot, "e.deep",   "1.0.0", {});
    auto up = makeUpstream("e.deep", "1.0.0");

    std::ostringstream src;
    src << R"({
        "details": { "name": "demo.gitov", "version": "0.1.0" },
        "settings": {
            "repositories": [
                { "name": "local", "type": "filesystem",
                  "path": ")" << repoRoot.string() << R"(" }
            ],
            "dependencies": {
                "e.direct": "1.0.0"
            },
            "overrides": {
                "e.deep": {
                    "git": ")" << up.url << R"(",
                    "rev": ")" << up.tag << R"("
                }
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

    bool foundDeepFromGitOverride = false;
    for (const auto& r : *resolved) {
        if (r.name == "e.deep" &&
            r.resolvedFromRepo == "<git-override>") {
            foundDeepFromGitOverride = true;
        }
    }
    EXPECT_TRUE(foundDeepFromGitOverride);
}
