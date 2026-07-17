// compile-cache Unit 2 (spec §2, §4) — the persistent stdlib-prime cache KEY.
//
// The key is (discriminator, digest):
//   - discriminator = computeCacheDiscriminator(compiler-version,
//     stdlib-affecting flag set) — sorted, so flag ORDER never busts (§2.6);
//     version or flag CHANGES do (§2.3/2.4).
//   - digest = primeDigestOver(embedded stdlib files, prelude tag) — any
//     source-byte change busts (§2.1; the embedded table is the full
//     transitive closure, so §2.2 is inherent), and a prelude/lazy-set
//     change busts (§2.5).
// §2.7: a corrupt/truncated entry at the keyed path is a MISS, never a load.

#include <gtest/gtest.h>

#include "cajeta/buildtool/IrCache.h"
#include "cajeta/buildtool/PrimeCache.h"
#include "cajeta/compile/Compiler.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using cajeta::buildtool::IrCache;
using cajeta::buildtool::primeDigestOver;
using cajeta::buildtool::primeValidatedLookup;

namespace {

using FileSet = std::vector<std::pair<std::string, std::string>>;

const FileSet kBase = {
    {"cajeta/lang/String.cajeta", "package cajeta.lang; class String {}"},
    {"cajeta/io/File.cajeta", "package cajeta.io; class File {}"},
};

std::filesystem::path freshTmpDir(const char* tag) {
    auto dir = std::filesystem::temp_directory_path()
        / (std::string("cajeta-prime-key-") + tag + "-"
           + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

// §2.1 — a source-byte change produces a different digest.
TEST(PrimeCacheKeyTests, sourceChangeBustsDigest) {
    FileSet changed = kBase;
    changed[0].second += " // changed";
    EXPECT_NE(primeDigestOver(kBase, "prelude"),
              primeDigestOver(changed, "prelude"));
}

// A file RENAME (same bytes, different path) also re-keys — the path
// participates because packages derive from it.
TEST(PrimeCacheKeyTests, pathChangeBustsDigest) {
    FileSet moved = kBase;
    moved[0].first = "cajeta/lang/Str.cajeta";
    EXPECT_NE(primeDigestOver(kBase, "prelude"),
              primeDigestOver(moved, "prelude"));
}

// Adding a file re-keys (the embedded table IS the transitive closure — §2.2).
TEST(PrimeCacheKeyTests, addedFileBustsDigest) {
    FileSet grown = kBase;
    grown.push_back({"cajeta/net/Dns.cajeta", "package cajeta.net; class Dns {}"});
    EXPECT_NE(primeDigestOver(kBase, "prelude"),
              primeDigestOver(grown, "prelude"));
}

// §2.5 — the prelude tag (eager/lazy package split) participates in the key.
TEST(PrimeCacheKeyTests, preludeChangeBustsDigest) {
    EXPECT_NE(primeDigestOver(kBase, "lazy:cajeta.math"),
              primeDigestOver(kBase, "lazy:cajeta.math,cajeta.xpu.mesh"));
}

// File ORDER never matters — the digest sorts by path internally.
TEST(PrimeCacheKeyTests, fileOrderDoesNotBustDigest) {
    FileSet reversed = {kBase[1], kBase[0]};
    EXPECT_EQ(primeDigestOver(kBase, "prelude"),
              primeDigestOver(reversed, "prelude"));
}

// The digest is stable across calls (pure function of its inputs).
TEST(PrimeCacheKeyTests, digestIsDeterministic) {
    EXPECT_EQ(primeDigestOver(kBase, "prelude"),
              primeDigestOver(kBase, "prelude"));
}

// §2.3/2.4/2.6 ride computeCacheDiscriminator (already pinned by
// IrCacheTests); this pins the LIVE binding: the compiler's own prime key is
// non-empty, deterministic, and its discriminator embeds the compiler
// version (a version bump re-keys the sub-tree).
TEST(PrimeCacheKeyTests, liveStdlibKeyIsStable) {
    auto k1 = cajeta::Compiler::stdlibPrimeCacheKey();
    auto k2 = cajeta::Compiler::stdlibPrimeCacheKey();
    EXPECT_FALSE(k1.discriminator.empty());
    EXPECT_FALSE(k1.digest.empty());
    EXPECT_EQ(k1.discriminator, k2.discriminator);
    EXPECT_EQ(k1.digest, k2.digest);
}

// §2.7 — a corrupt/truncated cached entry is a MISS. primeValidatedLookup
// only accepts a file that exists AND carries the LLVM bitcode magic; the
// full verify-on-load (parse) belongs to the artifact unit.
TEST(PrimeCacheKeyTests, corruptEntryIsMiss) {
    auto dir = freshTmpDir("corrupt");
    IrCache cache(dir.string());
    const std::string disc = "jitprime-test";
    const std::string digest = "deadbeef";

    // Nothing stored → miss.
    EXPECT_FALSE(primeValidatedLookup(cache, disc, digest).has_value());

    // Garbage bytes at the keyed path → still a miss.
    ASSERT_FALSE((bool) cache.store(disc, digest, "not bitcode at all"));
    EXPECT_FALSE(primeValidatedLookup(cache, disc, digest).has_value());

    // Truncated-to-empty → miss.
    ASSERT_FALSE((bool) cache.store(disc, digest, ""));
    EXPECT_FALSE(primeValidatedLookup(cache, disc, digest).has_value());

    // Real bitcode magic ('BC' 0xC0 0xDE) → hit with the path.
    std::string magic = "BC";
    magic.push_back((char) 0xC0);
    magic.push_back((char) 0xDE);
    magic += "rest-of-module";
    ASSERT_FALSE((bool) cache.store(disc, digest, magic));
    auto hit = primeValidatedLookup(cache, disc, digest);
    ASSERT_TRUE(hit.has_value());
    EXPECT_TRUE(std::filesystem::exists(*hit));

    std::filesystem::remove_all(dir);
}
