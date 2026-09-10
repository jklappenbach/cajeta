// Cajeta build-tool repository drivers: anything that can be asked for
// `<name>@<version>` and hand back a local `.cja` path, together with the
// protocol-v2 metadata shapes the remote drivers exchange.

#pragma once

#include "cajeta/buildtool/Dependency.h"

#include <llvm/Support/Error.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // Phase 6d protocol-v2 surface, advertised by GET
    // /.well-known/cajeta-capabilities.json, cached per host; v1 stays the fallback.
    struct RepoCapabilities {
        // e.g. {"v1"} for legacy, {"v1","v2"} for newer registries.
        std::vector<std::string> protocolVersions;
        bool bundle = false;
        bool contentAddressed = false;
        // Serves §3.8's revocation statement. A one-way door in practice: from
        // then on a missing or expired statement REFUSES rather than degrading.
        bool revocation = false;
        std::string transparencyLogUrl;
        struct Mirror {
            std::string url;
            std::string region;
        };
        std::vector<Mirror> mirrors;
        // How long a cached probe stays fresh; the server hints via Cache-Control.
        std::chrono::seconds ttl{3600};
        // Set when this came from the probe, not a never-contacted default.
        bool probed = false;

        bool supportsV2() const {
            for (const auto& v : protocolVersions) {
                if (v == "v2") return true;
            }
            return false;
        }
    };

    // What GET /v2/resolve says ABOUT an artifact — enough to decide whether to
    // fetch, whether to warn, and what to pull next. The bytes are /v2/blob.
    struct ResolveMetadata {
        std::string sha256;
        int64_t sizeBytes = 0;
        std::vector<std::pair<std::string, std::string>> deps;
        std::vector<std::string> capabilities;
        std::string publishedAt;
        // The UNSIGNED view of retraction. A mirror clears it as freely as any
        // other plain field, so the install path reads ReleaseIntegrity instead.
        bool retracted = false;
        std::string retractedReason;
        // The organization owning this name; carried here because the client
        // already runs this resolve, and must never DERIVE an org from a name.
        std::string organization;
    };

    // Request body for POST /v2/bundle: `have` are digests the server may omit,
    // `transitive` asks it to walk the dep graph, `format` picks the encoding.
    struct BundleRequest {
        std::vector<std::string> have;        // sha256 strings (no "sha256:" prefix)
        struct Want { std::string name; std::string version; };
        std::vector<Want> want;
        bool transitive = true;
        std::string format = "tar.zst";
    };

    // One artifact inside an unpacked bundle response: the `bundle.json` index
    // supplies (name, version, sha256), and the .cja bytes land at `artifactPath`.
    struct BundleEntry {
        std::string name;
        std::string version;
        std::string sha256;
        std::string artifactPath;
    };

    struct BundleResponse {
        std::vector<BundleEntry> entries;
        // Names the server omitted (already in `have`), kept for diagnostics.
        std::vector<std::string> omitted;
    };

    // GET /v2/transparency-log/<sha256>. Defence in depth alongside the
    // per-artifact signatures: it catches keys compromised after publish.
    struct TransparencyLogEntry {
        int64_t logIndex = 0;
        std::string logTimestamp;
        std::string logSignature;
        std::string keyId;
        std::string issuer;
    };

    // A source of artifacts: which versions of <name> it carries, and the local
    // file path for <name>@<version>, downloading and caching if it must.
    class Repository {
    public:
        virtual ~Repository() = default;

        // The repository's STABLE identity, for signed documents that name it —
        // NOT `name()`, which is only a label out of the user's own manifest.
        virtual std::string origin() const = 0;

        // The spec name (e.g. "central", "local-dev"). Used in
        // error messages + ResolvedDependency.resolvedFromRepo.
        virtual std::string name() const = 0;

        // All known versions of `name`. An empty vector means this repository
        // does not carry it — not an error; the caller tries the next repository.
        virtual llvm::Expected<std::vector<std::string>> listVersions(
            const std::string& name) const = 0;

        // A local, readable path to the `.cja` for `name@version`, copied out of
        // a remote source if need be, valid until the caller is done with it.
        virtual llvm::Expected<std::string> fetch(
            const std::string& name,
            const std::string& version) const = 0;

        // The dep's published `cajeta.json` bytes, for the transitive walker;
        // nullopt when this repo cannot produce one, so the caller falls through.
        virtual llvm::Expected<std::optional<std::string>>
        fetchManifestJson(
            const std::string& name,
            const std::string& version) const = 0;

        // The checksum this repository PUBLISHES for `name@version`, as
        // "sha256:<hex>", or nullopt — not a hash of the bytes just downloaded.
        virtual llvm::Expected<std::optional<std::string>>
        publishedChecksum(const std::string& name,
                          const std::string& version) const {
            (void) name;
            (void) version;
            return std::optional<std::string>{};
        }

        // The detached ed25519 signature published for `name@version`, as
        // `cajeta archive sign` writes it; nullopt when none is published, which
        // `require-signatures` decides how to treat.
        virtual llvm::Expected<std::optional<std::string>>
        publishedSignature(const std::string& name,
                           const std::string& version) const {
            (void) name;
            (void) version;
            return std::optional<std::string>{};
        }

        // The signed organization key document for `org` — raw, UNVERIFIED
        // envelope JSON. nullopt is ABSENCE (degradable); failure stays an error.
        virtual llvm::Expected<std::optional<std::string>>
        organizationKeys(const std::string& org) const {
            (void) org;
            return std::optional<std::string>{};
        }

        // The signed repository delegation naming the keys allowed to sign
        // release metadata. Repository-wide; same contract as organizationKeys.
        virtual llvm::Expected<std::optional<std::string>>
        repositoryKeys() const {
            return std::optional<std::string>{};
        }

        // What this repository advertises (protocol §3.1). The default claims
        // nothing, so §2.8.4's fail-closed revocation rule cannot apply to it.
        virtual llvm::Expected<RepoCapabilities> capabilities() const {
            return RepoCapabilities{};
        }

        // The signed revocation statement naming key ids no longer trusted.
        // Absence is a refusal, not a weaker path, once the repository
        // advertises revocation — that call belongs to revocationFor().
        virtual llvm::Expected<std::optional<std::string>>
        revocations() const {
            return std::optional<std::string>{};
        }

        // The release metadata for `name@version` — a signed envelope or a plain
        // object. Same contract as organizationKeys: unverified, nullopt = absent.
        virtual llvm::Expected<std::optional<std::string>>
        releaseMetadataJson(const std::string& name,
                            const std::string& version) const {
            (void) name;
            (void) version;
            return std::optional<std::string>{};
        }
    };

    using RepositoryPtr = std::shared_ptr<Repository>;

    // Build the typed drivers (filesystem, http, git; maven-compat parses but is
    // deferred) in priority-descending order. `downloadStageDir` stages remote
    // bytes for the ArtifactCache and is created on demand; local drivers ignore it.
    llvm::Expected<std::vector<RepositoryPtr>> buildRepositories(
        const std::vector<RepositorySpec>& specs,
        const std::string& downloadStageDir = {});

} // namespace cajeta::buildtool
