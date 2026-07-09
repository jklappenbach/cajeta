// cache-manifest-v1 — the build-tool ↔ compiler incremental-compilation
// protocol (docs/specification/buildtool/IncrementalCompilation.md § protocol;
// plan Phase 3/4). The build tool owns the dirty-set decision; the compiler
// obeys it. Per source the manifest names the `.bc` + obligations slots to
// load (clean) or write (dirty), plus the cache discriminator the slots were
// keyed under.
//
// An EMPTY discriminator means populate mode: the manifest is write-only
// (every entry dirty); the compiler adopts its own discriminator and prints
// it so the caller can key subsequent manifests. A non-empty discriminator
// that doesn't match the compiler's own computed one means the slots belong
// to a different compiler/flag world: the manifest is ignored wholesale
// (warn + full rebuild) — never half-honored.

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

        // Strict parse + structural validation. Rejects: wrong/missing
        // version, missing fields, and populate mode combined with a clean
        // entry (a write-only manifest has nothing sound to read).
        static llvm::Expected<CacheManifest> load(const std::string& path);
    };

    // The canonical (name, value) flag pairs that key the IR cache — every
    // flag that can change emitted module IR. Shared with the build tool so
    // both sides compute the SAME discriminator via
    // buildtool::computeCacheDiscriminator(CAJETA_VERSION, pairs).
    // `emit` is the mode's flag spelling ("ir"/"obj"/"exe"/"cja"/"uber");
    // `targetTriple` is the compiler's effective target.
    std::vector<std::pair<std::string, std::string>> cacheFlagPairs(
        const CompilerFlags& flags, const std::string& emit,
        const std::string& targetTriple);

} // namespace cajeta
