#include "cajeta/buildtool/repo/GitRepository.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <openssl/evp.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // Shell-escape a single argument for /bin/sh -c. Wraps the
        // value in single quotes and escapes embedded single quotes.
        // Used to assemble safe `git` invocations.
        std::string shEscape(const std::string& s) {
            std::string out;
            out.reserve(s.size() + 2);
            out.push_back('\'');
            for (char c : s) {
                if (c == '\'') out += "'\\''";
                else out.push_back(c);
            }
            out.push_back('\'');
            return out;
        }

        // Run a `git` command in the given working directory. stdout
        // and stderr are silenced; non-zero exit is reported with the
        // command line for diagnostics.
        llvm::Error runGit(const std::string& cwd,
                           const std::string& gitArgs,
                           const std::string& diagPrefix) {
            std::ostringstream cmd;
            cmd << "git -C " << shEscape(cwd) << " " << gitArgs
                << " >/dev/null 2>&1";
            int rc = std::system(cmd.str().c_str());
            if (rc != 0) {
                return err(diagPrefix + ": `git " + gitArgs +
                           "` failed (exit " + std::to_string(rc) +
                           ", cwd=" + cwd + ")");
            }
            return llvm::Error::success();
        }

        std::string sha256TruncatedHex(const std::string& input) {
            unsigned char digest[EVP_MAX_MD_SIZE];
            unsigned int len = 0;
            EVP_MD_CTX* ctx = EVP_MD_CTX_new();
            EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
            EVP_DigestUpdate(ctx, input.data(), input.size());
            EVP_DigestFinal_ex(ctx, digest, &len);
            EVP_MD_CTX_free(ctx);
            std::ostringstream hex;
            hex << std::hex << std::setfill('0');
            for (unsigned i = 0; i < 16 && i < len; ++i) {
                hex << std::setw(2) << static_cast<int>(digest[i]);
            }
            return hex.str();
        }

    } // namespace

    GitRepository::GitRepository(std::string name,
                                 std::string cloneUrl,
                                 std::string gitRef,
                                 std::string gitSubdir,
                                 std::string stageDir)
        : name_(std::move(name)),
          cloneUrl_(std::move(cloneUrl)),
          gitRef_(std::move(gitRef)),
          gitSubdir_(std::move(gitSubdir)),
          stageDir_(std::move(stageDir)) {
        namespace fs = std::filesystem;
        cloneDir_ = (fs::path(stageDir_) / "git" /
                     hashKey(cloneUrl_, gitRef_)).string();
    }

    GitRepository::~GitRepository() = default;

    std::string GitRepository::hashKey(const std::string& url,
                                       const std::string& ref) {
        return sha256TruncatedHex(url + "\n" + ref);
    }

    std::string GitRepository::checkoutDir() const {
        if (gitSubdir_.empty()) return cloneDir_;
        return (std::filesystem::path(cloneDir_) / gitSubdir_).string();
    }

    llvm::Error GitRepository::ensureClone() const {
        namespace fs = std::filesystem;
        std::lock_guard<std::mutex> lock(mu_);
        if (cloned_) return llvm::Error::success();

        std::error_code ec;
        fs::create_directories(fs::path(cloneDir_).parent_path(), ec);
        if (ec) {
            return err("git repository '" + name_ +
                       "': cannot create stage parent '" +
                       fs::path(cloneDir_).parent_path().string() +
                       "': " + ec.message());
        }

        if (!fs::exists(cloneDir_, ec)) {
            // First clone. Shallow + single-branch when the ref is a
            // branch/tag is a future enhancement; for now do a full
            // clone so any literal ref (including commit hashes) is
            // checkout-able.
            std::ostringstream cmd;
            cmd << "git clone " << shEscape(cloneUrl_) << " "
                << shEscape(cloneDir_) << " >/dev/null 2>&1";
            int rc = std::system(cmd.str().c_str());
            if (rc != 0) {
                return err("git repository '" + name_ +
                           "': clone failed (exit " +
                           std::to_string(rc) + ", url=" + cloneUrl_ +
                           ")");
            }
        }

        if (auto e = runGit(cloneDir_,
                            "checkout " + shEscape(gitRef_),
                            "git repository '" + name_ + "'")) {
            return e;
        }

        cloned_ = true;
        return llvm::Error::success();
    }

    llvm::Error GitRepository::ensureMetadata() const {
        if (auto e = ensureClone()) return e;
        std::lock_guard<std::mutex> lock(mu_);
        if (metadataLoaded_) return llvm::Error::success();

        namespace fs = std::filesystem;
        fs::path sidecar = fs::path(checkoutDir()) / "cajeta.json";
        std::error_code ec;
        if (!fs::is_regular_file(sidecar, ec)) {
            return err("git repository '" + name_ +
                       "': no cajeta.json at '" + sidecar.string() +
                       "' (clone=" + cloneUrl_ + ", ref=" + gitRef_ +
                       ", subdir='" + gitSubdir_ + "')");
        }

        std::ifstream in(sidecar, std::ios::binary);
        if (!in) {
            return err("git repository '" + name_ +
                       "': cannot open '" + sidecar.string() + "'");
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        manifestJsonBytes_ = buf.str();

        // Parse just enough to pull name + version. We use llvm::json
        // directly here (rather than the full Manifest loader) so
        // syntactically-invalid sidecars surface as parse errors
        // attributed to the git repo rather than as generic manifest
        // failures.
        auto parsed = llvm::json::parse(manifestJsonBytes_);
        if (!parsed) {
            llvm::consumeError(parsed.takeError());
            return err("git repository '" + name_ +
                       "': cajeta.json at '" + sidecar.string() +
                       "' is not valid JSON");
        }
        const auto* root = parsed->getAsObject();
        if (!root) {
            return err("git repository '" + name_ +
                       "': cajeta.json at '" + sidecar.string() +
                       "' must be a JSON object");
        }
        const auto* details = root->getObject("details");
        if (!details) {
            return err("git repository '" + name_ +
                       "': cajeta.json at '" + sidecar.string() +
                       "' missing 'details' block");
        }
        auto declName = details->getString("name");
        auto declVer  = details->getString("version");
        if (!declName || !declVer) {
            return err("git repository '" + name_ +
                       "': cajeta.json at '" + sidecar.string() +
                       "' must declare details.name + details.version");
        }
        declaredName_    = declName->str();
        declaredVersion_ = declVer->str();
        metadataLoaded_  = true;
        return llvm::Error::success();
    }

    llvm::Expected<std::vector<std::string>>
    GitRepository::listVersions(const std::string& packageName) const {
        if (auto e = ensureMetadata()) {
            return std::move(e);
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (packageName != declaredName_) return std::vector<std::string>{};
        return std::vector<std::string>{declaredVersion_};
    }

    llvm::Expected<std::string> GitRepository::fetch(
        const std::string& packageName,
        const std::string& version) const {
        if (auto e = ensureMetadata()) {
            return std::move(e);
        }
        std::string n, v, co;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (packageName != declaredName_ ||
                version != declaredVersion_) {
                return err("git repository '" + name_ +
                           "' does not carry " + packageName + "@" +
                           version + " (declared: " + declaredName_ +
                           "@" + declaredVersion_ + ")");
            }
            n = declaredName_;
            v = declaredVersion_;
            co = checkoutDir();
        }

        namespace fs = std::filesystem;
        fs::path artifact = fs::path(co) / "build" / "archive" /
                            (n + "-" + v + ".cja");
        std::error_code ec;
        if (!fs::is_regular_file(artifact, ec)) {
            return err("git repository '" + name_ +
                       "': expected pre-built artifact at '" +
                       artifact.string() + "' but it does not exist. "
                       "v1 limitation: run `cajeta build` inside the "
                       "clone, or check out a ref whose tree already "
                       "contains the built `.cja`.");
        }
        return artifact.string();
    }

    llvm::Expected<std::optional<std::string>>
    GitRepository::fetchManifestJson(
        const std::string& packageName,
        const std::string& version) const {
        if (auto e = ensureMetadata()) {
            return std::move(e);
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (packageName != declaredName_ ||
            version != declaredVersion_) {
            return std::optional<std::string>{};
        }
        return std::optional<std::string>{manifestJsonBytes_};
    }

} // namespace cajeta::buildtool
