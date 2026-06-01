#include "cajeta/buildtool/repo/HttpRepository.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <curl/curl.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // curl_global_init / cleanup pair. libcurl docs say to call
        // once per process before any handles are created. We
        // serialize the first-call init via a once-flag; cleanup
        // runs at static-destructor time. (HttpRepository instances
        // are constructed lazily from the resolver, so we can't rely
        // on a single fixed init site.)
        std::once_flag g_curlInitOnce;
        void initCurlOnce() {
            std::call_once(g_curlInitOnce, []() {
                ::curl_global_init(CURL_GLOBAL_DEFAULT);
                std::atexit([]() { ::curl_global_cleanup(); });
            });
        }

        size_t writeToString(char* ptr, size_t size, size_t nmemb,
                             void* userdata) {
            auto* out = static_cast<std::string*>(userdata);
            size_t bytes = size * nmemb;
            out->append(ptr, bytes);
            return bytes;
        }

        size_t writeToStream(char* ptr, size_t size, size_t nmemb,
                             void* userdata) {
            auto* out = static_cast<std::ofstream*>(userdata);
            size_t bytes = size * nmemb;
            out->write(ptr, static_cast<std::streamsize>(bytes));
            return out->good() ? bytes : 0;
        }

        std::string joinUrl(const std::string& base,
                            const std::string& rest) {
            // base already has no trailing slash by construction;
            // rest may or may not have a leading slash.
            if (rest.empty()) return base;
            if (rest.front() == '/') return base + rest;
            return base + "/" + rest;
        }

        // Resolve the bearer token: literal wins over env var (caller
        // has already verified at least one is set). Returns empty
        // when no token is configured.
        std::string resolveBearerToken(const RepositoryAuth& auth) {
            if (!auth.tokenLiteral.empty()) return auth.tokenLiteral;
            if (!auth.tokenEnv.empty()) {
                if (const char* v = std::getenv(auth.tokenEnv.c_str())) {
                    return std::string(v);
                }
            }
            return "";
        }

    } // namespace

    struct HttpRepository::State {
        CURL* curl = nullptr;
        State() {
            curl = ::curl_easy_init();
        }
        ~State() {
            if (curl) ::curl_easy_cleanup(curl);
        }
    };

    HttpRepository::HttpRepository(std::string name,
                                   std::string baseUrl,
                                   RepositoryAuth auth,
                                   std::string cacheDir)
        : name_(std::move(name)),
          baseUrl_(std::move(baseUrl)),
          auth_(std::move(auth)),
          cacheDir_(std::move(cacheDir)),
          state_(std::make_unique<State>()) {
        initCurlOnce();
        // Strip trailing slashes so joinUrl produces a clean path.
        while (!baseUrl_.empty() && baseUrl_.back() == '/') {
            baseUrl_.pop_back();
        }
    }

    HttpRepository::~HttpRepository() = default;

    namespace {

        // Configure auth headers / mTLS material on a curl handle.
        // Caller owns the slist and must free it after curl_easy_perform
        // returns.
        curl_slist* applyAuth(CURL* curl, const RepositoryAuth& auth) {
            curl_slist* headers = nullptr;
            if (auth.type == "bearer") {
                std::string tok = resolveBearerToken(auth);
                if (!tok.empty()) {
                    std::string h = "Authorization: Bearer " + tok;
                    headers = ::curl_slist_append(headers, h.c_str());
                }
            } else if (auth.type == "mtls") {
                ::curl_easy_setopt(curl, CURLOPT_SSLCERT,
                                   auth.clientCertPath.c_str());
                ::curl_easy_setopt(curl, CURLOPT_SSLKEY,
                                   auth.clientKeyPath.c_str());
                if (!auth.caCertPath.empty()) {
                    ::curl_easy_setopt(curl, CURLOPT_CAINFO,
                                       auth.caCertPath.c_str());
                }
            }
            return headers;
        }

        // Run a GET that buffers the body into `bodyOut`. Returns the
        // HTTP response code; on transport-level failure, returns an
        // error citing the curl message.
        llvm::Expected<long> getToString(CURL* curl,
                                         const std::string& url,
                                         const RepositoryAuth& auth,
                                         std::string& bodyOut) {
            ::curl_easy_reset(curl);
            ::curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
            ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bodyOut);
            ::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            ::curl_easy_setopt(curl, CURLOPT_USERAGENT, "cajeta/0.5");
            // 30s default — fast feedback when a registry is broken.
            ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

            curl_slist* headers = applyAuth(curl, auth);
            if (headers) {
                ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            }
            CURLcode rc = ::curl_easy_perform(curl);
            if (headers) ::curl_slist_free_all(headers);
            if (rc != CURLE_OK) {
                return err(std::string("HTTP GET ") + url + " failed: " +
                           ::curl_easy_strerror(rc));
            }
            long code = 0;
            ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            return code;
        }

        // Run a GET that streams the body to a file. Path is created
        // (parents made via the caller).
        llvm::Expected<long> getToFile(CURL* curl,
                                       const std::string& url,
                                       const RepositoryAuth& auth,
                                       const std::string& destPath) {
            ::curl_easy_reset(curl);
            std::ofstream out(destPath,
                              std::ios::binary | std::ios::trunc);
            if (!out) {
                return err("cannot open '" + destPath + "' for writing");
            }
            ::curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToStream);
            ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
            ::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            ::curl_easy_setopt(curl, CURLOPT_USERAGENT, "cajeta/0.5");
            ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);  // 5 min

            curl_slist* headers = applyAuth(curl, auth);
            if (headers) {
                ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            }
            CURLcode rc = ::curl_easy_perform(curl);
            if (headers) ::curl_slist_free_all(headers);
            out.close();
            if (rc != CURLE_OK) {
                return err(std::string("HTTP GET ") + url + " failed: " +
                           ::curl_easy_strerror(rc));
            }
            long code = 0;
            ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            return code;
        }

    } // namespace

    llvm::Expected<std::vector<std::string>>
    HttpRepository::listVersions(const std::string& packageName) const {
        std::vector<std::string> out;
        std::string url = joinUrl(baseUrl_, packageName + "/versions.json");
        std::string body;
        auto code = getToString(state_->curl, url, auth_, body);
        if (!code) return code.takeError();
        if (*code == 404) return out;  // not in this repo — caller falls through
        if (*code < 200 || *code >= 300) {
            return err("HTTP " + std::to_string(*code) + " from " + url);
        }
        auto val = llvm::json::parse(body);
        if (!val) {
            return err("malformed versions.json from " + url +
                       ": " + llvm::toString(val.takeError()));
        }
        const auto* obj = val->getAsObject();
        if (!obj) {
            return err("versions.json from " + url +
                       " is not a JSON object");
        }
        const auto* arr = obj->getArray("versions");
        if (!arr) {
            return err("versions.json from " + url +
                       " missing 'versions' array");
        }
        out.reserve(arr->size());
        for (const auto& v : *arr) {
            if (auto s = v.getAsString()) {
                out.emplace_back(s->str());
            } else {
                return err("versions.json from " + url +
                           " has a non-string version entry");
            }
        }
        return out;
    }

    llvm::Expected<std::string> HttpRepository::fetch(
        const std::string& packageName,
        const std::string& version) const {
        namespace fs = std::filesystem;
        fs::create_directories(cacheDir_);
        std::string filename = packageName + "-" + version + ".cja";
        std::string url = joinUrl(baseUrl_, packageName + "/" + version +
                                  "/" + filename);
        fs::path dest = fs::path(cacheDir_) / filename;

        auto code = getToFile(state_->curl, url, auth_, dest.string());
        if (!code) {
            std::error_code ec; fs::remove(dest, ec);
            return code.takeError();
        }
        if (*code < 200 || *code >= 300) {
            std::error_code ec; fs::remove(dest, ec);
            return err("HTTP " + std::to_string(*code) + " fetching " + url);
        }
        return dest.string();
    }

    llvm::Expected<std::optional<std::string>>
    HttpRepository::fetchManifestJson(
        const std::string& packageName,
        const std::string& version) const {
        // Spec exposes the sidecar at /<name>/<version>/manifest.json
        // (the filesystem repo uses /cajeta.json; on the wire the
        // canonical endpoint name is manifest.json — content is the
        // same JSON).
        std::string url = joinUrl(baseUrl_, packageName + "/" + version +
                                  "/manifest.json");
        std::string body;
        auto code = getToString(state_->curl, url, auth_, body);
        if (!code) return code.takeError();
        if (*code == 404) {
            // Pre-sidecar artifact — let the walker treat this dep
            // as a leaf, same as the filesystem-repo no-sidecar path.
            return std::optional<std::string>{};
        }
        if (*code < 200 || *code >= 300) {
            return err("HTTP " + std::to_string(*code) +
                       " fetching manifest from " + url);
        }
        return std::optional<std::string>(body);
    }

} // namespace cajeta::buildtool
