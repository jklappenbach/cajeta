// `upload` — push an artifact to a remote endpoint. Phase 9 ships:
//
//   target: http     PUT or POST(multipart) via libcurl
//   target: s3       wraps `aws s3 cp` (CLI dependency)
//   target: azure    wraps `az storage blob upload`
//   target: gcs      wraps `gsutil cp`
//   target: sftp     `curl --upload-file sftp://…` (libssh2-backed curl)
//
// `also` array uploads sidecar files (e.g. the .sig next to a .cja)
// in one action invocation. Substitution + retry are uniform across
// every transport.

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Retry.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <curl/curl.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cajeta/buildtool/Subprocess.h"

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // Curl write-discard sink — many of these endpoints return
        // tiny responses we don't care about.
        size_t writeToString(char* p, size_t s, size_t n, void* ud) {
            static_cast<std::string*>(ud)->append(p, s * n);
            return s * n;
        }

        // Read a file's bytes into memory; for any file the upload
        // action handles, the manageable size fits comfortably.
        llvm::Expected<std::string> readFile(const std::string& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                return err("upload: cannot open '" + path + "'");
            }
            std::ostringstream ss; ss << in.rdbuf();
            return ss.str();
        }

        // Run a child process; surface argv[0] in failure messages.
        llvm::Expected<int> runChild(
            const std::vector<std::string>& argv) {
            SubprocessOptions so;
            so.argv = argv;
            SubprocessResult res = runSubprocess(so);
            if (!res.launched) {
                return err("upload: cannot exec '" +
                           (argv.empty() ? std::string() : argv[0]) +
                           "': " + res.error);
            }
            return res.code();
        }

        // HTTP PUT a body to a URL via libcurl. Caller pre-substitutes
        // any ${env.X} references in url + headers.
        llvm::Expected<long> httpPut(
            const std::string& url,
            const std::string& body,
            const std::map<std::string, std::string>& headers) {
            CURL* curl = ::curl_easy_init();
            if (!curl) return err("upload: curl_easy_init failed");
            std::string respBody;
            ::curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            ::curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
            ::curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
            ::curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                               static_cast<curl_off_t>(body.size()));
            // Use a read callback over the buffered body.
            struct Cursor { const std::string* s; size_t pos; };
            Cursor cur{&body, 0};
            ::curl_easy_setopt(curl, CURLOPT_READDATA, &cur);
            ::curl_easy_setopt(curl, CURLOPT_READFUNCTION,
                +[](char* p, size_t s, size_t n, void* ud) -> size_t {
                    auto* c = static_cast<Cursor*>(ud);
                    size_t want = s * n;
                    size_t left = c->s->size() - c->pos;
                    size_t cnt = std::min(want, left);
                    std::memcpy(p, c->s->data() + c->pos, cnt);
                    c->pos += cnt;
                    return cnt;
                });
            ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
            ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respBody);
            ::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
            ::curl_easy_setopt(curl, CURLOPT_USERAGENT, "cajeta/0.5");

            curl_slist* slist = nullptr;
            for (const auto& kv : headers) {
                std::string h = kv.first + ": " + kv.second;
                slist = ::curl_slist_append(slist, h.c_str());
            }
            if (slist) {
                ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
            }
            CURLcode rc = ::curl_easy_perform(curl);
            if (slist) ::curl_slist_free_all(slist);
            if (rc != CURLE_OK) {
                int code = static_cast<int>(rc);
                std::string msg = std::string("PUT ") + url +
                                  " failed: " + ::curl_easy_strerror(rc);
                ::curl_easy_cleanup(curl);
                // Encode curl-code in the error so the retry layer
                // can classify.
                std::string tag = "[curl=" + std::to_string(code) + "] ";
                return err(tag + msg);
            }
            long status = 0;
            ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            ::curl_easy_cleanup(curl);
            return status;
        }

        // HTTP POST a multipart form. Uploads the file as "file"
        // by default; caller can override via `field-name`. Extra
        // form fields come from `form-fields`.
        llvm::Expected<long> httpPostMultipart(
            const std::string& url,
            const std::string& filePath,
            const std::string& fieldName,
            const std::map<std::string, std::string>& formFields,
            const std::map<std::string, std::string>& headers) {
            CURL* curl = ::curl_easy_init();
            if (!curl) return err("upload: curl_easy_init failed");
            curl_mime* mime = ::curl_mime_init(curl);

            curl_mimepart* filePart = ::curl_mime_addpart(mime);
            ::curl_mime_name(filePart, fieldName.c_str());
            ::curl_mime_filedata(filePart, filePath.c_str());

            for (const auto& kv : formFields) {
                curl_mimepart* p = ::curl_mime_addpart(mime);
                ::curl_mime_name(p, kv.first.c_str());
                ::curl_mime_data(p, kv.second.c_str(),
                                 CURL_ZERO_TERMINATED);
            }

            std::string respBody;
            ::curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            ::curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
            ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
            ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respBody);
            ::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
            ::curl_easy_setopt(curl, CURLOPT_USERAGENT, "cajeta/0.5");

            curl_slist* slist = nullptr;
            for (const auto& kv : headers) {
                std::string h = kv.first + ": " + kv.second;
                slist = ::curl_slist_append(slist, h.c_str());
            }
            if (slist) ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
            CURLcode rc = ::curl_easy_perform(curl);
            ::curl_mime_free(mime);
            if (slist) ::curl_slist_free_all(slist);

            if (rc != CURLE_OK) {
                int code = static_cast<int>(rc);
                std::string msg = std::string("POST(multipart) ") +
                                  url + " failed: " +
                                  ::curl_easy_strerror(rc);
                ::curl_easy_cleanup(curl);
                std::string tag = "[curl=" + std::to_string(code) + "] ";
                return err(tag + msg);
            }
            long status = 0;
            ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            ::curl_easy_cleanup(curl);
            return status;
        }

        // SFTP via curl. libcurl-with-libssh2 understands sftp://
        // URLs; --upload-file is implicit via CURLOPT_UPLOAD. The
        // user / password come from the URL form sftp://user@host/path
        // OR via key-path param (CURLOPT_SSH_PRIVATE_KEYFILE).
        llvm::Expected<long> sftpUpload(
            const std::string& url,
            const std::string& filePath,
            const std::string& sshKeyPath) {
            CURL* curl = ::curl_easy_init();
            if (!curl) return err("upload: curl_easy_init failed");
            std::ifstream in(filePath, std::ios::binary);
            if (!in) {
                ::curl_easy_cleanup(curl);
                return err("upload(sftp): cannot read '" + filePath + "'");
            }
            std::ostringstream ss; ss << in.rdbuf();
            std::string body = ss.str();

            ::curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            ::curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
            ::curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                               static_cast<curl_off_t>(body.size()));
            struct Cursor { const std::string* s; size_t pos; };
            Cursor cur{&body, 0};
            ::curl_easy_setopt(curl, CURLOPT_READDATA, &cur);
            ::curl_easy_setopt(curl, CURLOPT_READFUNCTION,
                +[](char* p, size_t s, size_t n, void* ud) -> size_t {
                    auto* c = static_cast<Cursor*>(ud);
                    size_t want = s * n;
                    size_t left = c->s->size() - c->pos;
                    size_t cnt = std::min(want, left);
                    std::memcpy(p, c->s->data() + c->pos, cnt);
                    c->pos += cnt;
                    return cnt;
                });
            if (!sshKeyPath.empty()) {
                ::curl_easy_setopt(curl,
                    CURLOPT_SSH_PRIVATE_KEYFILE, sshKeyPath.c_str());
            }
            ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
            CURLcode rc = ::curl_easy_perform(curl);
            if (rc != CURLE_OK) {
                int code = static_cast<int>(rc);
                std::string msg = std::string("SFTP ") + url +
                                  " failed: " + ::curl_easy_strerror(rc);
                ::curl_easy_cleanup(curl);
                std::string tag = "[curl=" + std::to_string(code) + "] ";
                return err(tag + msg);
            }
            ::curl_easy_cleanup(curl);
            return 200L;
        }

        // S3 / Azure / GCS shell-out wrappers. The acceptance is
        // "artifact + signature upload to <cloud>; URLs consumable by
        // downstream actions." Cloud-specific auth lives in the
        // canonical CLI tools — implementing sigv4 / shared-key /
        // service-account JWTs from scratch buys nothing here.
        llvm::Expected<std::string> awsS3Put(
            const std::string& filePath,
            const std::string& bucket,
            const std::string& key) {
            std::vector<std::string> argv{
                "aws", "s3", "cp", filePath,
                "s3://" + bucket + "/" + key,
            };
            auto code = runChild(argv);
            if (!code) return code.takeError();
            if (*code != 0) {
                return err("upload(s3): `aws s3 cp` exited " +
                           std::to_string(*code) +
                           " (is the AWS CLI installed + configured?)");
            }
            return std::string("s3://" + bucket + "/" + key);
        }

        llvm::Expected<std::string> azureBlobPut(
            const std::string& filePath,
            const std::string& account,
            const std::string& container,
            const std::string& blobName) {
            std::vector<std::string> argv{
                "az", "storage", "blob", "upload",
                "--account-name", account,
                "--container-name", container,
                "--name", blobName,
                "--file", filePath,
                "--overwrite",
            };
            auto code = runChild(argv);
            if (!code) return code.takeError();
            if (*code != 0) {
                return err("upload(azure): `az storage blob upload` "
                           "exited " + std::to_string(*code) +
                           " (is the Azure CLI installed + logged in?)");
            }
            return std::string("https://" + account +
                ".blob.core.windows.net/" + container + "/" + blobName);
        }

        llvm::Expected<std::string> gcsPut(
            const std::string& filePath,
            const std::string& bucket,
            const std::string& key) {
            std::vector<std::string> argv{
                "gsutil", "cp", filePath,
                "gs://" + bucket + "/" + key,
            };
            auto code = runChild(argv);
            if (!code) return code.takeError();
            if (*code != 0) {
                return err("upload(gcs): `gsutil cp` exited " +
                           std::to_string(*code) +
                           " (is the gcloud CLI installed + "
                           "authenticated?)");
            }
            return std::string("gs://" + bucket + "/" + key);
        }

        std::map<std::string, std::string> readStringMap(
            const llvm::json::Object& params, const std::string& key) {
            std::map<std::string, std::string> out;
            if (const auto* obj = params.getObject(key)) {
                for (const auto& kv : *obj) {
                    if (auto s = kv.second.getAsString())
                        out[kv.first.str()] = s->str();
                }
            }
            return out;
        }

    } // namespace

    class UploadAction : public Action {
    public:
        std::string name() const override { return "upload"; }

        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& /*ctx*/) const override {

            auto targetV = params.getString("target");
            if (!targetV) return err("upload: missing required 'target'");
            std::string target = targetV->str();

            auto inputV = params.getString("input");
            if (!inputV) return err("upload: missing required 'input'");
            std::string input = inputV->str();

            // Build the list of files to upload — `input` + every
            // `also` entry. `also` may carry either bare filename
            // strings (relative to input's dir) or {file, ...}
            // objects with destination overrides.
            struct ToUpload {
                std::string localPath;
                std::string urlOverride; // applied as full URL when set
            };
            std::vector<ToUpload> files;
            files.push_back({input, {}});
            if (const auto* arr = params.getArray("also")) {
                namespace fs = std::filesystem;
                fs::path inputDir = fs::path(input).parent_path();
                for (const auto& v : *arr) {
                    if (auto s = v.getAsString()) {
                        std::string p = s->str();
                        if (!fs::path(p).is_absolute()) {
                            p = (inputDir / p).string();
                        }
                        files.push_back({p, {}});
                    } else if (const auto* o = v.getAsObject()) {
                        if (auto f = o->getString("file")) {
                            std::string p = f->str();
                            if (!fs::path(p).is_absolute()) {
                                p = (inputDir / p).string();
                            }
                            ToUpload e{p, {}};
                            if (auto u = o->getString("url"))
                                e.urlOverride = u->str();
                            files.push_back(std::move(e));
                        }
                    }
                }
            }

            RetryPolicy policy;
            if (auto v = params.getInteger("retries")) {
                policy.maxAttempts =
                    std::max<int>(1, static_cast<int>(*v));
            }
            policy.isTransient = defaultNetworkTransient;

            ActionResult result;
            std::vector<std::string> uploadedUrls;

            for (const auto& f : files) {
                std::string finalUrl;

                if (target == "http" || target == "https") {
                    auto urlV = params.getString("url");
                    if (!urlV) return err(
                        "upload(http): missing required 'url'");
                    std::string url = !f.urlOverride.empty()
                        ? f.urlOverride : urlV->str();

                    std::string method = "PUT";
                    if (auto v = params.getString("method")) {
                        std::string m = v->str();
                        for (auto& c : m) c = ::toupper(c);
                        method = m;
                    }
                    auto headers = readStringMap(params, "headers");

                    if (method == "PUT") {
                        auto body = readFile(f.localPath);
                        if (!body) return body.takeError();
                        auto status = retryWithBackoff(policy,
                          [&]() -> llvm::Expected<long> {
                            auto s = httpPut(url, *body, headers);
                            if (!s) return s.takeError();
                            if (!(*s >= 200 && *s < 300)) {
                                return err("upload(http PUT): " + url +
                                    " returned (status=" +
                                    std::to_string(*s) + ")");
                            }
                            return *s;
                          });
                        if (!status) return status.takeError();
                    } else if (method == "POST") {
                        std::string fieldName = "file";
                        if (auto v = params.getString("field-name"))
                            fieldName = v->str();
                        auto formFields = readStringMap(
                            params, "form-fields");
                        auto status = retryWithBackoff(policy,
                          [&]() -> llvm::Expected<long> {
                            auto s = httpPostMultipart(
                                url, f.localPath, fieldName,
                                formFields, headers);
                            if (!s) return s.takeError();
                            if (!(*s >= 200 && *s < 300)) {
                                return err(
                                    "upload(http POST): " + url +
                                    " returned (status=" +
                                    std::to_string(*s) + ")");
                            }
                            return *s;
                          });
                        if (!status) return status.takeError();
                    } else {
                        return err("upload(http): 'method' must be "
                                   "PUT or POST (got '" + method + "')");
                    }
                    finalUrl = url;
                }
                else if (target == "s3") {
                    auto bucket = params.getString("bucket");
                    auto key    = params.getString("key");
                    if (!bucket || !key) return err(
                        "upload(s3): missing 'bucket' or 'key'");
                    auto u = retryWithBackoff(policy,
                      [&]() -> llvm::Expected<std::string> {
                        return awsS3Put(f.localPath, bucket->str(),
                                        key->str());
                      });
                    if (!u) return u.takeError();
                    finalUrl = *u;
                }
                else if (target == "azure") {
                    auto account = params.getString("account");
                    auto container = params.getString("container");
                    auto blob = params.getString("blob");
                    if (!account || !container || !blob) return err(
                        "upload(azure): missing 'account', "
                        "'container', or 'blob'");
                    auto u = retryWithBackoff(policy,
                      [&]() -> llvm::Expected<std::string> {
                        return azureBlobPut(f.localPath,
                            account->str(), container->str(),
                            blob->str());
                      });
                    if (!u) return u.takeError();
                    finalUrl = *u;
                }
                else if (target == "gcs") {
                    auto bucket = params.getString("bucket");
                    auto key    = params.getString("key");
                    if (!bucket || !key) return err(
                        "upload(gcs): missing 'bucket' or 'key'");
                    auto u = retryWithBackoff(policy,
                      [&]() -> llvm::Expected<std::string> {
                        return gcsPut(f.localPath, bucket->str(),
                                      key->str());
                      });
                    if (!u) return u.takeError();
                    finalUrl = *u;
                }
                else if (target == "sftp") {
                    auto urlV = params.getString("url");
                    if (!urlV) return err(
                        "upload(sftp): missing required 'url' "
                        "(sftp://user@host/path)");
                    std::string url = !f.urlOverride.empty()
                        ? f.urlOverride : urlV->str();
                    std::string keyPath;
                    if (auto v = params.getString("key-path"))
                        keyPath = v->str();
                    auto status = retryWithBackoff(policy,
                      [&]() -> llvm::Expected<long> {
                        return sftpUpload(url, f.localPath, keyPath);
                      });
                    if (!status) return status.takeError();
                    finalUrl = url;
                }
                else {
                    return err("upload: unknown target '" + target +
                               "' (supported: http, s3, azure, gcs, sftp)");
                }
                uploadedUrls.push_back(finalUrl);
            }

            // Outputs: `url` for the primary, `urls` JSON-ish for
            // every uploaded file (semicolon-separated, easy to
            // consume downstream via substitution).
            result.outputs["url"] = uploadedUrls.empty()
                ? std::string{} : uploadedUrls.front();
            {
                std::string joined;
                for (const auto& u : uploadedUrls) {
                    if (!joined.empty()) joined += ";";
                    joined += u;
                }
                result.outputs["urls"]  = joined;
                result.outputs["count"] = std::to_string(uploadedUrls.size());
            }
            result.outputs["target"] = target;
            return result;
        }
    };

    std::unique_ptr<Action> makeUploadAction() {
        return std::make_unique<UploadAction>();
    }

} // namespace cajeta::buildtool
