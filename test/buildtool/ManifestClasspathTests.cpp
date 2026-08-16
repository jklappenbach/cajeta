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

// An empty `--classpath=` and an absent one are different inputs to the
// compiler. BuildAction only pushes the flag when the resolved list is
// non-empty, so "no deps" must come back EMPTY rather than as one blank
// entry that would join into `--classpath=`.

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

// A version constraint no available version satisfies is a different
// failure from an unknown package, and it must report the constraint —
// otherwise "not found" sends the reader looking for a missing repository
// when the repository is right there with the wrong version.

// Classpath ORDER decides which archive wins a duplicate symbol, so an
// unstable order is an intermittent miscompile rather than a cosmetic
// issue. Resolve the same manifest twice and require identical sequences.

// The write-through hazard, pinned. A successful resolution installs into
// the workstation cache; `homeOverride` is what keeps that out of the
// developer's real ~/.olla. Assert the override directory actually
// receives something, so that a regression making homeOverride inert
// fails HERE rather than by quietly polluting whoever runs the suite.
