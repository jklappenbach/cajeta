// `cajeta upgrade`: plan and apply version bumps for a manifest's direct
// dependencies and melts. "Latest" is the highest version across the configured
// repos, and the new constraint is an exact pin of the chosen version.

#pragma once

#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // The candidate sidecar's `settings.capabilities` against the resolved
    // version's; a sidecar-less artifact contributes an empty set, not a delta.
    struct CapabilityDelta {
        std::vector<std::string> added;
        std::vector<std::string> removed;
        bool empty() const { return added.empty() && removed.empty(); }
    };

    // One row per requested dep. `oldVersion` is what resolveMvs picks today
    // (empty when unresolvable); `changed` is false when the pick already
    // matches, and the row is still surfaced so the CLI can say "already at X".
    struct UpgradeEntry {
        std::string name;
        std::string oldConstraint;
        std::string oldVersion;
        std::string newVersion;
        std::string newConstraint;
        std::string resolvedFromRepo;
        CapabilityDelta capDelta;
        bool changed = false;
    };

    struct UpgradePlan {
        std::vector<UpgradeEntry> entries;
        bool anyChange() const;
        bool anyCapabilityChange() const;
    };

    // Computes the plan without writing. An empty `targetNames` means every dep
    // in `settings.dependencies`; `explicitVersions` pins a name to a version
    // instead of the highest-in-repos pick. Nothing is downloaded or edited.
    llvm::Expected<UpgradePlan> planUpgrade(
        const Manifest& m,
        const std::string& projectRoot,
        const std::vector<std::string>& targetNames,
        const std::map<std::string, std::string>& explicitVersions = {},
        std::optional<std::string> homeOverride = std::nullopt);

    // Rewrites each changed entry's `settings.dependencies.<name>` in the
    // manifest source bytes and returns the new source; unchanged rows are
    // skipped and the caller owns writing the file.
    llvm::Expected<std::string> applyUpgradePlan(
        const std::string& manifestSource,
        const UpgradePlan& plan);

    // ─── Melt upgrades ──────────────────────────────────────────

    // Old melt's `melt.dependencies` table against the new one: `added` is
    // (name, constraint), `removed` is names, `changed` is (name, old, new).
    struct MeltDependencyDelta {
        std::vector<std::pair<std::string, std::string>> added;
        std::vector<std::string> removed;
        std::vector<std::tuple<std::string, std::string, std::string>>
            changed;
        bool empty() const {
            return added.empty() && removed.empty() && changed.empty();
        }
    };

    // One row of a melt-upgrade plan; `changed` is false when the pin already
    // equals the candidate, and the row is still surfaced for the CLI's report.
    struct MeltUpgradeEntry {
        std::string name;
        std::string oldVersion;
        std::string newVersion;
        std::string resolvedFromRepo;
        MeltDependencyDelta depDelta;
        bool changed = false;
    };

    struct MeltUpgradePlan {
        std::vector<MeltUpgradeEntry> entries;
        bool anyChange() const;
    };

    // planUpgrade for melts: an empty `targetNames` means every melt in
    // `settings.melts`, a name missing from the manifest is an error, and
    // `explicitVersions` pins a melt instead of taking the highest-in-repos.
    llvm::Expected<MeltUpgradePlan> planMeltUpgrade(
        const Manifest& m,
        const std::string& projectRoot,
        const std::vector<std::string>& targetNames,
        const std::map<std::string, std::string>& explicitVersions = {},
        std::optional<std::string> homeOverride = std::nullopt);

    // Rewrites each changed entry's `"name@oldVersion"` in `settings.melts` to
    // `"name@newVersion"` and returns the new source; unchanged rows are skipped.
    llvm::Expected<std::string> applyMeltUpgradePlan(
        const std::string& manifestSource,
        const MeltUpgradePlan& plan);

} // namespace cajeta::buildtool
