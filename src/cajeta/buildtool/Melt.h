// Cajeta build-tool melt model — Phase 6c.
//
// A melt is a curated bundle of dependency versions, properties,
// action presets, and repositories that consumers import as one
// unit. See cajeta-docs/BuildTool.md "Melts" for the spec.
//
// This header models:
//   - The typed `melt.*` block of a melt-publishing package.
//   - One `settings.melts[*]` entry from a consumer (pinned
//     `name@version` reference into a repository).
//
// The cross-package resolution (clone-the-melt, merge constraints,
// transitive expansion) lives in MeltResolver.{h,cpp}.

#pragma once

#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <map>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // One reference into a melt package from `settings.melts`.
    // The spec mandates a concrete `name@version` pin (no ranges)
    // so consumers reproducibly snap to a specific curation.
    struct MeltImport {
        std::string name;     // package name (e.g. "com.example.platform-melt")
        std::string version;  // concrete version (e.g. "2024.1.0")
    };

    // The typed payload of a melt package's `melt` block. Carries
    // the four exported surfaces (deps / props / actions / repos)
    // plus the optional transitive `melts` array.
    //
    // Fields the spec marks as NON-exportable from melts (plugins,
    // capabilities, tasks, settings.build flavors) are not modeled
    // here; the validator rejects them at melt-load time.
    struct Melt {
        // Curated version constraints. Keyed by package name.
        // Values are constraint strings — exact, wildcard, or range
        // forms (the same surface a consumer's
        // `settings.dependencies` accepts).
        std::map<std::string, std::string> dependencies;

        // Shared properties to merge into the consumer's property
        // table (inert-inherits: shadowed by consumer's own).
        std::map<std::string, std::string> properties;

        // Action presets — kept raw (same shape as the top-level
        // `actions` block). Consumer's own action with the same name
        // shadows the melt's preset.
        llvm::json::Object actionsRaw;

        // Additional repositories to append to the consumer's
        // resolution list. Honors `priority` exactly like a
        // consumer-declared repository.
        std::vector<RepositorySpec> repositories;

        // Transitive melt imports. Concrete `name@version` pins;
        // resolved post-order with cycle detection.
        std::vector<MeltImport> melts;
    };

    // Parse the manifest's `melt` block into the typed Melt model.
    // Returns an empty Melt with empty fields when the block is
    // absent (caller checks `manifest.hasMelt` to disambiguate).
    //
    // Rejects fields the spec marks as non-exportable
    // (plugins, capabilities, tasks, settings.build flavor knobs).
    llvm::Expected<Melt> parseMelt(const Manifest& m);

    // Parse `settings.melts[]` from the manifest. Each entry must be
    // a literal "name@version" string. Returns an empty list when
    // the array is absent. Declaration order is preserved — the
    // resolver uses it for last-write-wins on constraint conflicts.
    llvm::Expected<std::vector<MeltImport>> parseSettingsMelts(
        const Manifest& m);

    // Parse a single `name@version` reference string. Useful for
    // CLI flag parsing (`cajeta upgrade --melt name@version`).
    llvm::Expected<MeltImport> parseMeltImport(const std::string& s);

} // namespace cajeta::buildtool
