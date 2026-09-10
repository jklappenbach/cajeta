// Cache key and validated lookup for the JIT stdlib PRIME, which compiles the
// whole embedded table as one unit rather than per file. Layered on IrCache
// instead of being a second cache; Compiler::stdlibPrimeCacheKey() binds it.

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cajeta::buildtool {

    class IrCache;

    // Digest of a whole prime-input file set plus `preludeTag`. Pure, and sorts a
    // copy by path so caller order never matters. Bare hex, no "sha256:" prefix.
    std::string primeDigestOver(
        std::vector<std::pair<std::string, std::string>> files,
        const std::string& preludeTag);

    // IrCache::lookup with a corrupt-entry gate: the artifact path comes back only
    // if the file is readable and starts with the LLVM bitcode magic, else a miss.
    std::optional<std::string> primeValidatedLookup(
        const IrCache& cache,
        const std::string& discriminator,
        const std::string& sourceDigest);

} // namespace cajeta::buildtool
