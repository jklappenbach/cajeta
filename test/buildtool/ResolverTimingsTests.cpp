// Tests for ResolverTimings — the counters/durations populated when
// `resolveProjectDependencies` is asked to instrument itself. Used
// by `cajeta info --resolve-time`.

#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Resolver.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::resolveProjectDependencies;
using cajeta::buildtool::ResolverTimings;

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
                 ("cajeta-rt-test-" + tag + "-" +
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

    cajeta::buildtool::Manifest mustLoad(const std::string& src) {
        auto m = loadManifestString(src);
        if (!m) {
            ADD_FAILURE() << errorText(m.takeError());
            return {};
        }
        return std::move(*m);
    }

    std::string makeManifestSrc(
        const std::filesystem::path& repoRoot,
        const std::vector<std::pair<std::string, std::string>>& deps) {
        std::ostringstream out;
        out << R"({
  "details": { "name": "demo.app", "version": "0.1.0" },
  "settings": {
    "repositories": [
      { "name": "local", "type": "filesystem", "path": ")"
            << repoRoot.string() << R"(" }
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

// ─── nullptr safe: existing callers untouched ─────────────────────────

TEST(ResolverTimingsTests, nullTimingsDoesNotCrashOrAlter) {
    auto repoRoot = makeFsRepo({{"acme.lib", "1.0.0"}});
    writeSidecar(repoRoot, "acme.lib", "1.0.0", {});
    auto src = makeManifestSrc(repoRoot, {{"acme.lib", "1.0.0"}});
    auto m = mustLoad(src);
    auto proj = makeTempDir("nullptimings");
    auto result = resolveProjectDependencies(
        m, proj.string(), std::nullopt, nullptr);
    ASSERT_TRUE(static_cast<bool>(result)) << errorText(result.takeError());
    EXPECT_EQ(result->size(), 1u);
}

// ─── counters track per-phase calls on a single direct dep ────────────

TEST(ResolverTimingsTests, countersOnSingleDirectDep) {
    auto repoRoot = makeFsRepo({{"acme.lib", "1.0.0"}});
    writeSidecar(repoRoot, "acme.lib", "1.0.0", {});
    auto src = makeManifestSrc(repoRoot, {{"acme.lib", "1.0.0"}});
    auto m = mustLoad(src);
    auto proj = makeTempDir("counters-1");

    ResolverTimings t;
    auto result = resolveProjectDependencies(
        m, proj.string(), std::nullopt, &t);
    ASSERT_TRUE(static_cast<bool>(result)) << errorText(result.takeError());

    EXPECT_EQ(t.depsResolved, 1);
    EXPECT_GE(t.mvsIterations, 1);
    // resolveMvs's pickLowestForAll walks repos and calls
    // listVersions once per repo until a match. For a single
    // direct dep against one repo: exactly 1 list call.
    EXPECT_EQ(t.listVersionsCalls, 1);
    EXPECT_EQ(t.fetchCalls, 1);
    EXPECT_EQ(t.fetchManifestCalls, 1);

    // Wall-clock counters should be non-negative.
    EXPECT_GE(t.total.count(), 0);
    EXPECT_GE(t.listVersions.count(), 0);
    EXPECT_GE(t.fetch.count(), 0);
    EXPECT_GE(t.fetchManifest.count(), 0);
}

// ─── transitive graph bumps deps + per-call counts ────────────────────

TEST(ResolverTimingsTests, transitiveGraphBumpsCounters) {
    auto repoRoot = makeFsRepo({
        {"acme.lib", "1.0.0"},
        {"acme.util", "2.0.0"},
    });
    // acme.lib@1.0.0 depends on acme.util@2.0.0
    writeSidecar(repoRoot, "acme.lib", "1.0.0",
                 {{"acme.util", "2.0.0"}});
    writeSidecar(repoRoot, "acme.util", "2.0.0", {});

    auto src = makeManifestSrc(repoRoot, {{"acme.lib", "1.0.0"}});
    auto m = mustLoad(src);
    auto proj = makeTempDir("counters-trans");

    ResolverTimings t;
    auto result = resolveProjectDependencies(
        m, proj.string(), std::nullopt, &t);
    ASSERT_TRUE(static_cast<bool>(result)) << errorText(result.takeError());

    EXPECT_EQ(t.depsResolved, 2);
    EXPECT_GE(t.listVersionsCalls, 2);
    EXPECT_GE(t.fetchCalls, 2);
    EXPECT_GE(t.fetchManifestCalls, 2);
}

// ─── deps resolved equals output size on success ──────────────────────

TEST(ResolverTimingsTests, depsResolvedEqualsOutputCount) {
    auto repoRoot = makeFsRepo({
        {"acme.lib", "1.0.0"},
        {"other.pkg", "0.1.0"},
    });
    writeSidecar(repoRoot, "acme.lib", "1.0.0", {});
    writeSidecar(repoRoot, "other.pkg", "0.1.0", {});
    auto src = makeManifestSrc(repoRoot, {
        {"acme.lib", "1.0.0"},
        {"other.pkg", "0.1.0"},
    });
    auto m = mustLoad(src);
    auto proj = makeTempDir("counters-multi");

    ResolverTimings t;
    auto result = resolveProjectDependencies(
        m, proj.string(), std::nullopt, &t);
    ASSERT_TRUE(static_cast<bool>(result)) << errorText(result.takeError());
    EXPECT_EQ(static_cast<int>(result->size()), t.depsResolved);
    EXPECT_EQ(t.depsResolved, 2);
}

// ─── empty-dep manifest leaves timings near-zero ──────────────────────

TEST(ResolverTimingsTests, noDepsLeavesCountersZero) {
    auto src = R"({
        "details": { "name": "d", "version": "0.1.0" }
    })";
    auto m = mustLoad(src);
    auto proj = makeTempDir("counters-empty");

    ResolverTimings t;
    auto result = resolveProjectDependencies(
        m, proj.string(), std::nullopt, &t);
    ASSERT_TRUE(static_cast<bool>(result)) << errorText(result.takeError());
    EXPECT_EQ(result->size(), 0u);
    EXPECT_EQ(t.depsResolved, 0);
    EXPECT_EQ(t.listVersionsCalls, 0);
    EXPECT_EQ(t.fetchCalls, 0);
    EXPECT_EQ(t.fetchManifestCalls, 0);
    EXPECT_EQ(t.mvsIterations, 0);
}
