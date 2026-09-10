// The `~/.olla/` local repository: a machine-global, name/version-addressed store
// of `.cja` artifacts, laid out as `<root>/<name>/<version>/<name>-<version>.cja`
// so FilesystemRepository reads it unchanged. Writes are copy-then-rename atomic.

#pragma once

#include <llvm/Support/Error.h>

#include <optional>
#include <string>

namespace cajeta::buildtool {

    class OllaStore {
    public:
        // Construct against an already-resolved root; see resolveRoot().
        explicit OllaStore(std::string root);

        // $OLLA_HOME wins, else <home>/.olla with <home> from `homeOverride` or $HOME.
        static std::string resolveRoot(
            std::optional<std::string> homeOverride = std::nullopt);

        const std::string& root() const { return root_; }

        // Where name@version's artifact would live; no existence check.
        std::string artifactPath(const std::string& name,
                                 const std::string& version) const;

        // The artifact path if present in the store; nullopt on miss.
        std::optional<std::string> read(const std::string& name,
                                        const std::string& version) const;

        // Atomically copy the artifact, and any `sourceManifestPath` sidecar, into
        // name@version, then update versions.json. Returns the final artifact path.
        llvm::Expected<std::string> write(
            const std::string& name,
            const std::string& version,
            const std::string& sourceArtifactPath,
            std::optional<std::string> sourceManifestPath);

        // As `write`, but a non-empty `expectedSha256` ("sha256:<hex>") must match the
        // source first, a mismatch changing nothing; `manifestJson` becomes the sidecar.
        llvm::Expected<std::string> writeVerified(
            const std::string& name,
            const std::string& version,
            const std::string& sourceArtifactPath,
            const std::string& expectedSha256,
            const std::string& manifestJson);

    private:
        std::string root_;
    };

} // namespace cajeta::buildtool
