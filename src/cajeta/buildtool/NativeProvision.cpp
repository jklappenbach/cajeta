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
                return ioErr("native fetch: sha256 mismatch for '" + lib +
                             "' — expected " + expectedSha256 + ", got " + actual);
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

} // namespace cajeta::buildtool
