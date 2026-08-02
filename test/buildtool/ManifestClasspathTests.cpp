// Characterization tests for manifest-driven classpath resolution.
//
// These exist to pin behaviour that a defect report claimed was MISSING.
// `buildtool-dependency-classpath-spec` §1.1 reported that `cajeta.json`
// `dependencies` are inert for local builds and that consumers must pass
// `--classpath` by hand. That was verified end-to-end against released
// v0.14.0 on 2026-08-02 and found to be STALE: a project declaring a
// dependency and nothing else resolves it, caches it, compiles and links
// against it, and runs.
//
// So every test here is EXPECTED TO PASS on a healthy tree. They are a
// regression net, not a to-do list — if one of them ever fails, the
// manifest→classpath path has regressed and the hand-rolled `--classpath`
// workarounds the ecosystem deleted would start coming back.
//
// The seam under test is `resolveProjectDependencies`, which is what
// `BuildAction.cpp:329-343` calls before pushing `--classpath=`. Asserting
// on the compiler argv itself would need a subprocess; asserting on the
// resolver's output pins the same contract one layer down, where the
// interesting failure modes (empty vs absent, unresolvable, ordering) live.
//
// EVERY test MUST pass `homeOverride`. A successful resolution WRITES
// THROUGH to the workstation cache (see OllaWriteThroughTests), so a test
// that omits it installs its fixtures into the developer's real ~/.olla.
// That happened once during characterization and had to be cleaned by hand.

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Repository.h"
#include "cajeta/buildtool/Resolver.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::Manifest;
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
                 ("cajeta-mfcp-test-" + tag + "-" +
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

    // A filesystem repository laid out the way FilesystemRepository reads
    // it: <root>/<pkg>/<version>/<pkg>-<version>.cja plus a cajeta.json
    // sidecar carrying the package's own details.
    struct PkgVer { std::string pkg; std::string version; };

    std::filesystem::path makeFsRepo(const std::vector<PkgVer>& pkgs) {
        auto root = makeTempDir("fsrepo");
        for (const auto& pv : pkgs) {
            auto dir = root / pv.pkg / pv.version;
            std::filesystem::create_directories(dir);
            std::ofstream art(
                dir / (pv.pkg + "-" + pv.version + ".cja"),
                std::ios::binary);
            art << "stub-archive-bytes";
            std::ofstream side(dir / "cajeta.json", std::ios::binary);
            side << "{\"details\":{\"name\":\"" << pv.pkg
                 << "\",\"version\":\"" << pv.version << "\"}}";
        }
        return root;
    }

    // A manifest with the given dependency block and, optionally, a
    // filesystem repository serving `repoRoot`.
    std::string manifestSrc(
        const std::string& depsJson,
        const std::filesystem::path* repoRoot = nullptr) {
        std::ostringstream src;
        src << R"({"details":{"name":"app.consumer","version":"0.1.0"},)"
            << R"("settings":{)";
        if (repoRoot) {
            src << R"("repositories":[{"name":"localfs",)"
                << R"("type":"filesystem","path":")"
                << repoRoot->generic_string() << R"("}],)";
        }
        src << R"("dependencies":)" << depsJson << "}}";
        return src.str();
    }

} // namespace

// §1.1's core claim, inverted. A declared dependency served by a declared
// repository resolves to a concrete cached artifact — which is exactly
// what BuildAction joins into `--classpath=`. If this fails, dependencies
// really have gone inert.
TEST(ManifestClasspathTests, declaredDependencyResolvesToAnArtifact) {
    auto repoRoot = makeFsRepo({{"dev.example.greet", "0.1.0"}});
    auto m = mustLoad(
        manifestSrc(R"({"dev.example.greet":"0.1.*"})", &repoRoot));
    auto projectDir = makeTempDir("proj-resolves");
    auto homeDir    = makeTempDir("home-resolves");

    auto resolved = resolveProjectDependencies(
        m, projectDir.string(), homeDir.string());
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 1u);

    const auto& r = (*resolved)[0];
    EXPECT_EQ(r.name, "dev.example.greet");
    EXPECT_EQ(r.version, "0.1.0");
    // The path is what lands on the classpath, so it must be a real file
    // and not merely a non-empty string.
    ASSERT_FALSE(r.artifactPath.empty());
    EXPECT_TRUE(std::filesystem::exists(r.artifactPath))
        << "classpath entry does not exist: " << r.artifactPath;
}

// An empty `--classpath=` and an absent one are different inputs to the
// compiler. BuildAction only pushes the flag when the resolved list is
// non-empty, so "no deps" must come back EMPTY rather than as one blank
// entry that would join into `--classpath=`.
TEST(ManifestClasspathTests, noDependenciesYieldsEmptyRatherThanBlank) {
    auto m = mustLoad(manifestSrc("{}"));
    auto projectDir = makeTempDir("proj-empty");
    auto homeDir    = makeTempDir("home-empty");

    auto resolved = resolveProjectDependencies(
        m, projectDir.string(), homeDir.string());
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    EXPECT_TRUE(resolved->empty());
}

// Built-in stdlib deps are satisfied by the compiler's embedded stdlib, so
// they are dropped before resolution. The point of the test is the second
// half: NO repositories are declared here, so if the drop regressed this
// would fail trying to reach a repo that does not exist — which is what a
// fresh `cajeta init` project would hit on a machine with no network.
TEST(ManifestClasspathTests, builtinStdlibDepsNeedNoRepository) {
    auto m = mustLoad(manifestSrc(
        R"({"cajeta.lang":"1.0.*","cajeta.io":"1.0.*"})"));
    auto projectDir = makeTempDir("proj-stdlib");
    auto homeDir    = makeTempDir("home-stdlib");

    auto resolved = resolveProjectDependencies(
        m, projectDir.string(), homeDir.string());
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    EXPECT_TRUE(resolved->empty());
}

// The failure mode that matters most. A dependency nothing can serve must
// FAIL, and the message must name the package — a build that quietly
// omitted the classpath entry would surface later as "symbol not found"
// from the compiler, pointing at the wrong layer entirely.
TEST(ManifestClasspathTests, unresolvableDependencyFailsNamingThePackage) {
    auto repoRoot = makeFsRepo({{"dev.example.greet", "0.1.0"}});
    auto m = mustLoad(
        manifestSrc(R"({"dev.example.absent":"9.9.*"})", &repoRoot));
    auto projectDir = makeTempDir("proj-absent");
    auto homeDir    = makeTempDir("home-absent");

    auto resolved = resolveProjectDependencies(
        m, projectDir.string(), homeDir.string());
    ASSERT_FALSE((bool)resolved)
        << "an unservable dependency must not resolve silently";
    auto msg = errorText(resolved.takeError());
    EXPECT_NE(msg.find("dev.example.absent"), std::string::npos)
        << "error does not name the package: " << msg;
    EXPECT_NE(msg.find("9.9.*"), std::string::npos)
        << "error does not name the constraint: " << msg;
}

// A version constraint no available version satisfies is a different
// failure from an unknown package, and it must report the constraint —
// otherwise "not found" sends the reader looking for a missing repository
// when the repository is right there with the wrong version.
TEST(ManifestClasspathTests, unsatisfiableConstraintNamesTheConstraint) {
    auto repoRoot = makeFsRepo({{"dev.example.greet", "0.1.0"}});
    auto m = mustLoad(
        manifestSrc(R"({"dev.example.greet":"2.0.*"})", &repoRoot));
    auto projectDir = makeTempDir("proj-constraint");
    auto homeDir    = makeTempDir("home-constraint");

    auto resolved = resolveProjectDependencies(
        m, projectDir.string(), homeDir.string());
    ASSERT_FALSE((bool)resolved);
    auto msg = errorText(resolved.takeError());
    EXPECT_NE(msg.find("2.0.*"), std::string::npos)
        << "error does not name the constraint: " << msg;
}

// Classpath ORDER decides which archive wins a duplicate symbol, so an
// unstable order is an intermittent miscompile rather than a cosmetic
// issue. Resolve the same manifest twice and require identical sequences.
TEST(ManifestClasspathTests, resolutionOrderIsDeterministic) {
    auto repoRoot = makeFsRepo({
        {"dev.example.alpha", "1.0.0"},
        {"dev.example.beta",  "1.0.0"},
        {"dev.example.gamma", "1.0.0"},
    });
    auto deps = R"({"dev.example.alpha":"1.0.*",)"
                R"("dev.example.beta":"1.0.*",)"
                R"("dev.example.gamma":"1.0.*"})";

    std::vector<std::string> first;
    std::vector<std::string> second;
    for (auto* sink : {&first, &second}) {
        auto m = mustLoad(manifestSrc(deps, &repoRoot));
        auto projectDir = makeTempDir("proj-order");
        auto homeDir    = makeTempDir("home-order");
        auto resolved = resolveProjectDependencies(
            m, projectDir.string(), homeDir.string());
        ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
        for (const auto& r : *resolved) sink->push_back(r.name);
    }
    ASSERT_EQ(first.size(), 3u);
    EXPECT_EQ(first, second);
}

// The write-through hazard, pinned. A successful resolution installs into
// the workstation cache; `homeOverride` is what keeps that out of the
// developer's real ~/.olla. Assert the override directory actually
// receives something, so that a regression making homeOverride inert
// fails HERE rather than by quietly polluting whoever runs the suite.
TEST(ManifestClasspathTests, homeOverrideContainsTheWriteThrough) {
    auto repoRoot = makeFsRepo({{"dev.example.greet", "0.1.0"}});
    auto m = mustLoad(
        manifestSrc(R"({"dev.example.greet":"0.1.*"})", &repoRoot));
    auto projectDir = makeTempDir("proj-wt");
    auto homeDir    = makeTempDir("home-wt");

    auto resolved = resolveProjectDependencies(
        m, projectDir.string(), homeDir.string());
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());

    bool sawArtifactUnderOverride = false;
    for (auto& e :
         std::filesystem::recursive_directory_iterator(homeDir)) {
        if (e.is_regular_file() && e.path().extension() == ".cja") {
            sawArtifactUnderOverride = true;
            break;
        }
    }
    EXPECT_TRUE(sawArtifactUnderOverride)
        << "nothing was written under the pinned home override ("
        << homeDir << ") — if resolution still write-through installs, it "
           "is going somewhere else, possibly the real ~/.olla";
}
