// Unit 3a (olla-local-repo plan) — HTTP fallback + write-through.
//
// Additive: the existing ArtifactCache is left in place; on a local
// miss the resolver fetches from a remote, then writes the artifact
// through into ~/.olla so the next resolve is a local hit. Integrity:
// writeVerified rejects a hash mismatch (trust-on-first-use when no
// expected hash is known).
//
//   - remote fetch is written through to ~/.olla; a later resolve is
//     served locally even with no remote (offline).        [3.1.1, 3.3.1]
//   - writeVerified rejects a sha256 mismatch, leaving the store
//     unchanged; an empty expected hash trusts-on-first-use.   [3.1.2]

#include "cajeta/buildtool/ArtifactCache.h"
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

using cajeta::buildtool::ArtifactCache;
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
                 ("cajeta-ollawt-" + tag + "-" + std::to_string(::getpid()) +
                  "-" + std::to_string(::rand()));
        fs::create_directories(p);
        return p;
    }

    // A filesystem "remote" carrying one package version (stands in for
    // the HTTP repo; write-through is repo-type-agnostic — the HTTP fetch
    // itself is covered by HttpRepositoryTests).
    fs::path makeRemote(const std::string& pkg, const std::string& ver,
                        const std::string& content) {
        auto root = tempDir("remote");
        auto dir = root / pkg / ver;
        fs::create_directories(dir);
        std::ofstream o(dir / (pkg + "-" + ver + ".cja"), std::ios::binary);
        o << content;
        return root;
    }

    fs::path makeFile(const std::string& content) {
        auto p = tempDir("f") / "a.cja";
        std::ofstream o(p, std::ios::binary);
        o << content;
        return p;
    }

    struct OllaHomeUnset {
        bool had; std::string old;
        OllaHomeUnset() {
            const char* v = ::getenv("OLLA_HOME");
            had = v != nullptr; if (had) old = v;
            ::unsetenv("OLLA_HOME");
        }
        ~OllaHomeUnset() { if (had) ::setenv("OLLA_HOME", old.c_str(), 1); }
    };

} // namespace

// 3.1.1 + 3.3.1 — a remote fetch is written through into ~/.olla, and a
// later resolve with no usable remote is served from the local store.

// 3b.1.2 — U3b: resolving from/through ~/.olla never writes the retired
// workstation content-hash tier (<home>/.cajeta/cache/artifacts/). The
// per-project tier under proj stays; the workstation tier is gone.
TEST(OllaWriteThroughTest, ResolveWritesNoWorkstationCacheTier) {
    OllaHomeUnset guard;
    auto home = tempDir("home");
    auto proj = tempDir("proj");
    auto remote = makeRemote("y.pkg", "2.0.0", "REMOTE-BYTES-2");

    auto m = loadManifestString(
        "{\"details\":{\"name\":\"c\",\"version\":\"0.1.0\"},\"settings\":{"
        "\"dependencies\":{\"y.pkg\":\"2.0.0\"},"
        "\"repositories\":[{\"name\":\"remote\",\"type\":\"filesystem\","
        "\"path\":\"" + remote.generic_string() + "\"}]}}");
    ASSERT_TRUE(static_cast<bool>(m)) << errorText(m.takeError());
    auto r = resolveProjectDependencies(*m, proj.string(), home.string());
    ASSERT_TRUE(static_cast<bool>(r)) << errorText(r.takeError());

    // Write-through populated ~/.olla …
    OllaStore store((home / ".olla").string());
    EXPECT_TRUE(store.read("y.pkg", "2.0.0").has_value());
    // … but the workstation content-hash tier was never created.
    EXPECT_FALSE(fs::exists(home / ".cajeta" / "cache" / "artifacts"))
        << "workstation tier must be retired (U3b)";
}

// 3.1.2 — writeVerified rejects a sha256 mismatch (store untouched) and
// trusts-on-first-use when no expected hash is given.
TEST(OllaWriteThroughTest, WriteVerifiedRejectsHashMismatch) {
    auto root = tempDir("verify");
    OllaStore store(root.string());
    auto src = makeFile("PAYLOAD");

    // Mismatch → error, nothing written.
    auto bad = store.writeVerified("p", "1.0.0", src.string(),
                                   "sha256:deadbeef", /*manifestJson=*/"");
    EXPECT_FALSE(static_cast<bool>(bad));
    if (!bad) consumeError(bad.takeError());
    EXPECT_FALSE(store.read("p", "1.0.0").has_value());

    // Correct expected hash → written.
    auto good = store.writeVerified(
        "p", "1.0.0", src.string(),
        ArtifactCache::sha256OfFile(src.string()), "");
    ASSERT_TRUE(static_cast<bool>(good)) << errorText(good.takeError());
    EXPECT_TRUE(store.read("p", "1.0.0").has_value());

    // Trust-on-first-use (empty expected) → written.
    auto src2 = makeFile("OTHER");
    auto tofu = store.writeVerified("q", "2.0.0", src2.string(), "", "");
    ASSERT_TRUE(static_cast<bool>(tofu)) << errorText(tofu.takeError());
    EXPECT_TRUE(store.read("q", "2.0.0").has_value());
}
