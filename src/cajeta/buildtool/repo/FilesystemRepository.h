// Directory-tree backed repository driver, laid out as
// `<root>/<name>/<version>/<name>-<version>.cja` with optional versions.json and
// index.json listings. Used for dev-overrides, vendoring and pre-staged CI trees.

#pragma once

#include "cajeta/buildtool/Repository.h"

#include <string>

namespace cajeta::buildtool {

    class FilesystemRepository : public Repository {
    public:
        FilesystemRepository(std::string name, std::string root);

        std::string name() const override { return name_; }

        llvm::Expected<std::vector<std::string>> listVersions(
            const std::string& packageName) const override;

        llvm::Expected<std::string> fetch(
            const std::string& packageName,
            const std::string& version) const override;

        // `<root>/<name>/<version>/cajeta.json`; absent is nullopt, not an error.
        llvm::Expected<std::optional<std::string>>
        fetchManifestJson(
            const std::string& packageName,
            const std::string& version) const override;

        // `<archive>.cja.sha256`; absent means this repo publishes no checksum.
        llvm::Expected<std::optional<std::string>>
        publishedChecksum(const std::string& packageName,
                          const std::string& version) const override;

        // `<archive>.cja.sig`, the default output path of `cajeta archive sign`.
        llvm::Expected<std::optional<std::string>>
        publishedSignature(const std::string& packageName,
                           const std::string& version) const override;

        // `<root>/.well-known/org-keys/<org>.json`, so a local tree can be verified.
        llvm::Expected<std::optional<std::string>>
        organizationKeys(const std::string& org) const override;

        // `<archive>.release.json`, a signed envelope or a plain object; the bytes
        // come back either way and the caller decides what the difference buys.
        llvm::Expected<std::optional<std::string>>
        releaseMetadataJson(const std::string& packageName,
                            const std::string& version) const override;

        // Reads `<root>/.well-known/repository-keys.json`.
        llvm::Expected<std::optional<std::string>>
        repositoryKeys() const override;

        llvm::Expected<std::optional<std::string>>
        revocations() const override;

        // The canonical absolute root: a local tree's closest stable identity.
        std::string origin() const override;

    private:
        std::string name_;
        std::string root_;
    };

} // namespace cajeta::buildtool
