// Unit 2 (olla-local-repo plan) — local-first resolution.
//
// resolveProjectDependencies prepends the implicit `~/.olla/` store as
// the highest-priority repository, so a dependency present locally is
// resolved from it and the declared remotes are never consulted
// (pickLowestForAll short-circuits on the first satisfying repo). With
// the store always available, declaring a dependency with NO remote
// `repositories` is no longer an error — the tools/mcp case after a
// `cajeta install`.
//
//   - olla wins over a declared remote (remote never touched).        [2.1.1]
//   - a dep + zero declared repos resolves from olla.                 [2.1.1]
//   - the resolved artifact path is populated (feeds --classpath).    [2.1.2]

#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/OllaStore.h"
#include "cajeta/buildtool/Resolver.h"

#include <gtest/gtest.h>

#include "../PortableEnv.h"
#include <llvm/Support/Error.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::OllaStore;
using cajeta::buildtool::resolveProjectDependencies;

namespace {

    namespace fs = std::filesystem;

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    fs::path tempDir(const std::string& tag) {
        auto p = fs::temp_directory_path() /
                 ("cajeta-ollares-" + tag + "-" + std::to_string(::getpid()) +
                  "-" + std::to_string(::rand()));
        fs::create_directories(p);
        return p;
    }

    fs::path makeStubArtifact(const std::string& content) {
        auto p = tempDir("stub") / "a.cja";
        std::ofstream o(p, std::ios::binary);
        o << content;
        return p;
    }

    // Ensure $OLLA_HOME doesn't leak in from the environment so the
    // homeOverride deterministically selects <home>/.olla.
    struct OllaHomeUnset {
        bool had;
        std::string old;
        OllaHomeUnset() {
            const char* v = ::getenv("OLLA_HOME");
            had = v != nullptr;
            if (had) old = v;
            ::unsetenv("OLLA_HOME");
        }
        ~OllaHomeUnset() {
            if (had) ::setenv("OLLA_HOME", old.c_str(), 1);
        }
    };

} // namespace

// 2.1.1 — a dependency present in ~/.olla resolves from it even when the
// only declared remote is broken (it is never consulted).
TEST(OllaResolverTest, LocalHitShortCircuitsBrokenRemote) {
    OllaHomeUnset guard;
    auto home = tempDir("home");
    auto proj = tempDir("proj");

    OllaStore store((home / ".olla").string());
    auto art = makeStubArtifact("OLLA-BYTES");
    ASSERT_TRUE(static_cast<bool>(
        store.write("x.pkg", "1.0.0", art.string(), std::nullopt)));

    auto m = loadManifestString(
        "{\"details\":{\"name\":\"consumer\",\"version\":\"0.1.0\"},"
        "\"settings\":{\"dependencies\":{\"x.pkg\":\"1.0.0\"},"
        "\"repositories\":[{\"name\":\"remote\",\"type\":\"filesystem\","
        "\"path\":\"/cajeta-nonexistent-remote-xyz\"}]}}");
    ASSERT_TRUE(static_cast<bool>(m)) << errorText(m.takeError());

    auto resolved = resolveProjectDependencies(
        *m, proj.string(), /*homeOverride=*/home.string());
    ASSERT_TRUE(static_cast<bool>(resolved))
        << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 1u);
    EXPECT_EQ((*resolved)[0].name, "x.pkg");
    EXPECT_EQ((*resolved)[0].version, "1.0.0");
    EXPECT_EQ((*resolved)[0].resolvedFromRepo, "olla");
    EXPECT_FALSE((*resolved)[0].artifactPath.empty());  // feeds --classpath
}

// 2.1.1 — a declared dependency with NO remote repositories resolves
// from ~/.olla (the post-`cajeta install` / tools/mcp case).
TEST(OllaResolverTest, DepWithoutDeclaredReposResolvesFromOlla) {
    OllaHomeUnset guard;
    auto home = tempDir("home2");
    auto proj = tempDir("proj2");

    OllaStore store((home / ".olla").string());
    auto art = makeStubArtifact("CODEC-BYTES");
    ASSERT_TRUE(static_cast<bool>(
        store.write("dev.cajeta.codec", "0.5.0", art.string(), std::nullopt)));

    auto m = loadManifestString(
        "{\"details\":{\"name\":\"tools.mcp\",\"version\":\"0.1.0\"},"
        "\"settings\":{\"dependencies\":{\"dev.cajeta.codec\":\"0.5.0\"}}}");
    ASSERT_TRUE(static_cast<bool>(m)) << errorText(m.takeError());

    auto resolved = resolveProjectDependencies(
        *m, proj.string(), /*homeOverride=*/home.string());
    ASSERT_TRUE(static_cast<bool>(resolved))
        << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 1u);
    EXPECT_EQ((*resolved)[0].name, "dev.cajeta.codec");
    EXPECT_EQ((*resolved)[0].resolvedFromRepo, "olla");
}

// A dep that is in neither ~/.olla nor any remote still fails clearly.
TEST(OllaResolverTest, MissEverywhereStillErrors) {
    OllaHomeUnset guard;
    auto home = tempDir("home3");
    auto proj = tempDir("proj3");
    // Empty olla store (root dir exists but no packages).
    fs::create_directories(home / ".olla");

    auto m = loadManifestString(
        "{\"details\":{\"name\":\"consumer\",\"version\":\"0.1.0\"},"
        "\"settings\":{\"dependencies\":{\"absent.pkg\":\"1.0.0\"}}}");
    ASSERT_TRUE(static_cast<bool>(m)) << errorText(m.takeError());

    auto resolved = resolveProjectDependencies(
        *m, proj.string(), /*homeOverride=*/home.string());
    EXPECT_FALSE(static_cast<bool>(resolved));
    if (!resolved) consumeError(resolved.takeError());
}
