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

// ─── nullptr safe: existing callers untouched ─────────────────────────

TEST(ResolverTimingsTests, nullTimingsDoesNotCrashOrAlter) {
    auto repoRoot = makeFsRepo({{"acme.lib", "1.0.0"}});
    writeSidecar(repoRoot, "acme.lib", "1.0.0", {});
    auto src = makeManifestSrc(repoRoot, {{"acme.lib", "1.0.0"}});
    auto m = mustLoad(src);
    auto proj = makeTempDir("nullptimings");
    auto result = resolveProjectDependencies(
        m, proj.string(), proj.string(), nullptr);  // pin olla root (hermetic)
    ASSERT_TRUE(static_cast<bool>(result)) << errorText(result.takeError());
    EXPECT_EQ(result->size(), 1u);
}

// ─── counters track per-phase calls on a single direct dep ────────────


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
        m, proj.string(), proj.string(), &t);  // pin olla root (hermetic)
    ASSERT_TRUE(static_cast<bool>(result)) << errorText(result.takeError());

    EXPECT_EQ(t.depsResolved, 2);
    EXPECT_GE(t.listVersionsCalls, 2);
    EXPECT_GE(t.fetchCalls, 2);
    EXPECT_GE(t.fetchManifestCalls, 2);
}

// ─── deps resolved equals output size on success ──────────────────────


// ─── empty-dep manifest leaves timings near-zero ──────────────────────

