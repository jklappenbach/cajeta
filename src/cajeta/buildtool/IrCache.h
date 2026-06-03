// Per-file IR cache for `cajeta build`.
//
// Cache shape:
//
//   .cajeta/cache/ir/<discriminator>/<source-digest>.bc
//
//   - `discriminator` canonicalizes (compiler-version, sorted flag-set)
//     into a stable string. Different flavors / targets / profile
//     combinations land in different sub-trees so flipping flavor
//     doesn't bust the rest of the cache.
//   - `source-digest` is `sha256(source-bytes + sorted-transitive-
//     imports-digest)` so anything that affects the compilation
//     output for that source file changes the key.
//
// Acceptance criteria pinned (Phase 5b plan):
//   - Flag-set order doesn't bust the cache (discriminator sorts).
//   - Rebuild after eviction is byte-identical (atomic store +
//     content-addressed key).
//   - Cache size cap enforces eviction (evict() prunes LRU).
//
// What this header does NOT do:
//   - Drive the compiler. The build action calls `lookup()` before
//     compiling a source file and `store()` after. The compiler
//     side of "skip files we already have" is its own integration.

#pragma once

#include <llvm/Support/Error.h>

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // The compiler version + flag-set get canonicalized into a
    // single discriminator string. Same compiler + same flags →
    // same discriminator → same cache sub-tree.
    //
    // `flags` is taken by-value because we sort a copy; callers
    // don't have to worry about ordering at the call site.
    std::string computeCacheDiscriminator(
        const std::string& compilerVersion,
        std::vector<std::pair<std::string, std::string>> flags);

    // The per-file IR cache.
    class IrCache {
    public:
        // `rootDir` is typically `<project>/.cajeta/cache/ir`. The
        // cache creates per-discriminator sub-directories on demand.
        explicit IrCache(std::string rootDir);

        // Where the cached `.bc` for this (discriminator, sourceDigest)
        // pair lives, regardless of whether anything's at that path
        // yet. Used by the build action to point the compiler at the
        // intended output location (store-on-success).
        std::string keyFor(const std::string& discriminator,
                           const std::string& sourceDigest) const;

        // Cache hit → returns the absolute path to the cached `.bc`.
        // Miss → returns empty. lookup() does NOT update an atime
        // counter — the kernel's filesystem atime handles that.
        std::optional<std::string> lookup(
            const std::string& discriminator,
            const std::string& sourceDigest) const;

        // Atomic write: temp-file in the same directory, fsync, rename.
        // Returns an error on I/O failure; caller surfaces it as a
        // build failure (cache write errors should be loud — silently
        // failing turns a "what's wrong with my build?" into "why is
        // my CI slow?" later).
        llvm::Error store(const std::string& discriminator,
                          const std::string& sourceDigest,
                          const std::string& bytes) const;

        // Wipe the entire cache. Used by `cajeta clean --deep`.
        // Returns the count of files removed.
        llvm::Expected<int> wipe() const;

        // Prune entries to fit within `maxBytes` (LRU by access time)
        // and to drop anything older than `maxAge`. Either parameter
        // can be zero to skip that pass. Returns the count of files
        // removed. Idempotent.
        struct EvictionPolicy {
            // 0 = no size cap.
            uint64_t maxBytes = 0;
            // Zero = no TTL.
            std::chrono::seconds maxAge{0};
        };
        llvm::Expected<int> evict(EvictionPolicy policy) const;

        // Total cache size in bytes (sum of file sizes across all
        // discriminator sub-dirs). Used by tests and by future
        // `cajeta info --cache` output.
        llvm::Expected<uint64_t> sizeBytes() const;

        const std::string& rootDir() const { return rootDir_; }

    private:
        std::string rootDir_;
    };

} // namespace cajeta::buildtool
