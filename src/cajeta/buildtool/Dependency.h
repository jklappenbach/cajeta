// Cajeta build-tool dependency + repository model: the typed forms of
// `settings.dependencies`, `settings.repositories` and `settings.overrides`.

#pragma once

#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    /// `settings.repositories[*].auth`: a bearer token, named by env var or
    /// (discouraged) given literally, or an mTLS cert/key pair with an
    /// optional CA pin. See BuildTool.md "HTTP repository / Auth".
    struct RepositoryAuth {
        std::string type;                          // "bearer" | "mtls" | ""
        // Bearer:
        std::string tokenEnv;                      // env var name
        std::string tokenLiteral;                  // discouraged but supported
        // mTLS:
        std::string clientCertPath;
        std::string clientKeyPath;
        std::string caCertPath;                    // optional (server CA pin)
    };

    /// One `settings.repositories` entry. Maven-compat parses, but its driver
    /// is not implemented.
    struct RepositorySpec {
        std::string name;
        std::string type;        // "filesystem" | "http" | "git" | "maven-compat"
        std::string url;         // clone URL (git) / base URL (http / maven-compat)
        std::string path;        // for filesystem
        int priority = 0;        // higher wins on resolution
        RepositoryAuth auth;     // bearer / mtls (HTTP only)
        // `gitRef` is checked out literally (tag, branch or hash); `gitSubdir`
        // locates cajeta.json in the checkout, empty meaning the repo root.
        std::string gitRef;
        std::string gitSubdir;
    };

    /// One declared dependency from `settings.dependencies`.
    struct DependencySpec {
        std::string name;            // e.g. "dev.cajeta.http"
        std::string versionConstraint;  // semver constraint string
        std::optional<std::string> fromRepo;  // optional repository pin
    };

    /// One `settings.overrides` entry, forcing the resolution of a TRANSITIVE
    /// dependency; a direct root dep of the same name still wins. The path and
    /// git replacement forms parse here but are rejected at the resolver.
    struct OverrideSpec {
        std::string name;
        std::string versionConstraint;       // empty when path/git form
        std::optional<std::string> path;      // 6c
        std::optional<std::string> git;       // 6c
        std::optional<std::string> rev;       // 6c (companion to git)
        bool allowMajorDowngrade = false;
    };

    /// The outcome of resolving one declared dependency.
    struct ResolvedDependency {
        std::string name;
        std::string version;          // the concrete chosen version
        std::string resolvedFromRepo; // repository name that supplied it
        std::string artifactPath;     // absolute path to the cached .cja
        std::string sha256;            // "sha256:<hex>"
    };

    /// The flat package list plus the edges the MVS solver gathered. `roots`
    /// are the direct deps as the solver consumed them; a package in `opaque`
    /// had no manifest sidecar, so its children are UNKNOWN, not empty.
    struct ResolvedGraph {
        std::vector<ResolvedDependency> packages;
        std::vector<DependencySpec> roots;
        std::map<std::string, std::vector<DependencySpec>> children;
        std::set<std::string> opaque;
    };

    /// Parses `settings.repositories` into priority-descending order, ties
    /// broken by declaration order so the first-listed wins among equals.
    llvm::Expected<std::vector<RepositorySpec>> parseRepositories(
        const Manifest& m);

    /// Parses `settings.dependencies` in declaration order, accepting either a
    /// bare constraint string or a {version, from} object. The path and git
    /// shapes parse into stubs whose version constraint is empty.
    llvm::Expected<std::vector<DependencySpec>> parseDependencies(
        const Manifest& m);

    /// Parses `settings.overrides` in declaration order, accepting a bare
    /// version, a {version, allow-major-downgrade} object, or the path and git
    /// replacement objects.
    llvm::Expected<std::vector<OverrideSpec>> parseOverrides(
        const Manifest& m);

} // namespace cajeta::buildtool
