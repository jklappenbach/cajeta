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

    // Rewrite a `settings.melts[]` entry of the form
    // `"<name>@<oldVersion>"` to `"<name>@<newVersion>"`. The melt
    // name + old version must currently appear in the array (the
    // function errors otherwise so callers can surface a clear "no
    // such melt to upgrade" message). The melt's position in the
    // array, comments around the array, and unrelated formatting
    // are preserved verbatim.
    llvm::Expected<std::string> setMeltImportInManifest(
        const std::string& source,
        const std::string& name,
        const std::string& oldVersion,
        const std::string& newVersion);

    // Append a typed exclude entry to
    // `plugins.cajeta.coverage.config.exclude`. Creates `config` /
    // `exclude` when missing; refuses to add if the
    // `plugins.cajeta.coverage` block itself isn't declared (we
    // don't silently activate the plugin). Refuses duplicates by
    // (kind, pattern) — same pattern with a different reason is
    // still a duplicate.
    //
    // `kind` must be one of "file", "package", "symbol"; this
    // function trusts the caller for the rest (the CLI surface
    // rejects generic reasons before reaching here, so the validator
    // doesn't need a second pass).
    llvm::Expected<std::string> appendCoverageExclude(
        const std::string& source,
        const std::string& kind,
        const std::string& pattern,
        const std::string& reason);

    // Remove every exclude entry whose `pattern` field matches
    // `pattern`. The `count` field reports how many entries were
    // removed so the caller can confirm the operation. Errors when
    // no entries match (so the user sees a clear "nothing to
    // remove" instead of a silent no-op).
    struct RemoveCoverageExcludeResult {
        std::string newSource;
        int count = 0;
    };
    llvm::Expected<RemoveCoverageExcludeResult> removeCoverageExclude(
        const std::string& source,
        const std::string& pattern);

} // namespace cajeta::buildtool
