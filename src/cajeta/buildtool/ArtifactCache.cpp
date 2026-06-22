#include "cajeta/buildtool/ArtifactCache.h"

#include <llvm/Support/Error.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // sha256 digest is the cache key (with the "sha256:" prefix
        // stripped) — used to build filenames like `<hex>.cja`.
        std::string keyFromSha(const std::string& full) {
            static const std::string prefix = "sha256:";
            if (full.size() > prefix.size() &&
                full.compare(0, prefix.size(), prefix) == 0) {
                return full.substr(prefix.size());
            }
            return full;
        }

    } // namespace

    ArtifactCache::ArtifactCache(std::string projectRoot,
                                 std::optional<std::string> /*homeOverride*/) {
        namespace fs = std::filesystem;
        projectDir_ = (fs::path(projectRoot) / ".cajeta" / "cache" /
                       "artifacts").string();
        // homeOverride is unused since U3b: the workstation tier it
        // parameterized was retired in favor of ~/.olla (OllaStore).
    }

    std::string ArtifactCache::sha256OfFile(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return "";
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
        char buf[8192];
        while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
            EVP_DigestUpdate(ctx, buf,
                             static_cast<size_t>(in.gcount()));
        }
        unsigned char digest[SHA256_DIGEST_LENGTH];
        unsigned int outLen = 0;
        EVP_DigestFinal_ex(ctx, digest, &outLen);
        EVP_MD_CTX_free(ctx);
        static const char* hexd = "0123456789abcdef";
        std::string s = "sha256:";
        s.reserve(7 + outLen * 2);
        for (unsigned i = 0; i < outLen; ++i) {
            s += hexd[(digest[i] >> 4) & 0xF];
            s += hexd[digest[i] & 0xF];
        }
        return s;
    }

    std::optional<std::string> ArtifactCache::lookup(
        const std::string& sha256) const {
        namespace fs = std::filesystem;
        std::string key = keyFromSha(sha256);
        std::string filename = key + ".cja";
        std::error_code ec;
        fs::path p = fs::path(projectDir_) / filename;
        if (fs::is_regular_file(p, ec)) return p.string();
        return std::nullopt;
    }

    llvm::Expected<std::string> ArtifactCache::insert(
        const std::string& sourcePath) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::is_regular_file(sourcePath, ec)) {
            return err("cache.insert: source not a regular file: '" +
                       sourcePath + "'");
        }
        std::string sha = sha256OfFile(sourcePath);
        if (sha.empty()) {
            return err("cache.insert: cannot hash '" + sourcePath + "'");
        }
        std::string key = keyFromSha(sha);
        std::string filename = key + ".cja";

        fs::create_directories(projectDir_, ec);
        if (ec) {
            return err("cache.insert: cannot create project cache '" +
                       projectDir_ + "': " + ec.message());
        }

        // Copy into the project cache. overwrite_existing keeps
        // re-inserts idempotent. Cross-project persistence is ~/.olla
        // (OllaStore), not a second cache tier here (U3b).
        fs::path projectDst = fs::path(projectDir_) / filename;
        fs::copy_file(sourcePath, projectDst,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return err("cache.insert: copy to '" + projectDst.string() +
                       "': " + ec.message());
        }
        return projectDst.string();
    }

} // namespace cajeta::buildtool
