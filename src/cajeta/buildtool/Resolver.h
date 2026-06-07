// Cajeta build-tool dependency resolver.
//
// Phase 6a: direct-dep resolution (no transitive expansion, no
// MVS conflict resolver yet). Each declared dependency is
// matched against the priority-ordered repository list; the
// first repo that has a satisfying version wins. The artifact
// is fetched into the local cache and returned as a
// ResolvedDependency.
//
// Phase 6b folds in transitive expansion + MVS (lowest version
// satisfying all constraints). Phase 6c adds path/git overrides.

#pragma once

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/Repository.h"

#include <llvm/Support/Error.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // Counters + accumulated wall-clock for one resolver run. When a
    // pointer is passed to `resolveProjectDependencies` (and through
    // to `resolveMvs`), each repository call records into here via
    // the `TimingRepository` wrapper installed by the orchestrator.
    //
    // Used by `cajeta info --resolve-time` to surface pathological
    // graphs (e.g. an MVS fixed-point that re-picks many times, or a
    // repo whose `listVersions` is slow).
    struct ResolverTimings {
        using Duration = std::chrono::microseconds;
        Duration total{0};
        Duration listVersions{0};
        Duration fetch{0};
        Duration fetchManifest{0};
        int listVersionsCalls = 0;
        int fetchCalls = 0;
        int fetchManifestCalls = 0;
        // Number of fixed-point iterations the MVS solver ran. Each
        // iteration is one full pass over the package set picking
        // dirty packages; bumping above ~deps_count signals
        // re-pick churn.
        int mvsIterations = 0;
        int depsResolved = 0;
    };

    // Resolve every declared dependency. Inputs are the
    // priority-ordered repository drivers, the declared deps,
    // and a cache instance. Returns one ResolvedDependency per
    // dep in declaration order.
    //
    // Constraint matching (Phase 6a + 6b):
    //   - Exact      "1.2.3"             release-only equality
    //   - Wildcard   "1.2.*", "1.*", "*" prefix-of-segments match
    //   - Range      ">=1.2.0", "<2.0.0", ">1.0", "<=3", "=1.2.3"
    //   - AND-combo  ">=1.2.0,<2.0.0"     comma-separated, all must hold
    //
    // The MVS solver (also 6b) layers on top: gather constraints
    // across the dependency graph, then pick the lowest version
    // satisfying every gathered constraint.
    llvm::Expected<std::vector<ResolvedDependency>> resolveDirect(
        const std::vector<DependencySpec>& deps,
        const std::vector<RepositoryPtr>& repos,
        ArtifactCache& cache);

    // Transitive resolution with Minimum-Version-Selection
    // (Phase 6b). Gathers every constraint declared for each
    // package across the dependency graph and picks the LOWEST
    // version satisfying all of them. If a newly-discovered
    // child constraint excludes a previously-picked version,
    // the package is re-picked (still lowest-satisfying) and
    // its children re-walked. Iterates to a fixed point.
    //
    // Properties:
    //   - Deterministic: same inputs → same picks (constraint
    //     gathering order is the BFS visitation order, but the
    //     final pick depends only on the constraint set).
    //   - Terminating: each re-pick can only raise a package's
    //     selected version (constraints only ever get added /
    //     tightened), and the version set is finite.
    //   - Conflicts surface as a clear "no version of X satisfies
    //     [<atom>, <atom>, ...]" error citing every contributing
    //     constraint atom.
    //
    // Per-package version picker: lowest version that satisfies
    // EVERY collected constraint, walked repo-priority-first
    // (first repo with any satisfying version wins; within that
    // repo the lowest satisfying version is picked). The `from`
    // pin restricts to one repo; conflicting `from` pins for the
    // same package error out.
    //
    // Output order is topological: root deps in declaration
    // order first, then each dep's transitive children in
    // declaration order.
    //
    // A dep whose repository can't produce a manifest sidecar
    // (`fetchManifestJson` returns nullopt) is treated as a leaf —
    // i.e. assumed to have no further dependencies. This is the
    // backwards-compatible path for archives that pre-date the
    // sidecar convention.
    llvm::Expected<std::vector<ResolvedDependency>> resolveMvs(
        const std::vector<DependencySpec>& deps,
        const std::vector<RepositoryPtr>& repos,
        ArtifactCache& cache,
        const std::vector<OverrideSpec>& overrides = {},
        ResolverTimings* timings = nullptr,
        // Stage directory for git-replacement overrides (Phase 6c).
        // The resolver clones each git override into this directory
        // on demand. Empty when no git overrides are declared (the
        // pick step errors clearly if one is encountered with an
        // empty stage dir).
        const std::string& gitOverrideStageDir = "");

    // Compare just the major-version component of two semver
    // strings. Returns <0 if a's major is lower, 0 if equal, >0
    // if higher. Exposed for the override / major-downgrade
    // guard tests; the resolver uses it to detect when applying
    // an override drops a package below the major version a
    // transitive needed.
    int compareMajor(const std::string& a, const std::string& b);

    // Test whether `version` satisfies `constraint`. Exposed for
    // unit tests.
    bool versionSatisfies(const std::string& version,
                          const std::string& constraint);

    // Compare two semver-shape strings: <0 if a<b, 0 if equal,
    // >0 if a>b. Numeric components compared as integers;
    // non-numeric prerelease tags compared lexicographically.
    // Exposed for unit tests.
    int compareVersions(const std::string& a, const std::string& b);

    // Top-level orchestration helper: pulls `settings.repositories`,
    // `settings.dependencies`, and `settings.overrides` from the
    // manifest; builds the repository drivers; constructs an
    // ArtifactCache rooted at `projectRoot`; runs `resolveMvs`.
    //
    // Returns an empty vector when no dependencies are declared
    // (so BuildAction can skip the `--classpath` step entirely
    // for projects without external deps).
    //
    // `projectRoot` is where the local artifact cache lives
    // (`<projectRoot>/.cajeta/cache/artifacts/<sha256>.cja`).
    // `homeOverride` lets tests pin the workstation cache root
    // away from $HOME; production callers leave it unset.
    llvm::Expected<std::vector<ResolvedDependency>>
    resolveProjectDependencies(
        const Manifest& m,
        const std::string& projectRoot,
        std::optional<std::string> homeOverride = std::nullopt,
        ResolverTimings* timings = nullptr);

} // namespace cajeta::buildtool
