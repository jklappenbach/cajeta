// OllaStore — the `~/.olla/` local repository: a machine-global,
// name/version-addressed store of `.cja` artifacts (the Maven-`.m2`
// analog). Layout matches FilesystemRepository so reads go through
// that driver unchanged:
//
//   <root>/<name>/<version>/<name>-<version>.cja
//   <root>/<name>/<version>/cajeta.json      (cached manifest sidecar)
//   <root>/<name>/versions.json              ({ "versions": [...] })
//
// Root resolves to $OLLA_HOME when set, else <home>/.olla. The store
// is the write-through target for fetched artifacts (resolver) and the
// sink for `cajeta install` — replacing ArtifactCache's workstation
// tier. Writes are atomic (copy to a temp file in the destination
// directory, then rename).

#pragma once

#include <llvm/Support/Error.h>

#include <optional>
#include <string>

namespace cajeta::buildtool {

    class OllaStore {
    public:
        // Construct against an explicit, already-resolved root
        // (use resolveRoot() for the env-derived default).
        explicit OllaStore(std::string root);

        // Resolve the store root: $OLLA_HOME wins; otherwise
        // <home>/.olla, where <home> is `homeOverride` (tests) or $HOME.
        static std::string resolveRoot(
            std::optional<std::string> homeOverride = std::nullopt);

        const std::string& root() const { return root_; }

        // Absolute path where name@version's artifact lives (no
        // existence check).
        std::string artifactPath(const std::string& name,
                                 const std::string& version) const;

        // The artifact path if present in the store; nullopt on miss.
        std::optional<std::string> read(const std::string& name,
                                        const std::string& version) const;

        // Atomically copy `sourceArtifactPath` into the store at
        // name@version (and, when given, `sourceManifestPath` to the
        // cajeta.json sidecar), then update the package's
        // versions.json. Returns the final artifact path.
        llvm::Expected<std::string> write(
            const std::string& name,
            const std::string& version,
            const std::string& sourceArtifactPath,
            std::optional<std::string> sourceManifestPath);

    private:
        std::string root_;
    };

} // namespace cajeta::buildtool
