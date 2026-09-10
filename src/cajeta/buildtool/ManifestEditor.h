// Edit operations on a manifest's JSONC source text, preserving it verbatim except
// at the edited location; round-tripping through llvm::json::Value would normalize
// the layout away. Each operation re-validates through loadManifestString.

#pragma once

#include <llvm/Support/Error.h>

#include <string>

namespace cajeta::buildtool {

    // Insert or update `settings.dependencies.<name>` with `versionConstraint`,
    // creating a missing `settings` or `dependencies` block, and return the rewritten
    // source. Errors when the input, or the result, is not a valid manifest.
    llvm::Expected<std::string> addDependencyToManifest(
        const std::string& source,
        const std::string& name,
        const std::string& versionConstraint);

    // Remove `settings.dependencies.<name>`, erroring when the dep is not declared.
    // The surrounding `dependencies` block is left in place, even if it ends empty.
    llvm::Expected<std::string> removeDependencyFromManifest(
        const std::string& source,
        const std::string& name);

    // Rewrite a `settings.melts[]` entry `"<name>@<oldVersion>"` to the new version,
    // erroring when that exact entry is absent so the caller can say "no such melt".
    // Array position, surrounding comments and unrelated formatting are preserved.
    llvm::Expected<std::string> setMeltImportInManifest(
        const std::string& source,
        const std::string& name,
        const std::string& oldVersion,
        const std::string& newVersion);

    // Append a typed exclude to `plugins.cajeta.coverage.config.exclude`, creating
    // `config`/`exclude` but NOT the plugin block itself, whose absence is an error.
    // `kind` is "file", "package" or "symbol"; (kind, pattern) duplicates are refused.
    llvm::Expected<std::string> appendCoverageExclude(
        const std::string& source,
        const std::string& kind,
        const std::string& pattern,
        const std::string& reason);

    // Remove every exclude entry whose `pattern` matches, reporting how many in `count`.
    // Errors when none match, so the user sees that rather than a silent no-op.
    struct RemoveCoverageExcludeResult {
        std::string newSource;
        int count = 0;
    };
    llvm::Expected<RemoveCoverageExcludeResult> removeCoverageExclude(
        const std::string& source,
        const std::string& pattern);

} // namespace cajeta::buildtool
