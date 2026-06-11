// Cajeta build-tool melt model — Phase 6c.
//
// A melt is a curated bundle of dependency versions, properties,
// action presets, and repositories that consumers import as one
// unit. See docs/BuildTool.md "Melts" for the spec.
//
// This header models:
//   - The typed `melt.*` block of a melt-publishing package.
//   - One `settings.melts[*]` entry from a consumer (pinned
//     `name@version` reference into a repository).
//
// The cross-package resolution (clone-the-melt, merge constraints,
// transitive expansion) lives in MeltResolver.{h,cpp}.

#pragma once

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Repository.h"

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

    // ─── Cross-package melt resolution ───────────────────────────

    // The outcome of expanding the consumer manifest's `settings.melts`
    // through the repository machinery (including post-order
    // transitive expansion). Aggregates each exported surface plus
    // the audit trail (which melt supplied which constraint).
    struct MeltResolution {
        // One resolved melt — the artifact, who supplied it, and
        // its direct transitive declarations.
        struct Resolved {
            std::string name;
            std::string version;
            std::string resolvedFromRepo;
            std::string sha256;
            std::string artifactPath;
            std::vector<MeltImport> transitiveMelts;
        };
        // Post-order over all imports, declaration-order across.
        // The lockfile records this list verbatim.
        std::vector<Resolved> resolvedMelts;

        // Per-dep constraint table. Keyed by dep name. Last write
        // wins on conflict — driven by the post-order traversal +
        // declaration order at the consumer.
        std::map<std::string, std::string> depConstraints;
        // Per-dep audit: "<melt-name>@<melt-version>" that supplied
        // the constraint above. Used by the lockfile's `provided-by`
        // field.
        std::map<std::string, std::string> depProvidedBy;

        // Merged properties. Consumer-side shadowing is applied by
        // the properties layer; this struct just holds the union.
        std::map<std::string, std::string> properties;
        std::map<std::string, std::string> propertyProvidedBy;

        // Merged action presets. Raw — consumer's same-named
        // entries shadow these at the actions-resolution layer.
        llvm::json::Object actionsRaw;

        // Repositories contributed by all imported melts (post-order).
        // The caller appends these to the consumer's repository list
        // and re-sorts by priority.
        std::vector<RepositorySpec> repositories;
    };

    // Resolve every melt declared in the consumer manifest's
    // `settings.melts`. Each import is fetched through the
    // repository machinery; the melt's own `melt.melts` are
    // expanded post-order with cycle detection (a melt that
    // transitively imports itself fails with a cycle citation).
    //
    // Returns an empty MeltResolution when `settings.melts` is
    // absent. The caller uses the returned `depConstraints` to
    // substitute `"*"` version sentinels and appends `repositories`
    // to the resolution list before running MVS.
    llvm::Expected<MeltResolution> resolveMelts(
        const Manifest& m,
        const std::vector<RepositoryPtr>& repos,
        ArtifactCache& cache);

    // Walk `deps` in-place. Two cases:
    //
    //   - `"*"` constraint        — substituted from melts.depConstraints.
    //                                Missing entry is a hard error.
    //   - explicit constraint     — kept verbatim; if a melt curates
    //                                the SAME dep at a different
    //                                version, that's a divergence
    //                                (consumer's explicit pin wins)
    //                                and one warning per occurrence
    //                                is appended to `warningsOut`.
    //
    // Each substitution is recorded in `providedByOut` (dep name →
    // "melt-name@melt-version"), letting downstream callers populate
    // the lockfile's `provided-by` field. Divergence warnings are
    // for the operator's CLI output — they don't change resolution
    // behavior, they just surface that the consumer chose to ignore
    // curated guidance.
    llvm::Error applyMeltLookups(
        std::vector<DependencySpec>& deps,
        const MeltResolution& melts,
        std::map<std::string, std::string>& providedByOut,
        std::vector<std::string>& warningsOut);

} // namespace cajeta::buildtool
