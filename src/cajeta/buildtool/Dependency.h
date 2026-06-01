// Cajeta build-tool dependency + repository model.
//
// Phase 6a (this header): direct-dependency entries parsed from
// `settings.dependencies`, repository entries parsed from
// `settings.repositories`. Later slices extend:
//   - 6b: HTTP repository + Maven-compat shim
//   - 6c: Git repository, override mechanism (path / git / range)
//   - 6d: melt imports
//   - transitive resolution + MVS solver folded in throughout

#pragma once

#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>

#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // A repository entry from `settings.repositories`. Phase 6a
    // recognizes the `filesystem` type; other types parse to
    // RepositorySpec but the corresponding driver lands later.
    struct RepositorySpec {
        std::string name;
        std::string type;        // "filesystem" | "http" | "git" | "maven-compat"
        std::string url;         // for http / maven-compat
        std::string path;        // for filesystem
        int priority = 0;        // higher wins on resolution

        // Auth + advanced fields land with their respective drivers
        // in 6b / 6c.
    };

    // One declared dependency from `settings.dependencies`. Phase 6a
    // models the version-string form ("1.2.*") and the from-repo
    // shape ({"version": "...", "from": "<repo-name>"}). Path / Git
    // dependency sources land in 6c.
    struct DependencySpec {
        std::string name;            // e.g. "cajeta.io.net.http"
        std::string versionConstraint;  // semver constraint string
        std::optional<std::string> fromRepo;  // optional repository pin
    };

    // One entry in `settings.overrides` (Phase 6b). Forces a
    // specific resolution for a TRANSITIVE dependency — a direct
    // root dep of the same name still wins, per the spec.
    //
    // Two shapes today:
    //   - Version pin / range:        "acme.lib": "1.2.5"
    //                                "acme.lib": ">=1.2.0,<2.0.0"
    //   - Object form with allow-major-downgrade:
    //       "acme.lib": { "version": "1.2.5",
    //                     "allow-major-downgrade": true }
    //
    // Path/Git replacement (`{ "path": "..." }` / `{ "git": ... }`)
    // parses to an OverrideSpec with `path` / `git` populated but
    // is rejected at the resolver boundary with a clear "Phase 6c"
    // message — same pattern as the Phase 6a HTTP/Git
    // repository-type stubs.
    struct OverrideSpec {
        std::string name;
        std::string versionConstraint;       // empty when path/git form
        std::optional<std::string> path;      // 6c
        std::optional<std::string> git;       // 6c
        std::optional<std::string> rev;       // 6c (companion to git)
        bool allowMajorDowngrade = false;
    };

    // Result of resolving one declared dep. Phase 6a only fills in
    // the direct-dep fields; transitive expansion + version
    // negotiation arrive with the MVS solver.
    struct ResolvedDependency {
        std::string name;
        std::string version;          // the concrete chosen version
        std::string resolvedFromRepo; // repository name that supplied it
        std::string artifactPath;     // absolute path to the cached .cja
        std::string sha256;            // "sha256:<hex>"
    };

    // Parse `settings.repositories` array. Returns the spec list
    // in priority-descending order (ties broken by declaration order
    // so the first-listed wins among equals — matches the
    // "first repo that has the artifact wins" resolution rule).
    llvm::Expected<std::vector<RepositorySpec>> parseRepositories(
        const Manifest& m);

    // Parse `settings.dependencies` block. Returns the declared
    // entries in declaration order. Phase 6a accepts:
    //   { "name@kebab": "1.2.*" }
    //   { "name@kebab": { "version": "1.2.*", "from": "repo-name" } }
    // The `path` and `git` shapes are recognized but parsed into
    // DependencySpec stubs with the version constraint empty for
    // now — the corresponding sources land in 6c.
    llvm::Expected<std::vector<DependencySpec>> parseDependencies(
        const Manifest& m);

    // Parse `settings.overrides` block. Returns the declared
    // entries in declaration order. Accepted shapes:
    //   { "name@kebab": "1.2.5" }
    //   { "name@kebab": { "version": "1.2.5",
    //                     "allow-major-downgrade": true } }
    //   { "name@kebab": { "path": "./vendor/..." } }       // 6c
    //   { "name@kebab": { "git": "...", "rev": "..." } }    // 6c
    llvm::Expected<std::vector<OverrideSpec>> parseOverrides(
        const Manifest& m);

} // namespace cajeta::buildtool
