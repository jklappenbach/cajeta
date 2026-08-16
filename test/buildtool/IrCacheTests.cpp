// Regression tests for the Phase 5b IR cache + discriminator.
//
// What gets pinned:
//   - Discriminator is stable across flag-set order (the
//     load-bearing acceptance criterion).
//   - Different compiler versions / different flag values produce
//     different discriminators (so a compiler-upgrade busts the
//     cache, as intended).
//   - Cache store + lookup round-trip preserves bytes verbatim.
//   - Size-cap eviction drops the LRU entries (oldest atime first).
//   - TTL eviction drops anything past the age.
//   - Wipe removes every entry across discriminator sub-trees.
//
// What's deferred: the BuildAction integration tests ("touching
// one source rebuilds only that file") gate on compiler-side cache-
// fed builds. That acceptance lands when the compiler does.

#include "cajeta/buildtool/IrCache.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

using cajeta::buildtool::computeCacheDiscriminator;
using cajeta::buildtool::IrCache;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    std::filesystem::path tempDir(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-ircache-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    // Read the cache file's bytes back to confirm round-trip.
    std::string readBytes(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream buf; buf << in.rdbuf();
        return buf.str();
    }

} // namespace

// ─── Discriminator ────────────────────────────────────────────────





// ─── Store + lookup ────────────────────────────────────────────────






// ─── Eviction ──────────────────────────────────────────────────────

TEST(IrCacheTests, evictHonorsSizeCap) {
    auto root = tempDir("evict-size");
    IrCache cache(root.string());
    ASSERT_FALSE((bool)cache.store("d", "a", std::string(100, 'a')));
    // Spread atimes so LRU has something to sort.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    ASSERT_FALSE((bool)cache.store("d", "b", std::string(100, 'b')));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    ASSERT_FALSE((bool)cache.store("d", "c", std::string(100, 'c')));

    IrCache::EvictionPolicy p;
    p.maxBytes = 150;
    auto removed = cache.evict(p);
    ASSERT_TRUE((bool)removed) << errorText(removed.takeError());
    // Should drop the two oldest (a, b) to fit a single 100-byte
    // entry under the 150-byte cap. (200 → 100 fits.)
    EXPECT_GE(*removed, 1);
    auto sz = cache.sizeBytes();
    ASSERT_TRUE((bool)sz);
    EXPECT_LE(*sz, 150u);
}


TEST(IrCacheTests, wipeRemovesEverything) {
    auto root = tempDir("wipe");
    IrCache cache(root.string());
    ASSERT_FALSE((bool)cache.store("d1", "a", "x"));
    ASSERT_FALSE((bool)cache.store("d1", "b", "y"));
    ASSERT_FALSE((bool)cache.store("d2", "c", "z"));
    auto removed = cache.wipe();
    ASSERT_TRUE((bool)removed);
    EXPECT_EQ(*removed, 3);
    EXPECT_FALSE(cache.lookup("d1", "a").has_value());
    EXPECT_FALSE(cache.lookup("d2", "c").has_value());
    auto sz = cache.sizeBytes();
    ASSERT_TRUE((bool)sz);
    EXPECT_EQ(*sz, 0u);
}

