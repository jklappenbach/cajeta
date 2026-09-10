// cache-manifest-v1 — the incremental-compilation protocol. The build tool owns
// the dirty-set decision and names the slots; a discriminator that does not match
// the compiler's own means a different flag world and is ignored WHOLESALE.

#pragma once

#include "cajeta/compile/CompilerMode.h"

#include <llvm/Support/Error.h>

#include <string>
#include <utility>
#include <vector>

namespace cajeta {

    struct CacheManifestEntry {
        std::string relPath;          // source-root-relative, e.g. "test/Main.cajeta"
        bool clean = false;
        std::string bcPath;           // absolute slot for the module's .bc
        std::string obligationsPath;  // absolute slot for the obligations sidecar
        std::string objPath;          // optional; empty = no .o caching
    };

    struct CacheManifest {
        std::string discriminator;    // empty = populate mode
        std::vector<CacheManifestEntry> sources;

        bool populateMode() const { return discriminator.empty(); }

        const CacheManifestEntry* find(const std::string& relPath) const {
            for (auto& e : sources)
                if (e.relPath == relPath) return &e;
            return nullptr;
        }

        // Strict parse. Rejects a wrong version, a missing field, and populate
        // mode carrying a clean entry, which has nothing sound to read.
        static llvm::Expected<CacheManifest> load(const std::string& path);
    };

    // Every flag that can change emitted IR, as canonical (name, value) pairs.
    // Shared with the build tool so both sides derive the SAME discriminator.
    // `emit` is the mode's flag spelling; `targetTriple` the effective target.
    std::vector<std::pair<std::string, std::string>> cacheFlagPairs(
        const CompilerFlags& flags, const std::string& emit,
        const std::string& targetTriple);

} // namespace cajeta
