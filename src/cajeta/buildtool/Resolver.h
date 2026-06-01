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

#include <string>
#include <vector>

namespace cajeta::buildtool {

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

    // Test whether `version` satisfies `constraint`. Exposed for
    // unit tests.
    bool versionSatisfies(const std::string& version,
                          const std::string& constraint);

    // Compare two semver-shape strings: <0 if a<b, 0 if equal,
    // >0 if a>b. Numeric components compared as integers;
    // non-numeric prerelease tags compared lexicographically.
    // Exposed for unit tests.
    int compareVersions(const std::string& a, const std::string& b);

} // namespace cajeta::buildtool
