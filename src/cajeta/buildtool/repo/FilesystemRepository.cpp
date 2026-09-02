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

    llvm::Expected<std::optional<std::string>>
    FilesystemRepository::publishedChecksum(
        const std::string& packageName,
        const std::string& version) const {
        namespace fs = std::filesystem;
        fs::path sidecar = fs::path(root_) / packageName / version /
                           (packageName + "-" + version + ".cja.sha256");
        std::error_code ec;
        if (!fs::is_regular_file(sidecar, ec)) {
            return std::optional<std::string>{};
        }
        std::ifstream in(sidecar, std::ios::binary);
        if (!in) {
            return err("filesystem repository '" + name_ +
                       "': cannot open checksum sidecar '" +
                       sidecar.string() + "'");
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        std::string text = buf.str();
        // Tolerate a trailing newline and the `<hex>  <file>` shape
        // `sha256sum` writes, so a sidecar can be produced by the
        // ordinary tool without post-processing.
        auto cut = text.find_first_of(" \t\r\n");
        if (cut != std::string::npos) text.resize(cut);
        if (text.empty()) return std::optional<std::string>{};
        if (text.rfind("sha256:", 0) != 0) text = "sha256:" + text;
        return std::optional<std::string>{text};
    }

    llvm::Expected<std::optional<std::string>>
    FilesystemRepository::publishedSignature(
        const std::string& packageName,
        const std::string& version) const {
        namespace fs = std::filesystem;
        fs::path sidecar = fs::path(root_) / packageName / version /
                           (packageName + "-" + version + ".cja.sig");
        std::error_code ec;
        if (!fs::is_regular_file(sidecar, ec)) {
            return std::optional<std::string>{};
        }
        std::ifstream in(sidecar, std::ios::binary);
        if (!in) {
            return err("filesystem repository '" + name_ +
                       "': cannot open signature sidecar '" +
                       sidecar.string() + "'");
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        // Raw signature bytes, NOT text: no trimming here, unlike the
        // checksum sidecar. A stripped byte is a failed verification.
        return std::optional<std::string>{buf.str()};
    }

    namespace {

        // An organization name reaches the filesystem here, so it must not
        // be able to leave the repository root. `../../etc/passwd` as an
        // org would otherwise read anywhere the process can.
        bool isSafeOrgName(const std::string& org) {
            if (org.empty() || org == "." || org == "..") return false;
            for (char c : org) {
                bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                       || (c >= '0' && c <= '9')
                       || c == '-' || c == '_' || c == '.';
                if (!ok) return false;
            }
            return org.find("..") == std::string::npos;
        }

        llvm::Expected<std::optional<std::string>> readIfPresent(
                const std::filesystem::path& p, const std::string& repoName,
                const char* what) {
            namespace fs = std::filesystem;
            std::error_code ec;
            if (!fs::is_regular_file(p, ec)) {
                return std::optional<std::string>{};
            }
            std::ifstream in(p, std::ios::binary);
            if (!in) {
                return err("filesystem repository '" + repoName +
                           "': cannot open " + what + " '" + p.string() + "'");
            }
            std::ostringstream buf;
            buf << in.rdbuf();
            return std::optional<std::string>{buf.str()};
        }

    } // namespace

    llvm::Expected<std::optional<std::string>>
    FilesystemRepository::organizationKeys(const std::string& org) const {
        namespace fs = std::filesystem;
        if (!isSafeOrgName(org)) {
            return err("filesystem repository '" + name_ + "': '" + org +
                       "' is not a usable organization name");
        }
        fs::path doc = fs::path(root_) / ".well-known" / "org-keys"
                     / (org + ".json");
        return readIfPresent(doc, name_, "organization key document");
    }

    llvm::Expected<std::optional<std::string>>
    FilesystemRepository::releaseMetadataJson(
        const std::string& packageName,
        const std::string& version) const {
        namespace fs = std::filesystem;
        fs::path sidecar = fs::path(root_) / packageName / version /
                           (packageName + "-" + version + ".release.json");
        return readIfPresent(sidecar, name_, "release metadata");
    }

    llvm::Expected<std::optional<std::string>>
    FilesystemRepository::repositoryKeys() const {
        namespace fs = std::filesystem;
        fs::path doc = fs::path(root_) / ".well-known" / "repository-keys.json";
        return readIfPresent(doc, name_, "repository delegation");
    }

    llvm::Expected<std::optional<std::string>>
    FilesystemRepository::revocations() const {
        namespace fs = std::filesystem;
        fs::path doc = fs::path(root_) / ".well-known" / "revocations.json";
        return readIfPresent(doc, name_, "revocation statement");
    }

} // namespace cajeta::buildtool
