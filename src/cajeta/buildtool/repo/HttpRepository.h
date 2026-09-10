// HttpRepository — HTTP repository driver over libcurl (one reused handle per
// instance), speaking BuildTool.md's v1 REST API and, where the well-known probe
// advertises it, the v2 surface below; a failed v2 call falls back to v1.

#pragma once

#include "cajeta/buildtool/Repository.h"

#include <memory>
#include <string>

namespace cajeta::buildtool {

    class HttpRepository : public Repository {
    public:
        // Fetched artifacts land under `cacheDir`, which the caller's
        // ArtifactCache content-addresses, so temp files there are fine.
        HttpRepository(std::string name,
                       std::string baseUrl,
                       RepositoryAuth auth,
                       std::string cacheDir);
        ~HttpRepository() override;

        // Non-copyable: the libcurl easy handle is owned exclusively.
        HttpRepository(const HttpRepository&) = delete;
        HttpRepository& operator=(const HttpRepository&) = delete;

        std::string name() const override { return name_; }

        llvm::Expected<std::vector<std::string>> listVersions(
            const std::string& packageName) const override;

        llvm::Expected<std::string> fetch(
            const std::string& packageName,
            const std::string& version) const override;

        llvm::Expected<std::optional<std::string>>
        fetchManifestJson(
            const std::string& packageName,
            const std::string& version) const override;

        // ─── Phase 6d v2 surface ──────────────────────────────

        // Probes once, then answers from cache until the TTL elapses; a v1-only
        // server yields a default with `probed=true`, not "never tried".
        llvm::Expected<RepoCapabilities> capabilities() const override;

        // Forces a fresh probe; for tests simulating a server flipping surface.
        void invalidateCapabilityCache() const;

        // The checksum published for `name@version` as "sha256:<hex>". Nullopt
        // (not an error) when the server cannot answer: the following fetch
        // produces the diagnostic if the artifact is genuinely missing.
        llvm::Expected<std::optional<std::string>>
        publishedChecksum(const std::string& packageName,
                          const std::string& version) const override;

        // GET /v2/org-keys/<org> — the signed organization key document, raw for
        // the caller to verify. Nullopt only for a v1-only server or a 404 (no
        // such document); any other status is an ERROR, never a trust bypass.
        llvm::Expected<std::optional<std::string>>
        organizationKeys(const std::string& org) const override;

        // GET /v2/resolve verbatim, so the caller sees the `signed` envelope the
        // typed accessors here deliberately do not interpret.
        llvm::Expected<std::optional<std::string>>
        releaseMetadataJson(const std::string& packageName,
                            const std::string& version) const override;

        // GET /v2/repository-keys — which keys may sign release metadata.
        llvm::Expected<std::optional<std::string>>
        repositoryKeys() const override;

        llvm::Expected<std::optional<std::string>>
        revocations() const override;

        // scheme://host[:port] — the base URL reduced to its origin, so a
        // trailing slash or `/v2/` suffix still matches what the operator signed.
        std::string origin() const override;

        // The typed v2 resolve metadata for <name>@<version>. Caller-checked
        // like every v2-only call here: without capabilities().supportsV2() it
        // returns an Error citing the missing protocol.
        llvm::Expected<ResolveMetadata> v2Resolve(
            const std::string& packageName,
            const std::string& version) const;

        // GET /v2/blob/<sha256> — writes the bytes to `<cacheDir>/<sha256>.cja`
        // and returns that path, ready for ArtifactCache::insert. v2-only.
        llvm::Expected<std::string> v2FetchBlob(
            const std::string& sha256) const;

        // POST /v2/bundle — streams a tar.zst of the requested artifacts, each
        // written to `destDir/<sha256>.cja` per its bundle.json index. v2-only.
        llvm::Expected<BundleResponse> v2Bundle(
            const BundleRequest& req,
            const std::string& destDir) const;

        // POST /v2/lockfile-diff — only the artifacts the new lockfile adds. A
        // 404 means the server never snapshotted the old one, and is not an
        // error: the caller retries /v2/bundle with those sha256s as `have`.
        llvm::Expected<BundleResponse> v2LockfileDiff(
            const std::string& oldLockfileSha256,
            const std::string& newLockfileSha256,
            const std::string& destDir) const;

        // GET /v2/transparency-log/<sha256> — the artifact's log entry, or Error
        // on 404: unattestable, so the caller should fail the install.
        llvm::Expected<TransparencyLogEntry> v2TransparencyLog(
            const std::string& sha256) const;

    private:
        std::string name_;
        std::string baseUrl_;       // no trailing slash
        RepositoryAuth auth_;
        std::string cacheDir_;
        // An opaque pImpl, keeping the curl include out of this header.
        struct State;
        std::unique_ptr<State> state_;
    };

    // Parses a well-known capability blob; used by the probe and by `cajeta info`.
    llvm::Expected<RepoCapabilities> parseCapabilitiesJson(
        const std::string& jsonBody);

} // namespace cajeta::buildtool
