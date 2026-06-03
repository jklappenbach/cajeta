#include "cajeta/buildtool/repo/FilesystemRepository.h"

#include <llvm/Support/Error.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

    } // namespace

    FilesystemRepository::FilesystemRepository(
        std::string name, std::string root)
        : name_(std::move(name)), root_(std::move(root)) {}

    llvm::Expected<std::vector<std::string>>
    FilesystemRepository::listVersions(
        const std::string& packageName) const {
        namespace fs = std::filesystem;
        std::vector<std::string> versions;

        fs::path pkgDir = fs::path(root_) / packageName;
        std::error_code ec;
        if (!fs::is_directory(pkgDir, ec)) {
            // Empty list — caller handles "no such package here".
            return versions;
        }
        // Each subdirectory is a version. Skip anything that isn't
        // a directory.
        for (const auto& entry : fs::directory_iterator(pkgDir, ec)) {
            if (ec) {
                return err("filesystem repository '" + name_ +
                           "': cannot read " + pkgDir.string() +
                           ": " + ec.message());
            }
            if (!entry.is_directory()) continue;
            versions.push_back(entry.path().filename().string());
        }
        std::sort(versions.begin(), versions.end());
        return versions;
    }

    llvm::Expected<std::string> FilesystemRepository::fetch(
        const std::string& packageName,
        const std::string& version) const {
        namespace fs = std::filesystem;
        fs::path artifact = fs::path(root_) / packageName / version /
                            (packageName + "-" + version + ".cja");
        std::error_code ec;
        if (!fs::is_regular_file(artifact, ec)) {
            return err("filesystem repository '" + name_ +
                       "': artifact not found at '" +
                       artifact.string() + "'");
        }
        // Filesystem repo: return the path verbatim. Caller's
        // ArtifactCache decides whether to copy it into the
        // content-addressed cache for hash-keyed lookup.
        return artifact.string();
    }

    llvm::Expected<std::optional<std::string>>
    FilesystemRepository::fetchManifestJson(
        const std::string& packageName,
        const std::string& version) const {
        namespace fs = std::filesystem;
        fs::path sidecar = fs::path(root_) / packageName / version /
                           "cajeta.json";
        std::error_code ec;
        if (!fs::is_regular_file(sidecar, ec)) {
            return std::optional<std::string>{};
        }
        std::ifstream in(sidecar, std::ios::binary);
        if (!in) {
            return err("filesystem repository '" + name_ +
                       "': cannot open manifest sidecar '" +
                       sidecar.string() + "'");
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        return std::optional<std::string>{buf.str()};
    }

} // namespace cajeta::buildtool
