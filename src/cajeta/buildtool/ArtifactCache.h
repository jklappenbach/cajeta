// ArtifactCache — content-addressed staging of fetched `.cja`
// artifacts in the per-project cache:
//   <projectRoot>/.cajeta/cache/artifacts/<sha256>.cja
//
// This is the stable content-hash path handed to the `--classpath`
// build step; `cajeta clean --deep` wipes it. Cross-project /
// cross-machine persistence is the `~/.olla/` local repository
// (name/version-addressed, populated by resolution write-through and
// `cajeta install`) — see OllaStore. The former workstation
// content-hash tier (`~/.cajeta/cache/artifacts/`) was retired in
// favor of `~/.olla/` (olla-local-repo U3b).

#pragma once

#include <llvm/Support/Error.h>

#include <optional>
#include <string>

namespace cajeta::buildtool {

    // Resolved cache locations + lookup. Construct once per build
    // tool invocation.
    class ArtifactCache {
    public:
        // Construct using the given project root. `homeOverride` is
        // retained for call-site compatibility but is no longer used:
        // the workstation tier it parameterized was retired (U3b).
        explicit ArtifactCache(std::string projectRoot,
                               std::optional<std::string> homeOverride
                                   = std::nullopt);

        // Project cache root (`.cajeta/cache/artifacts/`).
        const std::string& projectCacheDir() const { return projectDir_; }

        // Look up an artifact by sha256 in the project cache. Returns
        // the local path when present; nullopt on miss.
        std::optional<std::string> lookup(const std::string& sha256) const;

        // Copy `sourcePath` into the project cache, keyed by the
        // file's content sha256. Returns the cached path.
        llvm::Expected<std::string> insert(const std::string& sourcePath);

        // Compute the sha256 hex digest of a file.
        // Returns "sha256:<hex>".
        static std::string sha256OfFile(const std::string& path);

    private:
        std::string projectDir_;
    };

} // namespace cajeta::buildtool
