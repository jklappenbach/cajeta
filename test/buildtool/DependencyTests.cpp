// Regression tests for the Phase 6a dependency layer:
// Dependency / RepositorySpec parsing, FilesystemRepository,
// ArtifactCache, and the direct-dependency resolver.

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
using cajeta::buildtool::compareVersions;
using cajeta::buildtool::DependencySpec;
using cajeta::buildtool::FilesystemRepository;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::parseDependencies;
using cajeta::buildtool::parseRepositories;
using cajeta::buildtool::Repository;
using cajeta::buildtool::RepositoryPtr;
using cajeta::buildtool::RepositorySpec;
using cajeta::buildtool::resolveDirect;
using cajeta::buildtool::ResolvedDependency;
using cajeta::buildtool::versionSatisfies;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    cajeta::buildtool::Manifest mustLoad(const std::string& src) {
        auto m = loadManifestString(src);
        if (!m) {
            ADD_FAILURE() << errorText(m.takeError());
            return {};
        }
        return std::move(*m);
    }

    std::filesystem::path makeTempDir(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-dep-test-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    // Lay out a filesystem-repo with packages + versions, writing
    // a `.cja` stub for each. Returns the repo root.
    struct PkgVer { std::string pkg; std::string version; std::string content; };
    std::filesystem::path makeFsRepo(
        const std::vector<PkgVer>& contents) {
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

} // namespace

// ─── version-constraint logic ─────────────────────────────────────────

TEST(DependencyTests, versionSatisfiesWildcards) {
    EXPECT_TRUE(versionSatisfies("1.2.3",   "*"));
    EXPECT_TRUE(versionSatisfies("0.0.1",   "*"));
    EXPECT_TRUE(versionSatisfies("1.2.7",   "1.2.*"));
    EXPECT_TRUE(versionSatisfies("1.2.0",   "1.2.*"));
    EXPECT_TRUE(versionSatisfies("1.99.99", "1.*"));
    EXPECT_FALSE(versionSatisfies("2.0.0",  "1.*"));
    EXPECT_FALSE(versionSatisfies("1.3.0",  "1.2.*"));
}

TEST(DependencyTests, versionSatisfiesExact) {
    EXPECT_TRUE(versionSatisfies("1.2.3", "1.2.3"));
    EXPECT_FALSE(versionSatisfies("1.2.4", "1.2.3"));
    EXPECT_FALSE(versionSatisfies("1.2.3-rc1", "1.2.3"));  // core comparison
}

TEST(DependencyTests, compareVersionsOrders) {
    EXPECT_LT(compareVersions("1.0.0", "1.0.1"), 0);
    EXPECT_LT(compareVersions("1.0.9", "1.0.10"), 0);  // numeric, not lexical
    EXPECT_LT(compareVersions("1.0.0", "1.1.0"), 0);
    EXPECT_LT(compareVersions("0.9.9", "1.0.0"), 0);
    EXPECT_EQ(compareVersions("1.2.3", "1.2.3"), 0);
    EXPECT_GT(compareVersions("2.0.0", "1.99.99"), 0);
}

// ─── settings.repositories parsing ────────────────────────────────────

TEST(DependencyTests, parsesRepositoriesByPriority) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "settings": {
            "repositories": [
                { "name": "central",  "url": "https://repo.cajeta.org",
                  "priority": 0 },
                { "name": "local-dev","type": "filesystem",
                  "path": "/tmp/local",  "priority": 200 },
                { "name": "company",  "url": "https://nexus.internal",
                  "priority": 100 }
            ]
        }
    })");
    auto repos = parseRepositories(m);
    ASSERT_TRUE((bool)repos);
    ASSERT_EQ(repos->size(), 3u);
    EXPECT_EQ((*repos)[0].name, "local-dev");   // priority 200 first
    EXPECT_EQ((*repos)[1].name, "company");     // priority 100
    EXPECT_EQ((*repos)[2].name, "central");     // priority 0 last
    EXPECT_EQ((*repos)[0].type, "filesystem");
    EXPECT_EQ((*repos)[0].path, "/tmp/local");
}

TEST(DependencyTests, parsesRepositoriesInfersTypeFromFields) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "settings": {
            "repositories": [
                { "name": "p", "path": "/some/path" },
                { "name": "u", "url":  "https://example.com" }
            ]
        }
    })");
    auto repos = parseRepositories(m);
    ASSERT_TRUE((bool)repos);
    EXPECT_EQ((*repos)[0].type, "filesystem");
    EXPECT_EQ((*repos)[1].type, "http");
}

TEST(DependencyTests, parsesRepositoriesErrorsOnMissingPathOrUrl) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "settings": {
            "repositories": [
                { "name": "broken", "type": "filesystem" }
            ]
        }
    })");
    auto repos = parseRepositories(m);
    ASSERT_FALSE((bool)repos);
    auto msg = errorText(repos.takeError());
    EXPECT_NE(msg.find("requires 'path'"), std::string::npos);
}

// ─── settings.dependencies parsing ────────────────────────────────────

TEST(DependencyTests, parsesDependenciesShortForm) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "settings": {
            "dependencies": {
                "cajeta.io.net.http": "1.2.*",
                "cajeta.lang":        "0.5.0"
            }
        }
    })");
    auto deps = parseDependencies(m);
    ASSERT_TRUE((bool)deps);
    ASSERT_EQ(deps->size(), 2u);
}

TEST(DependencyTests, parsesDependenciesObjectFormWithFromPin) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "settings": {
            "dependencies": {
                "acme.metrics": {
                    "version": "1.0.*",
                    "from":    "company-nexus"
                }
            }
        }
    })");
    auto deps = parseDependencies(m);
    ASSERT_TRUE((bool)deps);
    ASSERT_EQ(deps->size(), 1u);
    EXPECT_EQ((*deps)[0].name, "acme.metrics");
    EXPECT_EQ((*deps)[0].versionConstraint, "1.0.*");
    EXPECT_EQ((*deps)[0].fromRepo.value_or(""), "company-nexus");
}

// ─── FilesystemRepository ─────────────────────────────────────────────

TEST(DependencyTests, filesystemRepoListsAndFetches) {
    auto root = makeFsRepo({
        {"com.example.foo", "1.0.0", "foo-1.0.0-content"},
        {"com.example.foo", "1.0.1", "foo-1.0.1-content"},
        {"com.example.foo", "1.2.0", "foo-1.2.0-content"},
        {"com.example.bar", "0.1.0", "bar-content"},
    });
    FilesystemRepository repo("test", root.string());

    auto versions = repo.listVersions("com.example.foo");
    ASSERT_TRUE((bool)versions);
    EXPECT_EQ(versions->size(), 3u);

    auto fetched = repo.fetch("com.example.foo", "1.0.1");
    ASSERT_TRUE((bool)fetched);
    std::ifstream in(*fetched, std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    EXPECT_EQ(ss.str(), "foo-1.0.1-content");
    std::filesystem::remove_all(root);
}

TEST(DependencyTests, filesystemRepoReturnsEmptyForUnknownPackage) {
    auto root = makeFsRepo({});
    FilesystemRepository repo("test", root.string());
    auto versions = repo.listVersions("unknown.pkg");
    ASSERT_TRUE((bool)versions);
    EXPECT_TRUE(versions->empty());
    std::filesystem::remove_all(root);
}

TEST(DependencyTests, filesystemRepoErrorsOnMissingArtifact) {
    auto root = makeFsRepo({{"com.example.foo", "1.0.0", "x"}});
    FilesystemRepository repo("test", root.string());
    auto r = repo.fetch("com.example.foo", "9.9.9");
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("artifact not found"), std::string::npos);
    std::filesystem::remove_all(root);
}

// ─── ArtifactCache ────────────────────────────────────────────────────

TEST(DependencyTests, artifactCacheRoundTrips) {
    auto projectDir = makeTempDir("cache-proj");
    auto homeDir    = makeTempDir("cache-home");
    ArtifactCache cache(projectDir.string(), homeDir.string());

    // Insert an artifact, look it up via SHA, get it back.
    auto src = projectDir / "src.cja";
    std::ofstream o(src, std::ios::binary);
    o << "an artifact's content";
    o.close();

    auto cached = cache.insert(src.string());
    ASSERT_TRUE((bool)cached);
    auto sha = ArtifactCache::sha256OfFile(*cached);
    auto looked = cache.lookup(sha);
    ASSERT_TRUE(looked.has_value());
    // The cached path should live in the project cache.
    EXPECT_NE(looked->find(projectDir.string()), std::string::npos);

    // Workstation copy should also exist.
    auto wsPath = std::filesystem::path(cache.workstationCacheDir());
    EXPECT_TRUE(std::filesystem::exists(wsPath));

    std::filesystem::remove_all(projectDir);
    std::filesystem::remove_all(homeDir);
}

TEST(DependencyTests, artifactCacheReturnsNulloptOnMiss) {
    auto projectDir = makeTempDir("cache-miss-proj");
    auto homeDir    = makeTempDir("cache-miss-home");
    ArtifactCache cache(projectDir.string(), homeDir.string());
    EXPECT_FALSE(cache.lookup("sha256:0000000000000000000000000000000000000000000000000000000000000000").has_value());
    std::filesystem::remove_all(projectDir);
    std::filesystem::remove_all(homeDir);
}

// ─── direct resolver end-to-end ───────────────────────────────────────

TEST(DependencyTests, resolverPicksHighestSatisfyingFromRepoPriority) {
    auto repoRoot = makeFsRepo({
        {"com.example.foo", "1.0.0", "v1.0.0"},
        {"com.example.foo", "1.0.7", "v1.0.7"},
        {"com.example.foo", "1.2.0", "v1.2.0"},
    });
    auto projectDir = makeTempDir("resolver-proj");
    auto homeDir    = makeTempDir("resolver-home");

    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", repoRoot.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec d; d.name = "com.example.foo"; d.versionConstraint = "1.0.*";
    deps.push_back(d);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveDirect(deps, repos, cache);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 1u);
    EXPECT_EQ((*resolved)[0].name, "com.example.foo");
    EXPECT_EQ((*resolved)[0].version, "1.0.7");  // highest matching 1.0.*
    EXPECT_FALSE((*resolved)[0].artifactPath.empty());
    EXPECT_FALSE((*resolved)[0].sha256.empty());

    std::filesystem::remove_all(repoRoot);
    std::filesystem::remove_all(projectDir);
    std::filesystem::remove_all(homeDir);
}

TEST(DependencyTests, resolverErrorsWhenNoVersionSatisfies) {
    auto repoRoot = makeFsRepo({{"a.b", "1.0.0", "x"}});
    auto projectDir = makeTempDir("res-fail-proj");
    auto homeDir    = makeTempDir("res-fail-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("only", repoRoot.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec d; d.name = "a.b"; d.versionConstraint = "2.*";
    deps.push_back(d);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto r = resolveDirect(deps, repos, cache);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("not satisfied"), std::string::npos);

    std::filesystem::remove_all(repoRoot);
    std::filesystem::remove_all(projectDir);
    std::filesystem::remove_all(homeDir);
}

TEST(DependencyTests, resolverHonorsFromRepoPin) {
    auto repoA = makeFsRepo({{"acme.lib", "1.0.0", "from-A"}});
    auto repoB = makeFsRepo({{"acme.lib", "1.0.0", "from-B"}});
    auto projectDir = makeTempDir("res-pin-proj");
    auto homeDir    = makeTempDir("res-pin-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("A", repoA.string()),
        std::make_shared<FilesystemRepository>("B", repoB.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec d;
    d.name = "acme.lib";
    d.versionConstraint = "1.0.0";
    d.fromRepo = "B";  // pin to B even though A is listed first
    deps.push_back(d);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveDirect(deps, repos, cache);
    ASSERT_TRUE((bool)resolved);
    EXPECT_EQ((*resolved)[0].resolvedFromRepo, "B");

    std::filesystem::remove_all(repoA);
    std::filesystem::remove_all(repoB);
    std::filesystem::remove_all(projectDir);
    std::filesystem::remove_all(homeDir);
}

TEST(DependencyTests, resolverFallsThroughRepoPriorityWhenFirstLacksPackage) {
    auto repoLowPrio = makeFsRepo({{"com.example.bar", "0.1.0", "lo"}});
    auto repoHighPrio = makeFsRepo({{"com.example.bar", "0.2.0", "hi"}});
    auto projectDir = makeTempDir("res-fall-proj");
    auto homeDir    = makeTempDir("res-fall-home");
    // High-priority repo has the higher version; resolver should
    // pick it.
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("hi", repoHighPrio.string()),
        std::make_shared<FilesystemRepository>("lo", repoLowPrio.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec d; d.name = "com.example.bar"; d.versionConstraint = "*";
    deps.push_back(d);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveDirect(deps, repos, cache);
    ASSERT_TRUE((bool)resolved);
    EXPECT_EQ((*resolved)[0].version, "0.2.0");
    EXPECT_EQ((*resolved)[0].resolvedFromRepo, "hi");

    std::filesystem::remove_all(repoHighPrio);
    std::filesystem::remove_all(repoLowPrio);
    std::filesystem::remove_all(projectDir);
    std::filesystem::remove_all(homeDir);
}
