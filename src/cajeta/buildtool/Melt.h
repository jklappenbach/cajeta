// Cajeta build-tool melt model: the typed `melt.*` block of a
// melt-publishing package, and a consumer's pinned `settings.melts`
// entries. Cross-package resolution lives in MeltResolver.{h,cpp}.

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

    // One `settings.melts` reference into a melt package; the spec
    // mandates a concrete `name@version` pin, never a range.
    struct MeltImport {
        std::string name;     // package name (e.g. "com.example.platform-melt")
        std::string version;  // concrete version (e.g. "2024.1.0")
    };

    // The typed payload of a melt package's `melt` block: the four
    // exported surfaces plus the transitive `melts` array. Surfaces
    // the spec marks non-exportable are rejected at melt-load time.
    struct Melt {
        // Constraint strings by package name — exact, wildcard or range.
        std::map<std::string, std::string> dependencies;

        std::map<std::string, std::string> properties;

        // Raw action presets; a consumer action of the same name shadows one.
        llvm::json::Object actionsRaw;

        // Appended to the consumer's resolution list, `priority` honored.
        std::vector<RepositorySpec> repositories;

        std::vector<MeltImport> melts;
    };

    // Parses the manifest's `melt` block, empty when absent (the caller
    // checks `manifest.hasMelt` to disambiguate). Rejects the fields the
    // spec marks non-exportable: plugins, capabilities, tasks, flavors.
    llvm::Expected<Melt> parseMelt(const Manifest& m);

    // Parses `settings.melts[]`, each entry a literal "name@version";
    // empty when the array is absent. Declaration order is preserved —
    // the resolver uses it for last-write-wins on constraint conflicts.
    llvm::Expected<std::vector<MeltImport>> parseSettingsMelts(
        const Manifest& m);

    // Parses one `name@version` reference, as `cajeta upgrade --melt` does.
    llvm::Expected<MeltImport> parseMeltImport(const std::string& s);

    // ─── Cross-package melt resolution ───────────────────────────

    // The outcome of expanding the consumer's `settings.melts` through the
    // repository machinery, transitive expansion included: every exported
    // surface merged, plus the audit trail of which melt supplied what.
    struct MeltResolution {
        struct Resolved {
            std::string name;
            std::string version;
            std::string resolvedFromRepo;
            std::string sha256;
            std::string artifactPath;
            std::vector<MeltImport> transitiveMelts;
        };
        // Post-order over imports; the lockfile records this list verbatim.
        std::vector<Resolved> resolvedMelts;

        // Keyed by dep name; last write wins, ordered by the traversal above.
        std::map<std::string, std::string> depConstraints;
        // "<melt-name>@<melt-version>" per dep — the lockfile's `provided-by`.
        std::map<std::string, std::string> depProvidedBy;

        std::map<std::string, std::string> properties;
        std::map<std::string, std::string> propertyProvidedBy;

        llvm::json::Object actionsRaw;

        // The caller appends these and re-sorts the whole list by priority.
        std::vector<RepositorySpec> repositories;
    };

    // Resolves every melt in `settings.melts` through `repos` and `cache`,
    // expanding each melt's own `melt.melts` post-order with cycle detection.
    // Empty when absent; must run before MVS, whose inputs it supplies.
    llvm::Expected<MeltResolution> resolveMelts(
        const Manifest& m,
        const std::vector<RepositoryPtr>& repos,
        ArtifactCache& cache);

    // Rewrites `deps` in place: a `"*"` constraint is substituted from
    // melts.depConstraints (missing is a hard error) and recorded in
    // providedByOut; an explicit pin wins, and diverging warns via warningsOut.
    llvm::Error applyMeltLookups(
        std::vector<DependencySpec>& deps,
        const MeltResolution& melts,
        std::map<std::string, std::string>& providedByOut,
        std::vector<std::string>& warningsOut);

} // namespace cajeta::buildtool
