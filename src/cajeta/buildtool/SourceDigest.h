// Per-source digest computation for the IR cache.
//
// The cache key per source file is `H(source-bytes ⊕ sorted transitive-
// import digests)`. That way any of these mutations bust the file's
// cache entry:
//
//   - the file itself changes
//   - any file it imports changes
//   - any file *those* files import changes (transitively)
//
// Import-graph cycles are broken by leaving cycle members' transitive
// component at `H(source-bytes)` — that's still correct because any
// source change inside the cycle changes its hash, which cascades to
// downstream files outside the cycle.
//
// What this module does NOT do:
//   - Parse the full cajeta grammar. Just enough preamble scanning to
//     pull `import X;` and `import X.*;` lines.
//   - Cache anything to disk. The cache is held in-memory per
//     SourceDigestRegistry instance — one instance per build action
//     invocation is enough.

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
        // `sourceRoots` are searched in declared order to resolve an
        // import's dotted name (`com.foo.Bar` → first hit among
        // `<root>/com/foo/Bar.cajeta`). Imports that don't resolve
        // are treated as external (stdlib or dep) — their digest
        // contribution is the dotted name itself, so referencing a
        // different stdlib version (via the manifest's resolved
        // graph) still re-keys the cache once that change makes it
        // into the discriminator.
        explicit SourceDigestRegistry(
            std::vector<std::string> sourceRoots);

        // Transitive digest for `sourcePath`. Memoized; safe to call
        // repeatedly. Returns an error only when `sourcePath` is
        // unreadable — unresolved imports are not errors.
        llvm::Expected<std::string> digestOf(const std::string& sourcePath);

        // The set of imports parsed out of `sourcePath`'s preamble.
        // Exposed for tests + diagnostics (`cajeta info --deps`).
        std::vector<std::string> importsOf(const std::string& sourcePath);

    private:
        std::vector<std::string> sourceRoots_;
        // Per-path memo of `H(source-bytes)` — the leaf digest.
        std::map<std::string, std::string> leafCache_;
        // Per-path memo of the full transitive digest.
        std::map<std::string, std::string> transitiveCache_;
        // Recursion guard for cycle detection.
        std::set<std::string> visiting_;

        llvm::Expected<std::string> leafDigest(
            const std::string& sourcePath);
        std::optional<std::string> resolveImport(
            const std::string& dotted) const;
        llvm::Expected<std::string> digestRec(
            const std::string& sourcePath);
    };

} // namespace cajeta::buildtool
