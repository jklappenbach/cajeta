// Edit operations on a manifest's JSONC source text. The functions
// preserve the source verbatim except at the edited location —
// comments, trailing commas, and original formatting are kept
// intact (this is why we don't round-trip through llvm::json::Value,
// which would normalize and lose the original layout).
//
// Each operation re-validates the result through `loadManifestString`
// before returning, so callers never end up with a manifest that
// doesn't parse.
//
// Used by the `cajeta add` / `cajeta remove` CLI subcommands. The
// manifest-side rewrite path mirrors `VersionAction::rewriteVersionInPlace`
// (regex-targeted, same constraint: canonical manifest shape).

#pragma once

#include <llvm/Support/Error.h>

#include <string>

namespace cajeta::buildtool {

    // Insert (or replace) `settings.dependencies.<name>` with the
    // given constraint string in the manifest's JSONC source. If
    // the entry already exists, its constraint is updated in-place;
    // otherwise the entry is appended to `settings.dependencies`.
    //
    // Missing intermediate blocks are created:
    //   - no `settings` block          → adds `"settings": { ... }`
    //   - no `dependencies` subobject  → adds it under `settings`
    //
    // Returns the rewritten source. Errors when the input doesn't
    // parse as a valid manifest, or when the rewrite would produce
    // an invalid manifest.
    llvm::Expected<std::string> addDependencyToManifest(
        const std::string& source,
        const std::string& name,
        const std::string& versionConstraint);

    // Remove `settings.dependencies.<name>` from the manifest.
    // Errors when the dep isn't declared. Leaves the surrounding
    // `dependencies` block intact (even if empty as a result).
    llvm::Expected<std::string> removeDependencyFromManifest(
        const std::string& source,
        const std::string& name);

} // namespace cajeta::buildtool
