// Content-addressed staging of fetched `.cja` artifacts under
// <projectRoot>/.cajeta/cache/artifacts/<sha256>.cja — the stable path handed to
// `--classpath`. Cross-project persistence is OllaStore's `~/.olla/`, not this.

#pragma once

#include <llvm/Support/Error.h>

#include <optional>
#include <string>

namespace cajeta::buildtool {

    // Resolved cache locations + lookup; construct once per invocation.
    class ArtifactCache {
    public:
        // `homeOverride` is retained for call-site compatibility and unused.
        explicit ArtifactCache(std::string projectRoot,
                               std::optional<std::string> homeOverride
                                   = std::nullopt);

        const std::string& projectCacheDir() const { return projectDir_; }

        // The local path for an artifact's sha256, or nullopt on a miss.
        std::optional<std::string> lookup(const std::string& sha256) const;

        // Copy `sourcePath` in, keyed by its content sha256; returns that path.
        llvm::Expected<std::string> insert(const std::string& sourcePath);

        // The sha256 digest of a file, as "sha256:<hex>".
        static std::string sha256OfFile(const std::string& path);

    private:
        std::string projectDir_;
    };

} // namespace cajeta::buildtool
