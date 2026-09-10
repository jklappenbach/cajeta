// Per-source digest computation for the IR cache: a file's key is
// `H(source-bytes ⊕ sorted transitive-import digests)`, so a change anywhere in
// its import closure re-keys it. Cycle members contribute their leaf hash alone.

#pragma once

#include <llvm/Support/Error.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    class SourceDigestRegistry {
    public:
        // `sourceRoots` are searched in declared order to resolve an import's
        // dotted name to `<root>/com/foo/Bar.cajeta`. An import that resolves
        // nowhere is external, and contributes the dotted name itself.
        explicit SourceDigestRegistry(
            std::vector<std::string> sourceRoots);

        // Transitive digest for `sourcePath`. Memoized; safe to call
        // repeatedly. Returns an error only when `sourcePath` is
        // unreadable — unresolved imports are not errors.
        llvm::Expected<std::string> digestOf(const std::string& sourcePath);

        // The imports parsed out of `sourcePath`'s preamble; only the preamble
        // is scanned, not the full grammar. For tests and `cajeta info --deps`.
        std::vector<std::string> importsOf(const std::string& sourcePath);

    private:
        std::vector<std::string> sourceRoots_;
        std::map<std::string, std::string> leafCache_;
        std::map<std::string, std::string> transitiveCache_;
        std::set<std::string> visiting_;

        llvm::Expected<std::string> leafDigest(
            const std::string& sourcePath);
        std::optional<std::string> resolveImport(
            const std::string& dotted) const;
        llvm::Expected<std::string> digestRec(
            const std::string& sourcePath);
    };

} // namespace cajeta::buildtool
