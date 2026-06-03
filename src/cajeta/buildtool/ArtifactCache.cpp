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
                                 std::optional<std::string> homeOverride) {
        namespace fs = std::filesystem;
        projectDir_ = (fs::path(projectRoot) / ".cajeta" / "cache" /
                       "artifacts").string();
        std::string home;
        if (homeOverride) {
            home = *homeOverride;
        } else if (const char* h = std::getenv("HOME")) {
            home = h;
        } else {
            // Fall back to /tmp so the workstation tier still has
            // a writable location (degraded but functional).
            home = "/tmp";
        }
        workstationDir_ = (fs::path(home) / ".cajeta" / "cache" /
                           "artifacts").string();
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
        // Project tier first (closer + more likely fresh).
        fs::path p = fs::path(projectDir_) / filename;
        if (fs::is_regular_file(p, ec)) return p.string();
        // Workstation tier next.
        p = fs::path(workstationDir_) / filename;
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

        // Ensure both tier directories exist.
        fs::create_directories(projectDir_, ec);
        if (ec) {
            return err("cache.insert: cannot create project cache '" +
                       projectDir_ + "': " + ec.message());
        }
        fs::create_directories(workstationDir_, ec);
        // Workstation create failure is recoverable — the project
        // tier still works. Log via an output later if desired.

        // Copy to project tier. Use overwrite_existing so re-inserts
        // are idempotent.
        fs::path projectDst = fs::path(projectDir_) / filename;
        fs::copy_file(sourcePath, projectDst,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return err("cache.insert: copy to '" + projectDst.string() +
                       "': " + ec.message());
        }
        // Write-through to workstation tier (best-effort).
        fs::path wsDst = fs::path(workstationDir_) / filename;
        std::error_code wsEc;
        fs::copy_file(sourcePath, wsDst,
                      fs::copy_options::overwrite_existing, wsEc);
        // wsEc ignored — workstation tier is a courtesy.

        return projectDst.string();
    }

} // namespace cajeta::buildtool
