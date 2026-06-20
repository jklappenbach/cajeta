// Native-dependency provisioning (native-deps unit 9): the `~/.cajeta/native`
// cache, `cajeta fetch` (download → verify → cache) and `cajeta vendor`
// (materialize into the project `native/` dir), and a cache-backed resolver
// provider. The network is touched only here (provision time) — never at
// execution/JIT. See docs/specification/buildtool/native-deps-spec.md §6.

#pragma once

#include "NativeResolver.h"

#include <llvm/Support/Error.h>

#include <optional>
#include <string>

namespace cajeta::buildtool {

    // Versioned cache dir: <cacheRoot>/<lib>/<version>/<platform>.
    std::string nativeCacheDir(const std::string& cacheRoot,
                               const std::string& lib,
                               const std::string& version,
                               const std::string& platform);

    // `cajeta fetch`: bring an artifact into the cache. The "download" is a copy
    // from `sourcePath` (a real fetcher swaps a URL download in here); the
    // artifact is sha256-verified against `expectedSha256` (empty = skip) BEFORE
    // it is cached. Returns the cached path. Network/provision time only.
    llvm::Expected<std::string> fetchNativeToCache(
        const std::string& cacheRoot, const std::string& lib,
        const std::string& version, const std::string& platform,
        const std::string& sourcePath, const std::string& expectedSha256);

    // `cajeta vendor`: copy an artifact into the project `native/<platform>/`
    // dir so it ships inside the archive / resolves with no network.
    llvm::Expected<std::string> vendorNativeArtifact(
        const std::string& projectNativeDir, const std::string& platform,
        const std::string& sourcePath);

    // Find a cached artifact (lib<id>.{a,so,dylib} or <id>.*) for a concrete
    // (lib, version, platform). No network. Empty if absent.
    std::optional<std::string> findInNativeCache(
        const std::string& cacheRoot, const std::string& lib,
        const std::string& version, const std::string& platform);

    // A resolver provider (for resolveNatives) backed by the cache — fully
    // offline. Plugs into the unit-5 probe order.
    NativeProvider cacheNativeProvider(const std::string& cacheRoot);

    // --- Unit 10: Olla native mirror -------------------------------------

    // Whether a native lib's BINARY may be published to / fetched from a public
    // Olla mirror. Embargoed (redistributable=false) → false: Olla carries
    // metadata only, never the binary — the publish-side + fetch-side guard.
    bool mayMirrorNativeBinary(const NativeLibrary& lib);

    // Fetch a redistributable artifact from an Olla mirror into the cache
    // (provision time, verified). `mirrorRoot` stands in for the mirror (the
    // stub is a local `<lib>/<version>/<platform>/` dir; real HTTP swaps in via
    // HttpRepository). An embargoed lib is refused — Olla serves metadata only,
    // directing the developer to provision it (its `acquire` note).
    llvm::Expected<std::string> fetchFromOllaMirror(
        const NativeLibrary& lib, const std::string& version,
        const std::string& platform, const std::string& mirrorRoot,
        const std::string& cacheRoot);

} // namespace cajeta::buildtool
