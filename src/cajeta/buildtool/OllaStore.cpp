#include "cajeta/buildtool/OllaStore.h"

#include "cajeta/buildtool/ArtifactCache.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace cajeta::buildtool {

    namespace {

        namespace fs = std::filesystem;

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // Atomic copy: write a sibling temp file in the destination
        // directory, then rename over the target (rename within one
        // directory is atomic on POSIX).
        llvm::Error atomicCopy(const fs::path& src, const fs::path& dst) {
            std::error_code ec;
            fs::create_directories(dst.parent_path(), ec);
            if (ec) {
                return err("olla store: cannot create '" +
                           dst.parent_path().string() + "': " + ec.message());
            }
            fs::path tmp = dst;
            tmp += ".tmp";
            fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                fs::remove(tmp);
                return err("olla store: cannot copy '" + src.string() +
                           "' -> '" + tmp.string() + "': " + ec.message());
            }
            fs::rename(tmp, dst, ec);
            if (ec) {
                fs::remove(tmp);
                return err("olla store: cannot rename '" + tmp.string() +
                           "' -> '" + dst.string() + "': " + ec.message());
            }
            return llvm::Error::success();
        }

        // Atomically write `text` to `dst`.
        llvm::Error atomicWriteText(const fs::path& dst,
                                    const std::string& text) {
            std::error_code ec;
            fs::create_directories(dst.parent_path(), ec);
            fs::path tmp = dst;
            tmp += ".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out) {
                    return err("olla store: cannot open '" + tmp.string() +
                               "' for write");
                }
                out << text;
            }
            fs::rename(tmp, dst, ec);
            if (ec) {
                fs::remove(tmp);
                return err("olla store: cannot rename '" + tmp.string() +
                           "' -> '" + dst.string() + "': " + ec.message());
            }
            return llvm::Error::success();
        }

        // Merge `version` into <pkgDir>/versions.json's "versions"
        // array (idempotent, sorted).
        llvm::Error upsertVersions(const fs::path& pkgDir,
                                   const std::string& version) {
            fs::path file = pkgDir / "versions.json";
            std::vector<std::string> versions;

            std::error_code ec;
            if (fs::is_regular_file(file, ec)) {
                std::ifstream in(file, std::ios::binary);
                std::ostringstream buf;
                buf << in.rdbuf();
                auto parsed = llvm::json::parse(buf.str());
                if (parsed) {
                    if (auto* obj = parsed->getAsObject()) {
                        if (auto* arr = obj->getArray("versions")) {
                            for (const auto& v : *arr) {
                                if (auto s = v.getAsString()) {
                                    versions.push_back(std::string(*s));
                                }
                            }
                        }
                    }
                }
                // A malformed/unreadable versions.json is rebuilt from
                // the single known version below rather than failing.
            }

            if (std::find(versions.begin(), versions.end(), version) ==
                versions.end()) {
                versions.push_back(version);
            }
            std::sort(versions.begin(), versions.end());

            llvm::json::Array arr;
            for (const auto& v : versions) arr.push_back(v);
            llvm::json::Value doc =
                llvm::json::Object{{"versions", std::move(arr)}};
            std::string text;
            llvm::raw_string_ostream os(text);
            os << llvm::formatv("{0:2}", doc);
            os.flush();
            return atomicWriteText(file, text);
        }

    } // namespace

    OllaStore::OllaStore(std::string root) : root_(std::move(root)) {}

    std::string OllaStore::resolveRoot(
        std::optional<std::string> homeOverride) {
        if (const char* env = ::getenv("OLLA_HOME")) {
            if (env[0] != '\0') return std::string(env);
        }
        std::string home;
        if (homeOverride) {
            home = *homeOverride;
        } else if (const char* h = ::getenv("HOME")) {
            home = h;
        }
        return (fs::path(home) / ".olla").string();
    }

    std::string OllaStore::artifactPath(const std::string& name,
                                        const std::string& version) const {
        return (fs::path(root_) / name / version /
                (name + "-" + version + ".cja"))
            .string();
    }

    std::optional<std::string> OllaStore::read(
        const std::string& name, const std::string& version) const {
        std::string path = artifactPath(name, version);
        std::error_code ec;
        if (fs::is_regular_file(path, ec)) return path;
        return std::nullopt;
    }

    llvm::Expected<std::string> OllaStore::write(
        const std::string& name, const std::string& version,
        const std::string& sourceArtifactPath,
        std::optional<std::string> sourceManifestPath) {
        std::error_code ec;
        if (!fs::is_regular_file(sourceArtifactPath, ec)) {
            return err("olla store: source artifact not found: '" +
                       sourceArtifactPath + "'");
        }
        fs::path pkgDir = fs::path(root_) / name;
        fs::path verDir = pkgDir / version;
        fs::path dst = verDir / (name + "-" + version + ".cja");

        if (auto e = atomicCopy(sourceArtifactPath, dst)) {
            return std::move(e);
        }
        if (sourceManifestPath) {
            if (auto e = atomicCopy(*sourceManifestPath, verDir / "cajeta.json")) {
                return std::move(e);
            }
        }
        if (auto e = upsertVersions(pkgDir, version)) {
            return std::move(e);
        }
        return dst.string();
    }

    llvm::Expected<std::string> OllaStore::writeVerified(
        const std::string& name, const std::string& version,
        const std::string& sourceArtifactPath,
        const std::string& expectedSha256, const std::string& manifestJson) {
        if (!expectedSha256.empty()) {
            std::string actual = ArtifactCache::sha256OfFile(sourceArtifactPath);
            if (actual != expectedSha256) {
                return err("olla store: integrity check failed for '" + name +
                           "@" + version + "': expected " + expectedSha256 +
                           ", got " + actual + " (store left unchanged)");
            }
        }
        auto written = write(name, version, sourceArtifactPath, std::nullopt);
        if (!written) return written.takeError();
        if (!manifestJson.empty()) {
            fs::path sidecar =
                fs::path(root_) / name / version / "cajeta.json";
            if (auto e = atomicWriteText(sidecar, manifestJson)) {
                return std::move(e);
            }
        }
        return *written;
    }

} // namespace cajeta::buildtool
