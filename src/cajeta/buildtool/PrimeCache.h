// Persistent stdlib-PRIME cache key + validated lookup (compile-cache spec
// §2/§4; the JIT/test prime path, not `cajeta build`).
//
// The build-tool IrCache/SourceDigest pair is per-FILE and disk-path
// oriented; the JIT prime compiles the EMBEDDED stdlib table
// (cajeta::stdlib::g_files) as one unit. This module adds the two pieces
// the prime needs on top of IrCache, instead of a second cache (§4.4):
//
//   - primeDigestOver: content digest of a whole (path, bytes) file set +
//     a prelude tag. Sorted by path, so table order never matters; every
//     byte and every path participates; the tag folds the eager/lazy
//     package split (§2.5 — the prelude decides what the prime compiles).
//     The embedded table is the full transitive closure, so the per-file
//     transitive-import machinery of SourceDigest is unnecessary here.
//
//   - primeValidatedLookup: IrCache::lookup + the §2.7 cheap gate — a
//     hit must exist AND start with the LLVM bitcode magic, else it is
//     reported as a MISS (a killed writer / corrupt entry can never be
//     loaded). Full verify-on-load (parsing the module) belongs to the
//     artifact loader (plan Unit 3), not the key layer.
//
// The live binding of these to the embedded stdlib + compiler version +
// stdlib-affecting flags is Compiler::stdlibPrimeCacheKey() (the compiler
// owns those inputs).

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cajeta::buildtool {

    class IrCache;

    // Digest of a whole prime-input file set. Pure: same inputs → same
    // digest; sorts a copy of `files` by path so caller order is
    // irrelevant. Returns a bare hex string (no "sha256:" prefix) usable
    // as an IrCache sourceDigest.
    std::string primeDigestOver(
        std::vector<std::pair<std::string, std::string>> files,
        const std::string& preludeTag);

    // IrCache::lookup with the §2.7 corrupt-entry gate: returns the cached
    // artifact path only when the file exists, is readable, and begins
    // with the LLVM bitcode magic bytes; anything else is a miss.
    std::optional<std::string> primeValidatedLookup(
        const IrCache& cache,
        const std::string& discriminator,
        const std::string& sourceDigest);

} // namespace cajeta::buildtool
