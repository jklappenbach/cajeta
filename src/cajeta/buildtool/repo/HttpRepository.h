// HttpRepository — HTTP-backed repository driver (Phase 6b). Speaks
// the v1 REST API from BuildTool.md "HTTP repository":
//
//   GET <base>/<name>/versions.json
//     → { "versions": [...], "deprecated": [...] }
//   GET <base>/<name>/<version>/<name>-<version>.cja
//     → the artifact bytes
//   GET <base>/<name>/<version>/manifest.json
//     → the dep's published cajeta.json (sidecar form)
//
// Auth (per RepositoryAuth): "bearer" with `token` or `token-env`,
// or "mtls" with `client-cert` + `client-key` (+ optional `ca-cert`).
// Unauthenticated repos work fine with auth.type empty.
//
// Backed by libcurl. Each driver instance keeps a CURL* handle for
// connection reuse across listVersions / fetch / fetchManifestJson
// calls on the same repository.

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

} // namespace cajeta::buildtool
