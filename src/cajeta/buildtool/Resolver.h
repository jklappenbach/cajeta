// Cajeta build-tool dependency resolver: each declared dependency is matched
// against the priority-ordered repositories, the first repo with a satisfying
// version winning, then fetched into the local cache as a ResolvedDependency.

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

    // Counters + wall-clock for one resolver run, recorded by the
    // `TimingRepository` wrapper when the orchestrator is passed one of these.
    struct ResolverTimings {
        using Duration = std::chrono::microseconds;
        Duration total{0};
        Duration listVersions{0};
        Duration fetch{0};
        Duration fetchManifest{0};
        int listVersionsCalls = 0;
        int fetchCalls = 0;
        int fetchManifestCalls = 0;
        // One pass over the dirty packages each; far above the dep count is churn.
        int mvsIterations = 0;
        int depsResolved = 0;
    };

    // Resolves the declared deps directly, without transitive expansion: one
    // ResolvedDependency per dep in declaration order. A constraint is exact
    // ("1.2.3"), a wildcard ("1.2.*"), a range (">=1.2.0"), or a comma-AND.
    llvm::Expected<std::vector<ResolvedDependency>> resolveDirect(
        const std::vector<DependencySpec>& deps,
        const std::vector<RepositoryPtr>& repos,
        ArtifactCache& cache);

    // Transitive resolution by minimum-version selection, iterated to a fixed
    // point; output order is topological. A dep whose repo yields no manifest
    // sidecar is a leaf, which is how pre-sidecar archives still resolve.
    llvm::Expected<ResolvedGraph> resolveMvsGraph(
        const std::vector<DependencySpec>& deps,
        const std::vector<RepositoryPtr>& repos,
        ArtifactCache& cache,
        const std::vector<OverrideSpec>& overrides = {},
        ResolverTimings* timings = nullptr,
        const std::string& gitOverrideStageDir = "",
        const std::string& ollaWriteThroughRoot = "");

    // The same solve returning only `.packages`. `gitOverrideStageDir` is where
    // git overrides are cloned on demand (empty when none are declared) and
    // `ollaWriteThroughRoot`, when set, mirrors remote fetches into ~/.olla.
    llvm::Expected<std::vector<ResolvedDependency>> resolveMvs(
        const std::vector<DependencySpec>& deps,
        const std::vector<RepositoryPtr>& repos,
        ArtifactCache& cache,
        const std::vector<OverrideSpec>& overrides = {},
        ResolverTimings* timings = nullptr,
        const std::string& gitOverrideStageDir = "",
        const std::string& ollaWriteThroughRoot = "");

    // Orders two semver strings by major component alone, so the resolver can
    // catch an override dropping a package below the major a transitive needs.
    int compareMajor(const std::string& a, const std::string& b);

    bool versionSatisfies(const std::string& version,
                          const std::string& constraint);

    // Orders two semver-shape strings: numeric components as integers, other
    // prerelease tags lexicographically.
    int compareVersions(const std::string& a, const std::string& b);

    // Runs the whole resolve off a manifest: repositories, dependencies and
    // overrides from `settings`, an ArtifactCache under `projectRoot`, then
    // resolveMvs. `homeOverride` pins the workstation cache root for tests.
    llvm::Expected<std::vector<ResolvedDependency>>
    resolveProjectDependencies(
        const Manifest& m,
        const std::string& projectRoot,
        std::optional<std::string> homeOverride = std::nullopt,
        ResolverTimings* timings = nullptr);

    // The same resolution returning the graph; `.packages` is byte-for-byte the
    // resolveProjectDependencies result, and an empty graph means no deps.
    llvm::Expected<ResolvedGraph>
    resolveProjectGraph(
        const Manifest& m,
        const std::string& projectRoot,
        std::optional<std::string> homeOverride = std::nullopt,
        ResolverTimings* timings = nullptr);

} // namespace cajeta::buildtool
