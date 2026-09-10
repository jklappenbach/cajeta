// Per-file IR cache for `cajeta build`, keyed as
// `.cajeta/cache/ir/<discriminator>/<source-digest>.bc`, where the discriminator
// separates flavors/targets and the digest covers the source plus its imports.

#pragma once

#include <llvm/Support/Error.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // Canonicalize a compiler version and flag-set into one discriminator string.
    // `flags` is by value because it is sorted here, so call-site order is free.
    std::string computeCacheDiscriminator(
        const std::string& compilerVersion,
        std::vector<std::pair<std::string, std::string>> flags);

    // The per-file IR cache.
    class IrCache {
    public:
        // `rootDir` is typically `<project>/.cajeta/cache/ir`; sub-dirs are on demand.
        explicit IrCache(std::string rootDir);

        // Where this (discriminator, sourceDigest) pair's `.bc` belongs, whether or not
        // anything is there yet, so the compiler can be pointed at it up front.
        std::string keyFor(const std::string& discriminator,
                           const std::string& sourceDigest) const;

        // The absolute path to the cached `.bc`, or empty on a miss. Recency is left
        // to the filesystem's atime; no counter is updated here.
        std::optional<std::string> lookup(
            const std::string& discriminator,
            const std::string& sourceDigest) const;

        // Atomic write: temp file in the same directory, fsync, rename. An I/O error is
        // returned rather than swallowed, so a cache write failure stays loud.
        llvm::Error store(const std::string& discriminator,
                          const std::string& sourceDigest,
                          const std::string& bytes) const;

        // Wipe the entire cache, returning the count of files removed.
        llvm::Expected<int> wipe() const;

        // Prune to fit `maxBytes` (LRU by access time) and drop anything older than
        // `maxAge`; either may be zero to skip that pass. Idempotent; returns the count.
        struct EvictionPolicy {
            uint64_t maxBytes = 0;
            std::chrono::seconds maxAge{0};
        };
        llvm::Expected<int> evict(EvictionPolicy policy) const;

        // Total cache size in bytes across all discriminator sub-dirs.
        llvm::Expected<uint64_t> sizeBytes() const;

        const std::string& rootDir() const { return rootDir_; }

    private:
        std::string rootDir_;
    };

} // namespace cajeta::buildtool
