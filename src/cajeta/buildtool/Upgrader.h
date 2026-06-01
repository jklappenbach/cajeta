// `cajeta upgrade` — compute an upgrade plan for a manifest's
// direct dependencies and (when accepted) rewrite the manifest's
// constraints to pin to the upgraded versions.
//
// Upgrade semantics:
//   - "Latest" is the highest version across all configured repos,
//     compared with the resolver's semver-aware `compareVersions`.
//   - The new constraint is an exact pin of the chosen version
//     (matches the `cajeta add <name>@<version>` shape; users edit
//     by hand if they want a range).
//   - "Currently resolved" is the result of `resolveMvs` on the
//     existing manifest, so the diff reflects what the build is
//     actually using today, not just the literal constraint string.
//
// Capability-change detection: each candidate's sidecar `cajeta.json`
// (`settings.capabilities`) is compared against the currently-resolved
// version's sidecar. Net-new capabilities flag the entry — the CLI
// prompts before applying. Sidecar-less repos (pre-sidecar artifacts)
// contribute an empty capability set, which means no false-positive
// flags for legacy artifacts.

#pragma once

#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // Net-new and removed capabilities for one upgrade target. Empty
    // when both old and new sidecars carry the same set (or both are
    // absent).
    struct CapabilityDelta {
        std::vector<std::string> added;
        std::vector<std::string> removed;
        bool empty() const { return added.empty() && removed.empty(); }
    };

    // One row of the upgrade plan, one per requested dep. `changed`
    // is false when the chosen new version equals the currently
    // resolved version (the row is still surfaced so the CLI can
    // report "already at <version>").
    struct UpgradeEntry {
        std::string name;
        std::string oldConstraint;
        std::string oldVersion;            // empty when unresolvable
        std::string newVersion;
        std::string newConstraint;         // what we'll write
        std::string resolvedFromRepo;
        CapabilityDelta capDelta;
        bool changed = false;
    };

    struct UpgradePlan {
        std::vector<UpgradeEntry> entries;
        bool anyChange() const;
        bool anyCapabilityChange() const;
    };

    // Compute the plan without writing anything.
    //
    // `targetNames` selects which deps to consider:
    //   - empty             → every dep in `settings.dependencies`
    //   - non-empty         → only those names (error if any is
    //                         missing from the manifest)
    //
    // `explicitVersions` lets the caller pin a specific version per
    // name (the `<name>@<version>` form). When an entry is present,
    // its version is used verbatim instead of the highest-in-repos
    // pick (still validated to exist in some repo).
    //
    // `projectRoot` anchors the artifact cache + download stage dir
    // (same convention as `resolveProjectDependencies`).
    // `homeOverride` pins the workstation cache for tests.
    llvm::Expected<UpgradePlan> planUpgrade(
        const Manifest& m,
        const std::string& projectRoot,
        const std::vector<std::string>& targetNames,
        const std::map<std::string, std::string>& explicitVersions = {},
        std::optional<std::string> homeOverride = std::nullopt);

    // Apply the plan to the manifest source bytes. Rewrites each
    // changed entry's `settings.dependencies.<name>` to the new
    // constraint via `addDependencyToManifest`. Unchanged entries
    // are skipped. Returns the rewritten source.
    llvm::Expected<std::string> applyUpgradePlan(
        const std::string& manifestSource,
        const UpgradePlan& plan);

} // namespace cajeta::buildtool
