// Native-dependency provisioning: the `~/.cajeta/native` cache, fetch, vendor and a
// cache-backed provider. The network is touched only here, never at execution/JIT.

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

    // `cajeta fetch`: cache `sourcePath`, sha256-verified BEFORE caching (empty skips).
    llvm::Expected<std::string> fetchNativeToCache(
        const std::string& cacheRoot, const std::string& lib,
        const std::string& version, const std::string& platform,
        const std::string& sourcePath, const std::string& expectedSha256);

    // `cajeta vendor`: copy into `native/<platform>/` so it ships in the archive.
    llvm::Expected<std::string> vendorNativeArtifact(
        const std::string& projectNativeDir, const std::string& platform,
        const std::string& sourcePath);

    // A cached lib<id>.{a,so,dylib} or <id>.* for one concrete triple; no network.
    std::optional<std::string> findInNativeCache(
        const std::string& cacheRoot, const std::string& lib,
        const std::string& version, const std::string& platform);

    NativeProvider cacheNativeProvider(const std::string& cacheRoot);

    // False for an embargoed lib: Olla carries its metadata, never its binary.
    bool mayMirrorNativeBinary(const NativeLibrary& lib);

    // Cache a redistributable artifact from `mirrorRoot`; an embargoed lib is refused.
    llvm::Expected<std::string> fetchFromOllaMirror(
        const NativeLibrary& lib, const std::string& version,
        const std::string& platform, const std::string& mirrorRoot,
        const std::string& cacheRoot);

    // Every native failure names what, where and how to fix it.
    std::string nativeMissingMessage(
        const std::string& lib, const std::string& version,
        const std::string& platform, const std::vector<std::string>& probedDirs,
        const std::string& acquire);
    std::string nativeChecksumMismatchMessage(
        const std::string& lib, const std::string& expected,
        const std::string& actual);
    std::string nativeUnsupportedPlatformMessage(
        const std::string& lib, const std::string& platform,
        const std::vector<std::string>& availablePlatforms);

    // The network seam: the execution/JIT path sets Execution, hard-erroring any op.
    enum class NativePhase { Provision, Execution };
    void setNativePhase(NativePhase p);
    NativePhase nativePhase();
    // Guards every native network op: an Error during the Execution phase.
    llvm::Error guardNativeNetwork(const std::string& op);

} // namespace cajeta::buildtool
