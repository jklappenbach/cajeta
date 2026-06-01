// ArtifactCache — content-addressed local cache of fetched `.cja`
// artifacts. Two-tier:
//   1. Project cache: <projectRoot>/.cajeta/cache/artifacts/<sha256>.cja
//   2. Workstation cache: ~/.cajeta/cache/artifacts/<sha256>.cja
//
// Lookup is project → workstation → miss. Write-through populates
// both layers on a fresh fetch, so subsequent builds (this
// project, this workstation, or any other project) hit the cache
// without re-fetching.

#pragma once

#include <llvm/Support/Error.h>

#include <optional>
#include <string>

namespace cajeta::buildtool {

    // Resolved cache locations + lookup. Construct once per build
    // tool invocation.
    class ArtifactCache {
    public:
        // Construct using the given project root. The workstation
        // cache resolves from $HOME (or a stand-in for tests via
        // `homeOverride`).
        explicit ArtifactCache(std::string projectRoot,
                               std::optional<std::string> homeOverride
                                   = std::nullopt);

        // Project cache root (`.cajeta/cache/artifacts/`).
        const std::string& projectCacheDir() const { return projectDir_; }
        // Workstation cache root (`~/.cajeta/cache/artifacts/`).
        const std::string& workstationCacheDir() const {
            return workstationDir_;
        }

        // Look up an artifact by sha256. Returns the local path
        // when found in either tier; nullopt on miss.
        std::optional<std::string> lookup(const std::string& sha256) const;

        // Copy `sourcePath` into both cache tiers, keyed by the
        // file's content sha256. Returns the path inside the
        // project cache.
        llvm::Expected<std::string> insert(const std::string& sourcePath);

        // Compute the sha256 hex digest of a file.
        // Returns "sha256:<hex>".
        static std::string sha256OfFile(const std::string& path);

    private:
        std::string projectDir_;
        std::string workstationDir_;
    };

} // namespace cajeta::buildtool
