// `publish` — speak the cajeta repository POST endpoint to upload
// a built archive + its metadata. The wire shape is a multipart
// form with the archive bytes + a JSON metadata blob describing
// the package identity (name, version, sha256). Phase 9 v1 targets
// the v2 protocol endpoint `/v2/publish`.
//
// Inputs:
//   archive   (required) path to the .cja
//   url       (required) registry URL (publish endpoint base; we
//             append `/v2/publish` unless it already ends in
//             `/publish`)
//   name      (required) package name
//   version   (required) package version
//   auth      (optional) "Bearer <token>" header forwarded as-is
//   signature (optional) path to the detached signature file —
//             when present, also forwarded as the `signature` form
//             field
//   retries   (optional) maxAttempts (default 3)
//
// Outputs:
//   url       the publish URL that was POSTed
//   status    HTTP response status code
//   key-id    forwarded from signature action if upstream
//   sha256    sha256 of the archive bytes (re-confirmed)

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Retry.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <map>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        size_t writeToString(char* p, size_t s, size_t n, void* ud) {
            static_cast<std::string*>(ud)->append(p, s * n);
            return s * n;
        }

        std::string sha256OfFile(const std::string& path) {
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

        std::string ensurePublishPath(const std::string& base) {
            if (base.size() >= 8 &&
                base.compare(base.size() - 8, 8, "/publish") == 0) {
                return base;
            }
            std::string trimmed = base;
            while (!trimmed.empty() && trimmed.back() == '/') {
                trimmed.pop_back();
            }
            return trimmed + "/v2/publish";
        }

    } // namespace

    class PublishAction : public Action {
    public:
        std::string name() const override { return "publish"; }

        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& /*ctx*/) const override {

            auto archiveV = params.getString("archive");
            if (!archiveV) return err("publish: missing required 'archive'");
            auto urlV     = params.getString("url");
            if (!urlV) return err("publish: missing required 'url'");
            auto nameV    = params.getString("name");
            if (!nameV) return err("publish: missing required 'name'");
            auto versionV = params.getString("version");
            if (!versionV) return err("publish: missing required 'version'");

            std::string archive = archiveV->str();
            namespace fs = std::filesystem;
            std::error_code ec;
            if (!fs::exists(archive, ec)) {
                return err("publish: archive '" + archive + "' not found");
            }

            std::string publishUrl = ensurePublishPath(urlV->str());
            std::string sha = sha256OfFile(archive);

            // Metadata blob in the JSON-shaped `metadata` form field
            // — the v2 endpoint also accepts top-level name/version/
            // sha256 form fields directly, so we send both for
            // backward compat.
            llvm::json::Object meta{
                {"name",    nameV->str()},
                {"version", versionV->str()},
                {"sha256",  sha},
            };
            std::string metadataJson;
            {
                llvm::raw_string_ostream os(metadataJson);
                os << llvm::json::Value(std::move(meta));
            }

            std::string signature;
            if (auto v = params.getString("signature")) {
                signature = v->str();
            }
            std::string keyId;
            if (auto v = params.getString("key-id")) {
                keyId = v->str();
            }
            std::string authHeader;
            if (auto v = params.getString("auth")) {
                authHeader = v->str();
            }

            RetryPolicy policy;
            policy.isTransient = defaultNetworkTransient;
            if (auto v = params.getInteger("retries")) {
                policy.maxAttempts =
                    std::max<int>(1, static_cast<int>(*v));
            }

            std::string respBody;
            long httpStatus = 0;

            auto attempt = [&]() -> llvm::Expected<long> {
                CURL* curl = ::curl_easy_init();
                if (!curl) return err("publish: curl_easy_init failed");
                curl_mime* mime = ::curl_mime_init(curl);

                // archive
                {
                    auto* p = ::curl_mime_addpart(mime);
                    ::curl_mime_name(p, "archive");
                    ::curl_mime_filedata(p, archive.c_str());
                }
                // name / version / sha256 — both nested and flat.
                {
                    auto* p = ::curl_mime_addpart(mime);
                    ::curl_mime_name(p, "metadata");
                    ::curl_mime_data(p, metadataJson.c_str(),
                                     CURL_ZERO_TERMINATED);
                }
                {
                    auto* p = ::curl_mime_addpart(mime);
                    ::curl_mime_name(p, "name");
                    ::curl_mime_data(p, nameV->str().c_str(),
                                     CURL_ZERO_TERMINATED);
                }
                {
                    auto* p = ::curl_mime_addpart(mime);
                    ::curl_mime_name(p, "version");
                    ::curl_mime_data(p, versionV->str().c_str(),
                                     CURL_ZERO_TERMINATED);
                }
                {
                    auto* p = ::curl_mime_addpart(mime);
                    ::curl_mime_name(p, "sha256");
                    ::curl_mime_data(p, sha.c_str(),
                                     CURL_ZERO_TERMINATED);
                }
                if (!signature.empty()) {
                    auto* p = ::curl_mime_addpart(mime);
                    ::curl_mime_name(p, "signature");
                    ::curl_mime_filedata(p, signature.c_str());
                }
                if (!keyId.empty()) {
                    auto* p = ::curl_mime_addpart(mime);
                    ::curl_mime_name(p, "key-id");
                    ::curl_mime_data(p, keyId.c_str(),
                                     CURL_ZERO_TERMINATED);
                }

                respBody.clear();
                ::curl_easy_setopt(curl, CURLOPT_URL, publishUrl.c_str());
                ::curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
                ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                                   writeToString);
                ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respBody);
                ::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
                ::curl_easy_setopt(curl, CURLOPT_USERAGENT, "cajeta/0.5");

                curl_slist* slist = nullptr;
                if (!authHeader.empty()) {
                    std::string h = "Authorization: " + authHeader;
                    slist = ::curl_slist_append(slist, h.c_str());
                    ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
                }
                CURLcode rc = ::curl_easy_perform(curl);
                ::curl_mime_free(mime);
                if (slist) ::curl_slist_free_all(slist);
                if (rc != CURLE_OK) {
                    std::string msg = std::string("publish: ") +
                                      publishUrl + " failed: " +
                                      ::curl_easy_strerror(rc);
                    int code = static_cast<int>(rc);
                    ::curl_easy_cleanup(curl);
                    return err("[curl=" + std::to_string(code) + "] " + msg);
                }
                long st = 0;
                ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &st);
                ::curl_easy_cleanup(curl);
                if (!(st >= 200 && st < 300)) {
                    return err("publish: " + publishUrl +
                               " returned (status=" +
                               std::to_string(st) + ")");
                }
                httpStatus = st;
                return st;
            };

            auto r = retryWithBackoff(policy, attempt);
            if (!r) return r.takeError();

            ActionResult result;
            result.outputs["url"]     = publishUrl;
            result.outputs["status"]  = std::to_string(httpStatus);
            result.outputs["sha256"]  = sha;
            result.outputs["name"]    = nameV->str();
            result.outputs["version"] = versionV->str();
            if (!keyId.empty()) result.outputs["key-id"] = keyId;
            return result;
        }
    };

    std::unique_ptr<Action> makePublishAction() {
        return std::make_unique<PublishAction>();
    }

} // namespace cajeta::buildtool
