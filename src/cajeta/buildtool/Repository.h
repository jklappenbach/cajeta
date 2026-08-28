// Cajeta build-tool repository driver interface.
//
// A Repository is anything the build tool can ask for an artifact
// named `<name>@<version>` and get back a `.cja` (file path).
// Phase 6a ships the filesystem driver; HTTP / Git / Maven-compat
// follow in 6b / 6c.

#pragma once

#include "cajeta/buildtool/Dependency.h"

#include <llvm/Support/Error.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // Phase 6d — Repository protocol v2 surface.
    //
    // Servers advertise v2 support via
    // GET /.well-known/cajeta-capabilities.json (see BuildTool.md
    // "Repository protocol — v2 enhancements"). The probe is cached
    // per-host; v1 stays available as the fallback / compatibility
    // surface so a v1-only server and a v2-only client interoperate
    // (a v1-only client and a v2-capable server already do — the
    // v1 paths haven't moved).
    struct RepoCapabilities {
        // e.g. {"v1"} for legacy, {"v1","v2"} for newer registries.
        std::vector<std::string> protocolVersions;
        bool bundle = false;
        bool contentAddressed = false;
        std::string transparencyLogUrl;
        struct Mirror {
            std::string url;
            std::string region;
        };
        std::vector<Mirror> mirrors;
        // Probe TTL — how long the cached result stays fresh. The
        // server hints via Cache-Control; absent that we default to
        // one hour.
        std::chrono::seconds ttl{3600};
        // Set when this came from the probe rather than a
        // never-contacted default.
        bool probed = false;

        bool supportsV2() const {
            for (const auto& v : protocolVersions) {
                if (v == "v2") return true;
            }
            return false;
        }
    };

    // The metadata blob returned by GET /v2/resolve. The blob
    // itself comes from GET /v2/blob/<sha256>; this struct holds
    // everything *about* the artifact (so the client can decide
    // whether to fetch, whether to warn, what transitive set to
    // pull next).
    struct ResolveMetadata {
        std::string sha256;
        int64_t sizeBytes = 0;
        std::vector<std::pair<std::string, std::string>> deps;
        std::vector<std::string> capabilities;
        std::string publishedAt;
        bool retracted = false;
        std::string retractedReason;
    };

    // Request body for POST /v2/bundle.
    //
    // `have` lists sha256 digests the client already has in its
    // workstation cache so the server can omit them. `want` is the
    // resolved (name, version) set. `transitive=true` asks the
    // server to walk the dep graph server-side and include every
    // transitive artifact in one response. `format` selects the
    // wire encoding ("tar.zst" — the default — or
    // "supercompress.zst" — opt-in, server-CPU-expensive).
    struct BundleRequest {
        std::vector<std::string> have;        // sha256 strings (no "sha256:" prefix)
        struct Want { std::string name; std::string version; };
        std::vector<Want> want;
        bool transitive = true;
        std::string format = "tar.zst";
    };

    // One artifact inside an unpacked bundle response. The .cja
    // bytes land at `artifactPath` (typically inside a temp dir
    // owned by the caller — usually moved into ArtifactCache by
    // content address). The `bundle.json` index at the archive
    // root supplies (name, version, sha256); the client writes
    // every body and records what's inside here.
    struct BundleEntry {
        std::string name;
        std::string version;
        std::string sha256;
        std::string artifactPath;
    };

    struct BundleResponse {
        std::vector<BundleEntry> entries;
        // Names the server omitted (already in `have`) — kept for
        // diagnostics.
        std::vector<std::string> omitted;
    };

    // GET /v2/transparency-log/<sha256> response payload. The
    // signing-launcher already verifies per-artifact signatures; the
    // transparency check slots in alongside as a defence-in-depth
    // layer that catches keys compromised between publish and
    // install.
    struct TransparencyLogEntry {
        int64_t logIndex = 0;
        std::string logTimestamp;
        std::string logSignature;
        std::string keyId;
        std::string issuer;
    };

    // Each Repository implementation answers two questions:
    //   1. What versions of <name> do you have?
    //   2. Give me the artifact at <name>@<version> as a local
    //      file path (downloading + caching if needed).
    class Repository {
    public:
        virtual ~Repository() = default;

        // The spec name (e.g. "central", "local-dev"). Used in
        // error messages + ResolvedDependency.resolvedFromRepo.
        virtual std::string name() const = 0;

        // List all known versions for `name`. Returns an empty
        // vector when the repository doesn't carry that package
        // (NOT an error — caller falls through to the next repo).
        virtual llvm::Expected<std::vector<std::string>> listVersions(
            const std::string& name) const = 0;

        // Return a local file path to the `.cja` for
        // `name@version`. Implementations may copy from a remote
        // source into a local cache; the returned path must point
        // at a readable file that survives until the caller is
        // done with it (so callers can hand it to the compiler's
        // --classpath flag).
        virtual llvm::Expected<std::string> fetch(
            const std::string& name,
            const std::string& version) const = 0;

        // Return the dep's published `cajeta.json` JSON bytes (the
        // raw string — caller parses via `loadManifestString`).
        // Used by the transitive-expansion walker to read each
        // resolved dep's own `settings.dependencies` so we can
        // expand the graph.
        //
        // Implementations may serve this from a sidecar file
        // (filesystem repo) or a dedicated endpoint (HTTP repo's
        // `GET /<name>/<version>/cajeta.json`). When the published
        // archive embeds its manifest as a Resource entry (future
        // optimization), drivers may extract from there instead —
        // the contract is just "give me the bytes".
        //
        // `std::nullopt` means "this repo can't produce a manifest
        // for this dep" (typical for archives that pre-date the
        // sidecar convention). Caller falls through to the next
        // repository or surfaces a clear error.
        virtual llvm::Expected<std::optional<std::string>>
        fetchManifestJson(
            const std::string& name,
            const std::string& version) const = 0;

        // The checksum this repository PUBLISHES for `name@version`,
        // in "sha256:<hex>" form — what a fetched artifact is checked
        // against before it is trusted (notebook-olla-install spec
        // 3.2). Distinct from hashing the bytes we just downloaded,
        // which only proves the download was self-consistent.
        //
        // `std::nullopt` means this repository publishes no checksum
        // for that artifact; the caller proceeds on the fetch alone.
        // Defaulted rather than pure so drivers that have nothing to
        // publish need no change.
        virtual llvm::Expected<std::optional<std::string>>
        publishedChecksum(const std::string& name,
                          const std::string& version) const {
            (void) name;
            (void) version;
            return std::optional<std::string>{};
        }
    };

    using RepositoryPtr = std::shared_ptr<Repository>;

    // Build the typed Repository drivers from the parsed spec list,
    // in priority-descending order.
    //
    // `downloadStageDir` is the directory where remote drivers
    // (HTTP, eventually Git) stage fetched bytes before the
    // ArtifactCache content-addresses them. The path is created on
    // demand; callers typically pass `<projectRoot>/.cajeta/cache/downloads/`.
    // Local-only drivers (filesystem) ignore it.
    //
    // Recognized types:
    //   - filesystem (Phase 6a)
    //   - http        (Phase 6b)
    //   - git         (Phase 6c)
    // `maven-compat` parses but is deferred — Maven-Central-as-
    // primary-host is a JVM pattern that doesn't fit cajeta;
    // enterprise users can point the native HTTP driver at
    // Nexus/Artifactory directly.
    llvm::Expected<std::vector<RepositoryPtr>> buildRepositories(
        const std::vector<RepositorySpec>& specs,
        const std::string& downloadStageDir = {});

} // namespace cajeta::buildtool
