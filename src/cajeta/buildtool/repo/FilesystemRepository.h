// FilesystemRepository — directory-tree backed repository
// driver. Layout per BuildTool.md "Repositories" section:
//
//   <root>/<name>/<version>/<name>-<version>.cja
//   <root>/<name>/versions.json          (optional, listing)
//   <root>/index.json                    (optional, package list)
//
// Used for dev-overrides, vendoring, and CI scenarios that
// pre-stage artifacts.

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

        // Reads `<root>/<name>/<version>/cajeta.json` when present
        // and returns its raw bytes. Returns nullopt (not an error)
        // when the sidecar is absent — old artifacts that pre-date
        // the sidecar convention silently fall through to the next
        // repository.
        llvm::Expected<std::optional<std::string>>
        fetchManifestJson(
            const std::string& packageName,
            const std::string& version) const override;

        // Reads `<root>/<name>/<version>/<name>-<version>.cja.sha256`
        // when present — the filesystem layout's answer to the HTTP
        // driver's resolve metadata. Absent sidecar means this repo
        // publishes no checksum, which is not an error: pre-staged and
        // vendored trees routinely carry only the archive.
        llvm::Expected<std::optional<std::string>>
        publishedChecksum(const std::string& packageName,
                          const std::string& version) const override;

        // Reads `<root>/<name>/<version>/<name>-<version>.cja.sig` when
        // present — the default output path of `cajeta archive sign`.
        llvm::Expected<std::optional<std::string>>
        publishedSignature(const std::string& packageName,
                           const std::string& version) const override;

        // Reads `<root>/.well-known/org-keys/<org>.json` — the filesystem
        // layout's answer to the HTTP driver's key-document endpoint, so a
        // local or vendored repository can participate in publisher
        // verification instead of only being exempted from it.
        llvm::Expected<std::optional<std::string>>
        organizationKeys(const std::string& org) const override;

        // Reads `<root>/<name>/<version>/<name>-<version>.release.json`.
        // May hold a signed envelope or a plain object; this driver returns
        // the bytes either way and the caller decides what the difference
        // buys (publisher-trust spec 5.3.1).
        llvm::Expected<std::optional<std::string>>
        releaseMetadataJson(const std::string& packageName,
                            const std::string& version) const override;

    private:
        std::string name_;
        std::string root_;
    };

} // namespace cajeta::buildtool
