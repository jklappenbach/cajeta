// Cajeta build-tool repository driver interface.
//
// A Repository is anything the build tool can ask for an artifact
// named `<name>@<version>` and get back a `.cja` (file path).
// Phase 6a ships the filesystem driver; HTTP / Git / Maven-compat
// follow in 6b / 6c.

#pragma once

#include "cajeta/buildtool/Dependency.h"

#include <llvm/Support/Error.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // Each Repository implementation answers two questions:
    //   1. What versions of <name> do you have?
    //   2. Give me the artifact at <name>@<version> as a local
    //      file path (downloading + caching if needed).
    class Repository {
    public:
        virtual ~Repository() = default;

        // The spec name (e.g. "central", "local-dev"). Used in
        // error messages + ResolvedDependency.resolvedFromRepo.
        virtual std::string name() const = 0;

        // List all known versions for `name`. Returns an empty
        // vector when the repository doesn't carry that package
        // (NOT an error — caller falls through to the next repo).
        virtual llvm::Expected<std::vector<std::string>> listVersions(
            const std::string& name) const = 0;

        // Return a local file path to the `.cja` for
        // `name@version`. Implementations may copy from a remote
        // source into a local cache; the returned path must point
        // at a readable file that survives until the caller is
        // done with it (so callers can hand it to the compiler's
        // --classpath flag).
        virtual llvm::Expected<std::string> fetch(
            const std::string& name,
            const std::string& version) const = 0;

        // Return the dep's published `cajeta.json` JSON bytes (the
        // raw string — caller parses via `loadManifestString`).
        // Used by the transitive-expansion walker to read each
        // resolved dep's own `settings.dependencies` so we can
        // expand the graph.
        //
        // Implementations may serve this from a sidecar file
        // (filesystem repo) or a dedicated endpoint (HTTP repo's
        // `GET /<name>/<version>/cajeta.json`). When the published
        // archive embeds its manifest as a Resource entry (future
        // optimization), drivers may extract from there instead —
        // the contract is just "give me the bytes".
        //
        // `std::nullopt` means "this repo can't produce a manifest
        // for this dep" (typical for archives that pre-date the
        // sidecar convention). Caller falls through to the next
        // repository or surfaces a clear error.
        virtual llvm::Expected<std::optional<std::string>>
        fetchManifestJson(
            const std::string& name,
            const std::string& version) const = 0;
    };

    using RepositoryPtr = std::shared_ptr<Repository>;

    // Build the typed Repository drivers from the parsed spec list,
    // in priority-descending order.
    //
    // `downloadStageDir` is the directory where remote drivers
    // (HTTP, eventually Git) stage fetched bytes before the
    // ArtifactCache content-addresses them. The path is created on
    // demand; callers typically pass `<projectRoot>/.cajeta/cache/downloads/`.
    // Local-only drivers (filesystem) ignore it.
    //
    // Recognized types:
    //   - filesystem (Phase 6a)
    //   - http        (Phase 6b)
    //   - git         (Phase 6c)
    // `maven-compat` parses but is deferred — Maven-Central-as-
    // primary-host is a JVM pattern that doesn't fit cajeta;
    // enterprise users can point the native HTTP driver at
    // Nexus/Artifactory directly.
    llvm::Expected<std::vector<RepositoryPtr>> buildRepositories(
        const std::vector<RepositorySpec>& specs,
        const std::string& downloadStageDir = {});

} // namespace cajeta::buildtool
