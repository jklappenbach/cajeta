// HttpRepository — HTTP-backed repository driver (Phase 6b/6d).
// Speaks the v1 REST API from BuildTool.md "HTTP repository":
//
//   GET <base>/<name>/versions.json
//     → { "versions": [...], "deprecated": [...] }
//   GET <base>/<name>/<version>/<name>-<version>.cja
//     → the artifact bytes
//   GET <base>/<name>/<version>/manifest.json
//     → the dep's published cajeta.json (sidecar form)
//
// And (when the server advertises v2 via the well-known capability
// probe) the Phase 6d v2 surface:
//
//   GET  /.well-known/cajeta-capabilities.json
//   GET  /v2/resolve?name=<name>&version=<version>
//   GET  /v2/blob/<sha256>
//   POST /v2/bundle              (tar.zst stream)
//   POST /v2/lockfile-diff        (tar.zst stream)
//   GET  /v2/transparency-log/<sha256>
//
// Auth (per RepositoryAuth): "bearer" with `token` or `token-env`,
// or "mtls" with `client-cert` + `client-key` (+ optional `ca-cert`).
// Unauthenticated repos work fine with auth.type empty.
//
// Backed by libcurl. Each driver instance keeps a CURL* handle for
// connection reuse across listVersions / fetch / fetchManifestJson
// calls on the same repository.
//
// v1 ↔ v2 routing: on first contact the driver probes
// /.well-known/cajeta-capabilities.json once and caches the result
// for the TTL the server hints (or 1h default). A v2-only-aware
// client probing a v1-only server gets a 404 + empty capabilities
// → it sticks to v1 paths. A v1-only client doesn't probe at all
// → v1 paths keep working unchanged. When v2 is advertised the
// driver prefers v2/resolve + v2/blob for fetch (content-addressed
// reuse + retraction surfacing) but still falls back to v1 on
// per-call failure so a partial outage on the v2 surface doesn't
// brick installs.

#pragma once

#include "cajeta/buildtool/Repository.h"

#include <memory>
#include <string>

namespace cajeta::buildtool {

    class HttpRepository : public Repository {
    public:
        // `cacheDir` is the directory where fetched artifacts land
        // (typically the project's `.cajeta/cache/artifacts/` —
        // but the caller's ArtifactCache content-addresses by sha256
        // so a transient temp file under this directory is fine).
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

        // Probe + return the server's capabilities. First call
        // performs the well-known GET; subsequent calls return the
        // cached value until the TTL elapses. A 404 (server is
        // v1-only) yields a default-constructed RepoCapabilities
        // with `probed=true` so the caller can distinguish
        // "v1-only" from "never tried" without re-probing each
        // call.
        llvm::Expected<RepoCapabilities> capabilities() const override;

        // Force a fresh probe. Tests use this to simulate the
        // server flipping its capability surface; production code
        // wouldn't.
        void invalidateCapabilityCache() const;

        // The checksum the server publishes for `name@version`, in
        // "sha256:<hex>" form, from the v2 resolve metadata.
        //
        // `std::nullopt` when this server cannot answer — a v1-only
        // repository, or a resolve that fails. That is deliberately not an
        // error: it means "no published checksum", and the caller decides
        // what to do with that. The fetch that follows produces the real
        // diagnostic if the artifact is genuinely missing.
        llvm::Expected<std::optional<std::string>>
        publishedChecksum(const std::string& packageName,
                          const std::string& version) const override;

        // GET /v2/org-keys/<org> — the signed organization key document
        // (publisher-trust spec 6.1), returned as raw bytes for the caller
        // to verify against its own roots.
        //
        // `std::nullopt` for a v1-only server or a 404: those mean "this
        // repository serves no document for that organization", which is
        // the condition spec 5.4 degrades on. A transport failure or any
        // other status is an ERROR — degrading on those would turn an
        // outage into a verification bypass.
        llvm::Expected<std::optional<std::string>>
        organizationKeys(const std::string& org) const override;

        // GET /v2/resolve?name=...&version=... returned verbatim, so the
        // caller sees the `signed` envelope this driver's typed accessors
        // deliberately do not interpret.
        llvm::Expected<std::optional<std::string>>
        releaseMetadataJson(const std::string& packageName,
                            const std::string& version) const override;

        // GET /v2/repository-keys — the signed delegation naming which keys
        // may sign release metadata.
        llvm::Expected<std::optional<std::string>>
        repositoryKeys() const override;

        llvm::Expected<std::optional<std::string>>
        revocations() const override;

        // GET /v2/resolve?name=...&version=... — returns the typed
        // metadata for the artifact at <name>@<version>. v2-only.
        // Caller-checked: only invoke when capabilities().supportsV2()
        // (otherwise the call returns Error citing the missing
        // protocol).
        llvm::Expected<ResolveMetadata> v2Resolve(
            const std::string& packageName,
            const std::string& version) const;

        // GET /v2/blob/<sha256> — writes the raw bytes to a file
        // under cacheDir (named `<sha256>.cja`). The returned path
        // is suitable for handing to ArtifactCache::insert. v2-only.
        llvm::Expected<std::string> v2FetchBlob(
            const std::string& sha256) const;

        // POST /v2/bundle — streams a tar.zst of all requested
        // artifacts. The unpacker reads the bundle.json index at
        // archive root, writes each `.cja` to `destDir/<sha256>.cja`,
        // and returns a typed map of (name, version) → sha256.
        // v2-only.
        llvm::Expected<BundleResponse> v2Bundle(
            const BundleRequest& req,
            const std::string& destDir) const;

        // POST /v2/lockfile-diff — server returns only the
        // artifacts present in the new lockfile and absent from
        // the old. Falls back to /v2/bundle when the server hasn't
        // snapshotted the old lockfile (a 404 here is *not* an
        // error — the caller is expected to retry as /v2/bundle
        // with the old lockfile's sha256 set as `have`). v2-only.
        llvm::Expected<BundleResponse> v2LockfileDiff(
            const std::string& oldLockfileSha256,
            const std::string& newLockfileSha256,
            const std::string& destDir) const;

        // GET /v2/transparency-log/<sha256> — returns the log
        // entry for the artifact, or Error if the server reports
        // 404 (no log entry means we can't attest this artifact
        // and the caller should fail the install).
        llvm::Expected<TransparencyLogEntry> v2TransparencyLog(
            const std::string& sha256) const;

    private:
        std::string name_;
        std::string baseUrl_;       // no trailing slash
        RepositoryAuth auth_;
        std::string cacheDir_;
        // libcurl handle owned via opaque pImpl — keeps the curl
        // include out of the header so other build units don't need
        // libcurl on their include path.
        struct State;
        std::unique_ptr<State> state_;
    };

    // Free helper exposed for testing: parse a well-known
    // capability JSON blob into a typed RepoCapabilities. Used by
    // the probe + by `cajeta info --capabilities`.
    llvm::Expected<RepoCapabilities> parseCapabilitiesJson(
        const std::string& jsonBody);

} // namespace cajeta::buildtool
