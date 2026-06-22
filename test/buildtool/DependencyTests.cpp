// Regression tests for the Phase 6a dependency layer:
// Dependency / RepositorySpec parsing, FilesystemRepository,
// ArtifactCache, and the direct-dependency resolver.

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Repository.h"
#include "cajeta/buildtool/Resolver.h"
#include "cajeta/buildtool/repo/FilesystemRepository.h"

#include "TempTeardown.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

using cajeta::buildtool::ArtifactCache;
using cajeta::buildtool::compareVersions;
using cajeta::buildtool::DependencySpec;
using cajeta::buildtool::FilesystemRepository;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::parseDependencies;
using cajeta::buildtool::OverrideSpec;
using cajeta::buildtool::parseOverrides;
using cajeta::buildtool::parseRepositories;
using cajeta::buildtool::Repository;
using cajeta::buildtool::RepositoryPtr;
using cajeta::buildtool::RepositorySpec;
using cajeta::buildtool::resolveDirect;
using cajeta::buildtool::ResolvedDependency;
using cajeta::buildtool::resolveMvs;
using cajeta::buildtool::resolveProjectDependencies;
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

    // Drop a sidecar cajeta.json next to a fs-repo artifact. The
    // manifest is the minimum loadManifestString will accept:
    // `details.name`, `details.version`, plus settings.dependencies.
    // Each entry in `deps` is "name@kebab" → constraint string.
    void writeSidecarManifest(
        const std::filesystem::path& repoRoot,
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
        auto path = repoRoot / pkg / version / "cajeta.json";
        std::ofstream o(path, std::ios::binary);
        o << js.str();
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

TEST(DependencyTests, versionSatisfiesRangeOperators) {
    EXPECT_TRUE(versionSatisfies("1.2.0", ">=1.2.0"));
    EXPECT_TRUE(versionSatisfies("1.2.1", ">=1.2.0"));
    EXPECT_FALSE(versionSatisfies("1.1.9", ">=1.2.0"));

    EXPECT_FALSE(versionSatisfies("2.0.0", "<2.0.0"));
    EXPECT_TRUE(versionSatisfies("1.99.99", "<2.0.0"));

    EXPECT_FALSE(versionSatisfies("1.2.0", ">1.2.0"));
    EXPECT_TRUE(versionSatisfies("1.2.1", ">1.2.0"));

    EXPECT_TRUE(versionSatisfies("1.2.0", "<=1.2.0"));
    EXPECT_TRUE(versionSatisfies("1.1.9", "<=1.2.0"));
    EXPECT_FALSE(versionSatisfies("1.2.1", "<=1.2.0"));

    // Explicit `=` form is equivalent to the bare-exact form.
    EXPECT_TRUE(versionSatisfies("1.2.3", "=1.2.3"));
    EXPECT_FALSE(versionSatisfies("1.2.4", "=1.2.3"));
}

TEST(DependencyTests, versionSatisfiesAndCombinations) {
    EXPECT_TRUE(versionSatisfies("1.5.0",  ">=1.2.0,<2.0.0"));
    EXPECT_TRUE(versionSatisfies("1.2.0",  ">=1.2.0,<2.0.0"));
    EXPECT_FALSE(versionSatisfies("2.0.0", ">=1.2.0,<2.0.0"));
    EXPECT_FALSE(versionSatisfies("1.1.0", ">=1.2.0,<2.0.0"));

    // Whitespace around commas and operators tolerated.
    EXPECT_TRUE(versionSatisfies("1.5.0", " >= 1.2.0 , < 2.0.0 "));

    // Wildcard composed with a lower bound.
    EXPECT_TRUE(versionSatisfies("1.2.7", ">=1.2.5,1.2.*"));
    EXPECT_FALSE(versionSatisfies("1.2.4", ">=1.2.5,1.2.*"));
    EXPECT_FALSE(versionSatisfies("1.3.0", ">=1.2.5,1.2.*"));
}

TEST(DependencyTests, versionSatisfiesNumericCompareInRanges) {
    // `1.0.10 >= 1.0.9` must be true — numeric, not lexical
    // (matches the compareVersions guarantee).
    EXPECT_TRUE(versionSatisfies("1.0.10", ">=1.0.9"));
    EXPECT_FALSE(versionSatisfies("1.0.9", ">1.0.9"));
}

TEST(DependencyTests, versionSatisfiesRejectsMalformed) {
    // Empty atom (trailing comma, double comma) — unsatisfiable.
    EXPECT_FALSE(versionSatisfies("1.2.3", ">=1.0,"));
    EXPECT_FALSE(versionSatisfies("1.2.3", ">=1.0,,<2.0"));
    // Operator with no version.
    EXPECT_FALSE(versionSatisfies("1.2.3", ">="));
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
    rmTree(root);
}

TEST(DependencyTests, filesystemRepoReturnsEmptyForUnknownPackage) {
    auto root = makeFsRepo({});
    FilesystemRepository repo("test", root.string());
    auto versions = repo.listVersions("unknown.pkg");
    ASSERT_TRUE((bool)versions);
    EXPECT_TRUE(versions->empty());
    rmTree(root);
}

TEST(DependencyTests, filesystemRepoErrorsOnMissingArtifact) {
    auto root = makeFsRepo({{"com.example.foo", "1.0.0", "x"}});
    FilesystemRepository repo("test", root.string());
    auto r = repo.fetch("com.example.foo", "9.9.9");
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("artifact not found"), std::string::npos);
    rmTree(root);
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

    // U3b: the workstation content-hash tier is retired — ~/.olla is the
    // single cross-project store now, so insert must NOT write
    // <home>/.cajeta/cache/artifacts/.
    auto wsArtifacts =
        homeDir / ".cajeta" / "cache" / "artifacts";
    EXPECT_FALSE(std::filesystem::exists(wsArtifacts))
        << "workstation content-hash tier should be retired (U3b)";

    rmTree(projectDir);
    rmTree(homeDir);
}

TEST(DependencyTests, artifactCacheReturnsNulloptOnMiss) {
    auto projectDir = makeTempDir("cache-miss-proj");
    auto homeDir    = makeTempDir("cache-miss-home");
    ArtifactCache cache(projectDir.string(), homeDir.string());
    EXPECT_FALSE(cache.lookup("sha256:0000000000000000000000000000000000000000000000000000000000000000").has_value());
    rmTree(projectDir);
    rmTree(homeDir);
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

    rmTree(repoRoot);
    rmTree(projectDir);
    rmTree(homeDir);
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

    rmTree(repoRoot);
    rmTree(projectDir);
    rmTree(homeDir);
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

    rmTree(repoA);
    rmTree(repoB);
    rmTree(projectDir);
    rmTree(homeDir);
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

    rmTree(repoHighPrio);
    rmTree(repoLowPrio);
    rmTree(projectDir);
    rmTree(homeDir);
}

// ─── filesystem sidecar manifest (Phase 6b) ───────────────────────────

TEST(DependencyTests, filesystemRepoReadsSidecarManifestJson) {
    auto root = makeFsRepo({{"com.example.foo", "1.0.0", "x"}});
    writeSidecarManifest(root, "com.example.foo", "1.0.0",
                         {{"com.example.bar", "1.*"}});
    FilesystemRepository repo("test", root.string());

    auto js = repo.fetchManifestJson("com.example.foo", "1.0.0");
    ASSERT_TRUE((bool)js) << errorText(js.takeError());
    ASSERT_TRUE(js->has_value());
    EXPECT_NE((*js)->find("com.example.foo"), std::string::npos);
    EXPECT_NE((*js)->find("com.example.bar"), std::string::npos);
    rmTree(root);
}

TEST(DependencyTests, filesystemRepoNoSidecarReturnsNullopt) {
    auto root = makeFsRepo({{"com.example.foo", "1.0.0", "x"}});
    // No sidecar written.
    FilesystemRepository repo("test", root.string());
    auto js = repo.fetchManifestJson("com.example.foo", "1.0.0");
    ASSERT_TRUE((bool)js) << errorText(js.takeError());
    EXPECT_FALSE(js->has_value());
    rmTree(root);
}

// ─── transitive resolver ──────────────────────────────────────────────

TEST(DependencyTests, mvsResolverExpandsOneLevel) {
    auto root = makeFsRepo({
        {"com.example.foo", "1.0.0", "foo-1.0.0"},
        {"com.example.bar", "1.5.0", "bar-1.5.0"},
    });
    // foo depends on bar.
    writeSidecarManifest(root, "com.example.foo", "1.0.0",
                         {{"com.example.bar", "1.*"}});
    writeSidecarManifest(root, "com.example.bar", "1.5.0", {});

    auto projectDir = makeTempDir("trans-1lvl-proj");
    auto homeDir    = makeTempDir("trans-1lvl-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec d; d.name = "com.example.foo"; d.versionConstraint = "1.0.0";
    deps.push_back(d);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 2u);
    EXPECT_EQ((*resolved)[0].name, "com.example.foo");
    EXPECT_EQ((*resolved)[0].version, "1.0.0");
    EXPECT_EQ((*resolved)[1].name, "com.example.bar");
    EXPECT_EQ((*resolved)[1].version, "1.5.0");

    rmTree(root);
    rmTree(projectDir);
    rmTree(homeDir);
}

TEST(DependencyTests, mvsResolverThreeDeepGraph) {
    // foo -> bar -> baz, three-deep chain (matches the Phase 6
    // acceptance criterion).
    auto root = makeFsRepo({
        {"a.foo", "1.0.0", "foo"},
        {"a.bar", "2.0.0", "bar"},
        {"a.baz", "3.0.0", "baz"},
    });
    writeSidecarManifest(root, "a.foo", "1.0.0", {{"a.bar", "2.*"}});
    writeSidecarManifest(root, "a.bar", "2.0.0", {{"a.baz", "3.*"}});
    writeSidecarManifest(root, "a.baz", "3.0.0", {});

    auto projectDir = makeTempDir("trans-3deep-proj");
    auto homeDir    = makeTempDir("trans-3deep-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec d; d.name = "a.foo"; d.versionConstraint = "1.0.0";
    deps.push_back(d);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 3u);
    EXPECT_EQ((*resolved)[0].name, "a.foo");
    EXPECT_EQ((*resolved)[1].name, "a.bar");
    EXPECT_EQ((*resolved)[2].name, "a.baz");

    rmTree(root);
    rmTree(projectDir);
    rmTree(homeDir);
}

TEST(DependencyTests, mvsResolverDeduplicatesDiamond) {
    // Diamond:  root -> {a, b}, both a and b -> shared.
    // shared should appear exactly once in the resolved set.
    auto root = makeFsRepo({
        {"d.a",      "1.0.0", "a"},
        {"d.b",      "1.0.0", "b"},
        {"d.shared", "5.0.0", "shared"},
    });
    writeSidecarManifest(root, "d.a", "1.0.0", {{"d.shared", "5.*"}});
    writeSidecarManifest(root, "d.b", "1.0.0", {{"d.shared", "5.*"}});
    writeSidecarManifest(root, "d.shared", "5.0.0", {});

    auto projectDir = makeTempDir("trans-diamond-proj");
    auto homeDir    = makeTempDir("trans-diamond-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec a; a.name = "d.a"; a.versionConstraint = "1.0.0";
    DependencySpec b; b.name = "d.b"; b.versionConstraint = "1.0.0";
    deps.push_back(a); deps.push_back(b);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    EXPECT_EQ(resolved->size(), 3u);
    int sharedCount = 0;
    for (const auto& r : *resolved) {
        if (r.name == "d.shared") ++sharedCount;
    }
    EXPECT_EQ(sharedCount, 1);

    rmTree(root);
    rmTree(projectDir);
    rmTree(homeDir);
}

TEST(DependencyTests, mvsResolverBreaksCycles) {
    // A -> B, B -> A. Neither should loop forever; both should
    // appear in the resolved set exactly once.
    auto root = makeFsRepo({
        {"c.a", "1.0.0", "a"},
        {"c.b", "1.0.0", "b"},
    });
    writeSidecarManifest(root, "c.a", "1.0.0", {{"c.b", "1.0.0"}});
    writeSidecarManifest(root, "c.b", "1.0.0", {{"c.a", "1.0.0"}});

    auto projectDir = makeTempDir("trans-cycle-proj");
    auto homeDir    = makeTempDir("trans-cycle-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec d; d.name = "c.a"; d.versionConstraint = "1.0.0";
    deps.push_back(d);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    EXPECT_EQ(resolved->size(), 2u);

    rmTree(root);
    rmTree(projectDir);
    rmTree(homeDir);
}

TEST(DependencyTests, mvsResolverTreatsLeafAsTerminalWhenSidecarMissing) {
    // Backwards-compat with pre-sidecar artifacts: foo's sidecar
    // declares it depends on bar, but bar has no sidecar so the
    // walker terminates there without erroring.
    auto root = makeFsRepo({
        {"l.foo", "1.0.0", "foo"},
        {"l.bar", "1.0.0", "bar"},
    });
    writeSidecarManifest(root, "l.foo", "1.0.0", {{"l.bar", "1.0.0"}});
    // No sidecar for bar.

    auto projectDir = makeTempDir("trans-leaf-proj");
    auto homeDir    = makeTempDir("trans-leaf-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec d; d.name = "l.foo"; d.versionConstraint = "1.0.0";
    deps.push_back(d);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 2u);
    EXPECT_EQ((*resolved)[1].name, "l.bar");

    rmTree(root);
    rmTree(projectDir);
    rmTree(homeDir);
}

TEST(DependencyTests, mvsResolverErrorsOnUnsatisfiableChildConstraint) {
    auto root = makeFsRepo({
        {"u.foo", "1.0.0", "foo"},
        {"u.bar", "1.0.0", "bar"},
    });
    // foo asks for bar 99.x which doesn't exist.
    writeSidecarManifest(root, "u.foo", "1.0.0", {{"u.bar", "99.*"}});
    writeSidecarManifest(root, "u.bar", "1.0.0", {});

    auto projectDir = makeTempDir("trans-unsat-proj");
    auto homeDir    = makeTempDir("trans-unsat-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec d; d.name = "u.foo"; d.versionConstraint = "1.0.0";
    deps.push_back(d);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache);
    ASSERT_FALSE((bool)resolved);
    auto msg = errorText(resolved.takeError());
    EXPECT_NE(msg.find("u.bar"), std::string::npos);
    EXPECT_NE(msg.find("satisfies constraints"), std::string::npos)
        << "expected MVS unsatisfiable message, got: " << msg;

    rmTree(root);
    rmTree(projectDir);
    rmTree(homeDir);
}

// ─── MVS-specific behavior (Phase 6b acceptance criteria) ─────────────

TEST(DependencyTests, mvsPicksLowestSatisfyingFromRange) {
    // Repo has versions 1.0.0, 1.2.0, 1.5.0, 1.9.0; constraint `>=1.2.0,<2.0.0`
    // → MVS picks 1.2.0 (lowest satisfying). The Phase 6a-style
    // "highest satisfying" resolver would have picked 1.9.0.
    auto root = makeFsRepo({
        {"m.foo", "1.0.0", "v1.0.0"},
        {"m.foo", "1.2.0", "v1.2.0"},
        {"m.foo", "1.5.0", "v1.5.0"},
        {"m.foo", "1.9.0", "v1.9.0"},
    });
    writeSidecarManifest(root, "m.foo", "1.2.0", {});
    auto projectDir = makeTempDir("mvs-low-proj");
    auto homeDir    = makeTempDir("mvs-low-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec d; d.name = "m.foo"; d.versionConstraint = ">=1.2.0,<2.0.0";
    deps.push_back(d);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 1u);
    EXPECT_EQ((*resolved)[0].version, "1.2.0");

    rmTree(root);
    rmTree(projectDir);
    rmTree(homeDir);
}

TEST(DependencyTests, mvsRepicksOnTighterChildConstraint) {
    // root -> foo, baz
    // foo  -> shared >=1.0.0
    // baz  -> shared >=1.5.0
    // Expected MVS pick for `shared`: 1.5.0 (lowest satisfying max).
    // Repo has shared at 1.0.0, 1.2.0, 1.5.0, 1.9.0. Walking BFS in
    // declaration order, foo's child constraint arrives first and
    // picks 1.0.0; when baz's child constraint arrives later it
    // forces a re-pick to 1.5.0.
    auto root = makeFsRepo({
        {"r.foo",    "1.0.0", "foo"},
        {"r.baz",    "1.0.0", "baz"},
        {"r.shared", "1.0.0", "s1.0.0"},
        {"r.shared", "1.2.0", "s1.2.0"},
        {"r.shared", "1.5.0", "s1.5.0"},
        {"r.shared", "1.9.0", "s1.9.0"},
    });
    writeSidecarManifest(root, "r.foo", "1.0.0", {{"r.shared", ">=1.0.0"}});
    writeSidecarManifest(root, "r.baz", "1.0.0", {{"r.shared", ">=1.5.0"}});
    writeSidecarManifest(root, "r.shared", "1.0.0", {});
    writeSidecarManifest(root, "r.shared", "1.2.0", {});
    writeSidecarManifest(root, "r.shared", "1.5.0", {});
    writeSidecarManifest(root, "r.shared", "1.9.0", {});

    auto projectDir = makeTempDir("mvs-repick-proj");
    auto homeDir    = makeTempDir("mvs-repick-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec a; a.name = "r.foo"; a.versionConstraint = "1.0.0";
    DependencySpec b; b.name = "r.baz"; b.versionConstraint = "1.0.0";
    deps.push_back(a); deps.push_back(b);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 3u);

    std::string sharedPick;
    for (const auto& r : *resolved) {
        if (r.name == "r.shared") sharedPick = r.version;
    }
    EXPECT_EQ(sharedPick, "1.5.0");

    rmTree(root);
    rmTree(projectDir);
    rmTree(homeDir);
}

TEST(DependencyTests, mvsRejectsContradictoryExactRequirements) {
    // foo asks for shared 1.0.0 exact; baz asks for shared 2.0.0 exact.
    // No single version satisfies both — MVS should error with both
    // constraints cited.
    auto root = makeFsRepo({
        {"x.foo",    "1.0.0", "foo"},
        {"x.baz",    "1.0.0", "baz"},
        {"x.shared", "1.0.0", "s1"},
        {"x.shared", "2.0.0", "s2"},
    });
    writeSidecarManifest(root, "x.foo", "1.0.0", {{"x.shared", "1.0.0"}});
    writeSidecarManifest(root, "x.baz", "1.0.0", {{"x.shared", "2.0.0"}});
    writeSidecarManifest(root, "x.shared", "1.0.0", {});
    writeSidecarManifest(root, "x.shared", "2.0.0", {});

    auto projectDir = makeTempDir("mvs-contra-proj");
    auto homeDir    = makeTempDir("mvs-contra-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec a; a.name = "x.foo"; a.versionConstraint = "1.0.0";
    DependencySpec b; b.name = "x.baz"; b.versionConstraint = "1.0.0";
    deps.push_back(a); deps.push_back(b);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache);
    ASSERT_FALSE((bool)resolved);
    auto msg = errorText(resolved.takeError());
    EXPECT_NE(msg.find("x.shared"), std::string::npos);
    EXPECT_NE(msg.find("1.0.0"), std::string::npos);
    EXPECT_NE(msg.find("2.0.0"), std::string::npos)
        << "error should cite both contributing constraints, got: " << msg;

    rmTree(root);
    rmTree(projectDir);
    rmTree(homeDir);
}

TEST(DependencyTests, mvsConflictingFromPinsError) {
    auto rootA = makeFsRepo({{"f.pkg", "1.0.0", "fromA"}});
    auto rootB = makeFsRepo({{"f.pkg", "1.0.0", "fromB"}});
    writeSidecarManifest(rootA, "f.pkg", "1.0.0", {});
    writeSidecarManifest(rootB, "f.pkg", "1.0.0", {});

    auto projectDir = makeTempDir("mvs-from-proj");
    auto homeDir    = makeTempDir("mvs-from-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("A", rootA.string()),
        std::make_shared<FilesystemRepository>("B", rootB.string()),
    };
    // Two root deps for the same package with different `from` pins.
    std::vector<DependencySpec> deps;
    DependencySpec a; a.name = "f.pkg"; a.versionConstraint = "1.0.0";
    a.fromRepo = "A";
    DependencySpec b; b.name = "f.pkg"; b.versionConstraint = "1.0.0";
    b.fromRepo = "B";
    deps.push_back(a); deps.push_back(b);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache);
    ASSERT_FALSE((bool)resolved);
    auto msg = errorText(resolved.takeError());
    EXPECT_NE(msg.find("conflicting 'from'"), std::string::npos)
        << "expected from-pin conflict error, got: " << msg;

    rmTree(rootA);
    rmTree(rootB);
    rmTree(projectDir);
    rmTree(homeDir);
}

// ─── settings.overrides parsing ───────────────────────────────────────

TEST(DependencyTests, parsesOverridesShortAndObjectForms) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "settings": {
            "overrides": {
                "acme.metrics": "1.2.5",
                "old.lib":      ">=2.3.4,<3.0.0",
                "down.grade":   { "version": "1.0.0",
                                  "allow-major-downgrade": true },
                "vendor.fork":  { "path": "./vendor/patched-fork" },
                "old.vuln":     { "git": "https://example.com/fork",
                                  "rev": "a1b2c3d" }
            }
        }
    })");
    auto overrides = parseOverrides(m);
    ASSERT_TRUE((bool)overrides) << errorText(overrides.takeError());
    ASSERT_EQ(overrides->size(), 5u);

    auto byName = [&](const std::string& n) -> const OverrideSpec* {
        for (const auto& o : *overrides) if (o.name == n) return &o;
        return nullptr;
    };
    ASSERT_NE(byName("acme.metrics"), nullptr);
    EXPECT_EQ(byName("acme.metrics")->versionConstraint, "1.2.5");

    ASSERT_NE(byName("old.lib"), nullptr);
    EXPECT_EQ(byName("old.lib")->versionConstraint, ">=2.3.4,<3.0.0");

    ASSERT_NE(byName("down.grade"), nullptr);
    EXPECT_EQ(byName("down.grade")->versionConstraint, "1.0.0");
    EXPECT_TRUE(byName("down.grade")->allowMajorDowngrade);

    ASSERT_NE(byName("vendor.fork"), nullptr);
    ASSERT_TRUE(byName("vendor.fork")->path.has_value());
    EXPECT_EQ(*byName("vendor.fork")->path, "./vendor/patched-fork");

    ASSERT_NE(byName("old.vuln"), nullptr);
    ASSERT_TRUE(byName("old.vuln")->git.has_value());
    EXPECT_EQ(*byName("old.vuln")->git, "https://example.com/fork");
    ASSERT_TRUE(byName("old.vuln")->rev.has_value());
    EXPECT_EQ(*byName("old.vuln")->rev, "a1b2c3d");
}

// ─── MVS with overrides (Phase 6b) ────────────────────────────────────

TEST(DependencyTests, mvsOverridePinsTransitive) {
    // root -> foo, foo -> bar >=1.0.0. Without overrides MVS picks
    // bar 1.0.0 (lowest). Override forces bar 1.5.0; the override
    // wins (no direct dep on bar).
    auto root = makeFsRepo({
        {"o.foo", "1.0.0", "foo"},
        {"o.bar", "1.0.0", "b1"},
        {"o.bar", "1.5.0", "b2"},
        {"o.bar", "1.9.0", "b3"},
    });
    writeSidecarManifest(root, "o.foo", "1.0.0", {{"o.bar", ">=1.0.0"}});
    writeSidecarManifest(root, "o.bar", "1.0.0", {});
    writeSidecarManifest(root, "o.bar", "1.5.0", {});
    writeSidecarManifest(root, "o.bar", "1.9.0", {});

    auto projectDir = makeTempDir("ov-pin-proj");
    auto homeDir    = makeTempDir("ov-pin-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec d; d.name = "o.foo"; d.versionConstraint = "1.0.0";
    deps.push_back(d);

    std::vector<OverrideSpec> overrides;
    OverrideSpec ov; ov.name = "o.bar"; ov.versionConstraint = "1.5.0";
    overrides.push_back(ov);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());

    std::string barVersion;
    for (const auto& r : *resolved) {
        if (r.name == "o.bar") barVersion = r.version;
    }
    EXPECT_EQ(barVersion, "1.5.0");

    rmTree(root);
    rmTree(projectDir);
    rmTree(homeDir);
}

TEST(DependencyTests, mvsOverrideIgnoredForDirectRootDep) {
    // Root depends directly on bar 1.9.0. Override says bar 1.0.0.
    // Spec: direct beats override. bar stays at 1.9.0.
    auto root = makeFsRepo({
        {"o2.bar", "1.0.0", "b1"},
        {"o2.bar", "1.5.0", "b2"},
        {"o2.bar", "1.9.0", "b3"},
    });
    writeSidecarManifest(root, "o2.bar", "1.0.0", {});
    writeSidecarManifest(root, "o2.bar", "1.5.0", {});
    writeSidecarManifest(root, "o2.bar", "1.9.0", {});

    auto projectDir = makeTempDir("ov-direct-proj");
    auto homeDir    = makeTempDir("ov-direct-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec d; d.name = "o2.bar"; d.versionConstraint = "1.9.0";
    deps.push_back(d);

    std::vector<OverrideSpec> overrides;
    OverrideSpec ov; ov.name = "o2.bar"; ov.versionConstraint = "1.0.0";
    overrides.push_back(ov);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 1u);
    EXPECT_EQ((*resolved)[0].version, "1.9.0");

    rmTree(root);
    rmTree(projectDir);
    rmTree(homeDir);
}

TEST(DependencyTests, mvsOverrideMajorDowngradeErrors) {
    // root -> foo, foo -> bar >=2.0.0. Override pins bar to 1.0.0.
    // That's a major-version drop. Default policy errors.
    auto root = makeFsRepo({
        {"d.foo", "1.0.0", "foo"},
        {"d.bar", "1.0.0", "b1"},
        {"d.bar", "2.0.0", "b2"},
        {"d.bar", "2.5.0", "b3"},
    });
    writeSidecarManifest(root, "d.foo", "1.0.0", {{"d.bar", ">=2.0.0"}});
    writeSidecarManifest(root, "d.bar", "1.0.0", {});
    writeSidecarManifest(root, "d.bar", "2.0.0", {});
    writeSidecarManifest(root, "d.bar", "2.5.0", {});

    auto projectDir = makeTempDir("ov-dg-err-proj");
    auto homeDir    = makeTempDir("ov-dg-err-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec d; d.name = "d.foo"; d.versionConstraint = "1.0.0";
    deps.push_back(d);

    std::vector<OverrideSpec> overrides;
    OverrideSpec ov; ov.name = "d.bar"; ov.versionConstraint = "1.0.0";
    overrides.push_back(ov);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides);
    ASSERT_FALSE((bool)resolved);
    auto msg = errorText(resolved.takeError());
    EXPECT_NE(msg.find("d.bar"), std::string::npos);
    EXPECT_NE(msg.find("allow-major-downgrade"), std::string::npos)
        << "expected guidance toward the escape hatch, got: " << msg;

    rmTree(root);
    rmTree(projectDir);
    rmTree(homeDir);
}

TEST(DependencyTests, mvsOverrideMajorDowngradeAllowed) {
    // Same as above but with allow-major-downgrade=true. The build
    // succeeds and bar resolves to 1.0.0.
    auto root = makeFsRepo({
        {"d2.foo", "1.0.0", "foo"},
        {"d2.bar", "1.0.0", "b1"},
        {"d2.bar", "2.0.0", "b2"},
    });
    writeSidecarManifest(root, "d2.foo", "1.0.0", {{"d2.bar", ">=2.0.0"}});
    writeSidecarManifest(root, "d2.bar", "1.0.0", {});
    writeSidecarManifest(root, "d2.bar", "2.0.0", {});

    auto projectDir = makeTempDir("ov-dg-allow-proj");
    auto homeDir    = makeTempDir("ov-dg-allow-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec d; d.name = "d2.foo"; d.versionConstraint = "1.0.0";
    deps.push_back(d);

    std::vector<OverrideSpec> overrides;
    OverrideSpec ov; ov.name = "d2.bar"; ov.versionConstraint = "1.0.0";
    ov.allowMajorDowngrade = true;
    overrides.push_back(ov);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    std::string barVersion;
    for (const auto& r : *resolved) {
        if (r.name == "d2.bar") barVersion = r.version;
    }
    EXPECT_EQ(barVersion, "1.0.0");

    rmTree(root);
    rmTree(projectDir);
    rmTree(homeDir);
}

TEST(DependencyTests, mvsOverrideRangeNarrowsResolution) {
    // foo -> bar >=1.0.0. Bar repo has 1.0.0, 1.5.0, 1.9.0.
    // Without override MVS picks 1.0.0. Override range
    // ">=1.5.0,<1.9.0" picks 1.5.0 (lowest satisfying).
    auto root = makeFsRepo({
        {"o3.foo", "1.0.0", "foo"},
        {"o3.bar", "1.0.0", "b1"},
        {"o3.bar", "1.5.0", "b2"},
        {"o3.bar", "1.9.0", "b3"},
    });
    writeSidecarManifest(root, "o3.foo", "1.0.0", {{"o3.bar", ">=1.0.0"}});
    writeSidecarManifest(root, "o3.bar", "1.0.0", {});
    writeSidecarManifest(root, "o3.bar", "1.5.0", {});
    writeSidecarManifest(root, "o3.bar", "1.9.0", {});

    auto projectDir = makeTempDir("ov-range-proj");
    auto homeDir    = makeTempDir("ov-range-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec d; d.name = "o3.foo"; d.versionConstraint = "1.0.0";
    deps.push_back(d);

    std::vector<OverrideSpec> overrides;
    OverrideSpec ov; ov.name = "o3.bar";
    ov.versionConstraint = ">=1.5.0,<1.9.0";
    overrides.push_back(ov);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    std::string barVersion;
    for (const auto& r : *resolved) {
        if (r.name == "o3.bar") barVersion = r.version;
    }
    EXPECT_EQ(barVersion, "1.5.0");

    rmTree(root);
    rmTree(projectDir);
    rmTree(homeDir);
}

// Path + git overrides ship in Phase 6c (see PathOverrideTests.cpp +
// GitOverrideTests.cpp for full coverage). The boundary case worth
// keeping here: a git override missing its 'rev' field errors at
// the pre-flight rather than crashing later.
TEST(DependencyTests, mvsRejectsGitOverrideMissingRev) {
    auto root = makeFsRepo({{"px.bar", "1.0.0", "x"}});
    writeSidecarManifest(root, "px.bar", "1.0.0", {});
    auto projectDir = makeTempDir("ov-git-proj");
    auto homeDir    = makeTempDir("ov-git-home");
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    std::vector<DependencySpec> deps;
    DependencySpec d; d.name = "px.bar"; d.versionConstraint = "1.0.0";
    deps.push_back(d);

    std::vector<OverrideSpec> overrides;
    OverrideSpec ov; ov.name = "px.bar";
    ov.git = "https://example.com/x";
    // No rev set — must be rejected.
    overrides.push_back(ov);

    ArtifactCache cache(projectDir.string(), homeDir.string());
    auto resolved = resolveMvs(deps, repos, cache, overrides);
    ASSERT_FALSE((bool)resolved);
    EXPECT_NE(errorText(resolved.takeError()).find(
                  "requires 'rev'"),
              std::string::npos);

    rmTree(root);
    rmTree(projectDir);
    rmTree(homeDir);
}

// ─── resolveProjectDependencies (manifest → resolved set) ─────────────

TEST(DependencyTests, projectResolutionEmptyWhenNoDepsDeclared) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" }
    })");
    auto projectDir = makeTempDir("proj-empty-proj");
    auto homeDir    = makeTempDir("proj-empty-home");
    auto resolved = resolveProjectDependencies(
        m, projectDir.string(), homeDir.string());
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    EXPECT_TRUE(resolved->empty());
    rmTree(projectDir);
    rmTree(homeDir);
}

TEST(DependencyTests, projectResolutionEndToEnd) {
    auto repoRoot = makeFsRepo({
        {"p.foo", "1.0.0", "foo"},
        {"p.bar", "1.5.0", "bar"},
    });
    writeSidecarManifest(repoRoot, "p.foo", "1.0.0", {{"p.bar", "1.*"}});
    writeSidecarManifest(repoRoot, "p.bar", "1.5.0", {});

    // Manifest declares a single direct dep + the fs repo.
    std::ostringstream js;
    js << R"({
        "details": { "name": "consumer", "version": "0.1.0" },
        "settings": {
            "repositories": [
                { "name": "test", "type": "filesystem",
                  "path": ")" << repoRoot.generic_string() << R"(" }
            ],
            "dependencies": {
                "p.foo": "1.0.0"
            }
        }
    })";
    auto m = mustLoad(js.str());

    auto projectDir = makeTempDir("proj-e2e-proj");
    auto homeDir    = makeTempDir("proj-e2e-home");
    auto resolved = resolveProjectDependencies(
        m, projectDir.string(), homeDir.string());
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 2u);
    EXPECT_EQ((*resolved)[0].name, "p.foo");
    EXPECT_EQ((*resolved)[0].version, "1.0.0");
    EXPECT_EQ((*resolved)[1].name, "p.bar");
    EXPECT_EQ((*resolved)[1].version, "1.5.0");
    // Cached paths actually exist.
    EXPECT_TRUE(std::filesystem::exists((*resolved)[0].artifactPath));
    EXPECT_TRUE(std::filesystem::exists((*resolved)[1].artifactPath));

    rmTree(repoRoot);
    rmTree(projectDir);
    rmTree(homeDir);
}

TEST(DependencyTests, projectResolutionErrorsWhenDepUnavailableEverywhere) {
    // With the implicit ~/.olla local repository always prepended,
    // declaring a dep without any remote `repositories` is no longer an
    // error by itself — it only fails when the dep is in neither the
    // local store nor a remote. The error then names the package and
    // the repos tried (which always includes "olla").
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "settings": {
            "dependencies": { "missing": "1.0.0" }
        }
    })");
    auto projectDir = makeTempDir("proj-norepo-proj");
    auto homeDir    = makeTempDir("proj-norepo-home");  // empty <home>/.olla
    auto resolved = resolveProjectDependencies(
        m, projectDir.string(), homeDir.string());
    ASSERT_FALSE((bool)resolved);
    auto msg = errorText(resolved.takeError());
    EXPECT_NE(msg.find("missing"), std::string::npos)
        << "expected the missing package named, got: " << msg;
    EXPECT_NE(msg.find("olla"), std::string::npos)
        << "expected the olla local repo among those tried, got: " << msg;
    rmTree(projectDir);
    rmTree(homeDir);
}

TEST(DependencyTests, projectResolutionAppliesOverrides) {
    // Verify the top-level helper actually plumbs overrides through
    // to resolveMvs.
    auto repoRoot = makeFsRepo({
        {"o4.foo", "1.0.0", "foo"},
        {"o4.bar", "1.0.0", "b1"},
        {"o4.bar", "1.5.0", "b2"},
    });
    writeSidecarManifest(repoRoot, "o4.foo", "1.0.0", {{"o4.bar", ">=1.0.0"}});
    writeSidecarManifest(repoRoot, "o4.bar", "1.0.0", {});
    writeSidecarManifest(repoRoot, "o4.bar", "1.5.0", {});

    std::ostringstream js;
    js << R"({
        "details": { "name": "consumer", "version": "0.1.0" },
        "settings": {
            "repositories": [
                { "name": "test", "type": "filesystem",
                  "path": ")" << repoRoot.generic_string() << R"(" }
            ],
            "dependencies": { "o4.foo": "1.0.0" },
            "overrides":    { "o4.bar": "1.5.0" }
        }
    })";
    auto m = mustLoad(js.str());

    auto projectDir = makeTempDir("proj-ov-proj");
    auto homeDir    = makeTempDir("proj-ov-home");
    auto resolved = resolveProjectDependencies(
        m, projectDir.string(), homeDir.string());
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());

    std::string barVersion;
    for (const auto& r : *resolved) {
        if (r.name == "o4.bar") barVersion = r.version;
    }
    // Without the override, MVS would have picked 1.0.0 (lowest).
    // The override forces 1.5.0.
    EXPECT_EQ(barVersion, "1.5.0");

    rmTree(repoRoot);
    rmTree(projectDir);
    rmTree(homeDir);
}
