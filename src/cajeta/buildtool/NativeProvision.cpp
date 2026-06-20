// Native-dependency provisioning — unit 9. See NativeProvision.h.

#include "NativeProvision.h"
#include "ArtifactCache.h"

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace cajeta::buildtool {

    namespace {
        std::string stripSha(const std::string& s) {
            const std::string p = "sha256:";
            return s.rfind(p, 0) == 0 ? s.substr(p.size()) : s;
        }
        llvm::Error ioErr(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }
    } // namespace

    std::string nativeCacheDir(const std::string& cacheRoot,
                               const std::string& lib,
                               const std::string& version,
                               const std::string& platform) {
        return cacheRoot + "/" + lib + "/" + version + "/" + platform;
    }

    llvm::Expected<std::string> fetchNativeToCache(
            const std::string& cacheRoot, const std::string& lib,
            const std::string& version, const std::string& platform,
            const std::string& sourcePath, const std::string& expectedSha256) {
        std::error_code ec;
        if (!fs::exists(sourcePath, ec))
            return ioErr("native fetch: source not found: " + sourcePath);
        // Verify BEFORE caching — a corrupt download never lands in the cache.
        if (!expectedSha256.empty()) {
            std::string actual = ArtifactCache::sha256OfFile(sourcePath);
            if (actual.empty())
                return ioErr("native fetch: cannot hash '" + sourcePath + "'");
            if (stripSha(actual) != stripSha(expectedSha256))
                return ioErr(nativeChecksumMismatchMessage(
                    lib, expectedSha256, actual));
        }
        std::string dir = nativeCacheDir(cacheRoot, lib, version, platform);
        fs::create_directories(dir, ec);
        if (ec)
            return ioErr("native fetch: cannot create cache dir '" + dir +
                         "': " + ec.message());
        std::string dest = dir + "/" + fs::path(sourcePath).filename().string();
        fs::copy_file(sourcePath, dest,
                      fs::copy_options::overwrite_existing, ec);
        if (ec)
            return ioErr("native fetch: copy to cache failed: " + ec.message());
        return dest;
    }

    llvm::Expected<std::string> vendorNativeArtifact(
            const std::string& projectNativeDir, const std::string& platform,
            const std::string& sourcePath) {
        std::error_code ec;
        if (!fs::exists(sourcePath, ec))
            return ioErr("native vendor: source not found: " + sourcePath);
        std::string dir = projectNativeDir + "/" + platform;
        fs::create_directories(dir, ec);
        if (ec)
            return ioErr("native vendor: cannot create '" + dir + "': " +
                         ec.message());
        std::string dest = dir + "/" + fs::path(sourcePath).filename().string();
        fs::copy_file(sourcePath, dest,
                      fs::copy_options::overwrite_existing, ec);
        if (ec)
            return ioErr("native vendor: copy failed: " + ec.message());
        return dest;
    }

    std::optional<std::string> findInNativeCache(
            const std::string& cacheRoot, const std::string& lib,
            const std::string& version, const std::string& platform) {
        std::string dir = nativeCacheDir(cacheRoot, lib, version, platform);
        const char* suffixes[] = {".a", ".so", ".dylib"};
        std::error_code ec;
        for (const char* sfx : suffixes) {
            for (const std::string& p : {dir + "/lib" + lib + sfx,
                                         dir + "/" + lib + sfx}) {
                if (fs::exists(p, ec)) return p;
            }
        }
        return std::nullopt;
    }

    NativeProvider cacheNativeProvider(const std::string& cacheRoot) {
        return NativeProvider{"cache",
            [cacheRoot](const std::string& lib, const std::string& version,
                        const std::string& platform)
                -> std::optional<std::string> {
                return findInNativeCache(cacheRoot, lib, version, platform);
            }};
    }

    // --- Unit 10: Olla native mirror -------------------------------------

    bool mayMirrorNativeBinary(const NativeLibrary& lib) {
        return lib.redistributable;
    }

    llvm::Expected<std::string> fetchFromOllaMirror(
            const NativeLibrary& lib, const std::string& version,
            const std::string& platform, const std::string& mirrorRoot,
            const std::string& cacheRoot) {
        // Network seam: a mirror fetch is a network op — refused at run/JIT time.
        if (auto e = guardNativeNetwork("Olla mirror fetch of '" + lib.id + "'"))
            return std::move(e);
        if (!mayMirrorNativeBinary(lib)) {
            std::string acq = lib.acquire ? (" (" + *lib.acquire + ")") : "";
            return ioErr("native: '" + lib.id + "' is embargoed "
                         "(redistributable:false) — Olla serves metadata only, "
                         "not the binary. Acquire it and provision locally" +
                         acq + ".");
        }
        // Locate the artifact on the mirror (stub: a local
        // <mirrorRoot>/<lib>/<version>/<platform>/ dir).
        std::string mdir =
            mirrorRoot + "/" + lib.id + "/" + version + "/" + platform;
        std::string srcFile;
        const char* suffixes[] = {".a", ".so", ".dylib"};
        std::error_code ec;
        for (const char* sfx : suffixes) {
            for (const std::string& p : {mdir + "/lib" + lib.id + sfx,
                                         mdir + "/" + lib.id + sfx}) {
                if (fs::exists(p, ec)) { srcFile = p; break; }
            }
            if (!srcFile.empty()) break;
        }
        if (srcFile.empty())
            return ioErr("native: Olla mirror has no artifact for '" + lib.id +
                         "' " + version + " (" + platform + ")");
        std::string sha;
        auto it = lib.artifacts.find(platform);
        if (it != lib.artifacts.end() && it->second.sha256)
            sha = *it->second.sha256;
        return fetchNativeToCache(cacheRoot, lib.id, version, platform,
                                  srcFile, sha);
    }

    // --- Unit 11: diagnostics + network seam -----------------------------

    std::string nativeMissingMessage(
            const std::string& lib, const std::string& version,
            const std::string& platform,
            const std::vector<std::string>& probedDirs,
            const std::string& acquire) {
        std::string probed;
        for (const auto& d : probedDirs)
            probed += (probed.empty() ? "" : ", ") + d;
        if (probed.empty()) probed = "(none)";
        std::string msg = "native library '" + lib + "' " + version +
            " for " + platform + " not found; probed: " + probed +
            ". Fix: run `cajeta fetch` (online), vendor it into native/" +
            platform + "/, or set CAJETA_NATIVE_PATH";
        if (!acquire.empty()) msg += "; " + acquire;
        return msg + ".";
    }

    std::string nativeChecksumMismatchMessage(
            const std::string& lib, const std::string& expected,
            const std::string& actual) {
        return "native library '" + lib + "': sha256 mismatch — expected " +
            expected + ", got " + actual +
            " (refusing to use a corrupt/tampered artifact)";
    }

    std::string nativeUnsupportedPlatformMessage(
            const std::string& lib, const std::string& platform,
            const std::vector<std::string>& availablePlatforms) {
        std::string avail;
        for (const auto& p : availablePlatforms)
            avail += (avail.empty() ? "" : ", ") + p;
        if (avail.empty()) avail = "(none)";
        return "native library '" + lib + "' is not available for platform '" +
            platform + "'; available: " + avail;
    }

    namespace {
        NativePhase g_nativePhase = NativePhase::Provision;
    }
    void setNativePhase(NativePhase p) { g_nativePhase = p; }
    NativePhase nativePhase() { return g_nativePhase; }

    llvm::Error guardNativeNetwork(const std::string& op) {
        if (g_nativePhase == NativePhase::Execution)
            return ioErr("native: refusing network op (" + op +
                         ") at execution/JIT time — the network is touched only "
                         "at provision time. Pre-provision via `cajeta fetch`/"
                         "`vendor` or bundle the artifact in the .cja.");
        return llvm::Error::success();
    }

} // namespace cajeta::buildtool
