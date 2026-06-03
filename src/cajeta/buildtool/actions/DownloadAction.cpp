// `download` — HTTP fetch with optional SHA-256 verify.
//
// v1 implementation shells out to `curl`. Phase 6 (repositories +
// dependency resolution) introduces a real HTTP client with auth /
// retry / caching; this action gains a switch to use it as the
// transport at that point. For Phase 4, curl-shell is sufficient
// for the use cases that aren't latency-sensitive.
//
// Params:
//   url         (required) source URL
//   to          (required) destination path
//   sha256      (optional) "sha256:<hex>" — when present, the
//               downloaded bytes are checksummed and the action
//               fails on mismatch
//   auth        (optional, future) bearer-token / basic-auth
//               support; today the action passes -L (follow) but
//               doesn't add auth headers
//
// Outputs:
//   path        the destination path
//   sha256      "sha256:<hex>" of the downloaded bytes

#include "cajeta/buildtool/Action.h"

#include <llvm/Support/Error.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        std::string sha256HexOfFile(const std::string& path) {
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

    } // namespace

    class DownloadAction : public Action {
    public:
        std::string name() const override { return "download"; }

        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& /*ctx*/) const override {

            auto url = params.getString("url");
            auto to  = params.getString("to");
            if (!url) return err("download: missing required 'url'");
            if (!to)  return err("download: missing required 'to'");

            std::vector<std::string> argv = {
                "curl",
                "--fail",          // non-2xx is an error
                "--location",      // follow redirects
                "--silent",        // no progress meter
                "--show-error",    // but print errors to stderr
                "--output", to->str(),
                url->str(),
            };
            // Convert argv to char* for execvp.
            std::vector<char*> argp;
            for (auto& a : argv) argp.push_back(a.data());
            argp.push_back(nullptr);

            pid_t pid = ::fork();
            if (pid < 0) {
                return err("download: fork failed");
            }
            if (pid == 0) {
                ::execvp("curl", argp.data());
                _exit(127);
            }
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0) {
                // restart on EINTR
            }
            int exitCode = 0;
            if (WIFEXITED(status)) exitCode = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) exitCode = 128 + WTERMSIG(status);
            if (exitCode == 127) {
                return err("download: 'curl' not found in PATH "
                           "(Phase 4 uses curl as the HTTP transport; "
                           "Phase 6 swaps in a native client)");
            }
            if (exitCode != 0) {
                return err("download: curl exited " +
                           std::to_string(exitCode) + " fetching '" +
                           url->str() + "'");
            }

            std::string actualSha = sha256HexOfFile(to->str());
            if (auto wanted = params.getString("sha256")) {
                if (actualSha != wanted->str()) {
                    return err("download: sha256 mismatch for '" +
                               url->str() + "': expected " +
                               wanted->str() + ", got " + actualSha);
                }
            }

            ActionResult r;
            r.outputs["path"]   = to->str();
            r.outputs["sha256"] = actualSha;
            return r;
        }
    };

    std::unique_ptr<Action> makeDownloadAction() {
        return std::make_unique<DownloadAction>();
    }

} // namespace cajeta::buildtool
