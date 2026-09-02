#include "cajeta/buildtool/repo/HttpRepository.h"
#include "cajeta/buildtool/repo/TarZstd.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <curl/curl.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

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
        // Capability-probe cache (Phase 6d). Once probed we keep
        // the result + the wall-clock expiry; subsequent calls
        // return the cached value until expiry. A 404 from the
        // well-known endpoint counts as "v1-only, never re-probe
        // until TTL elapses".
        mutable std::mutex capMu;
        mutable std::optional<RepoCapabilities> cap;
        mutable std::chrono::steady_clock::time_point capExpiry{};
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

        // Run a POST whose body is `requestBody` (mime type
        // `contentType`); buffer the response body into `bodyOut`.
        llvm::Expected<long> postToString(CURL* curl,
                                          const std::string& url,
                                          const RepositoryAuth& auth,
                                          const std::string& requestBody,
                                          const std::string& contentType,
                                          std::string& bodyOut) {
            ::curl_easy_reset(curl);
            ::curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            ::curl_easy_setopt(curl, CURLOPT_POST, 1L);
            ::curl_easy_setopt(curl, CURLOPT_POSTFIELDS,
                               requestBody.data());
            ::curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                               static_cast<long>(requestBody.size()));
            ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
            ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bodyOut);
            ::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            ::curl_easy_setopt(curl, CURLOPT_USERAGENT, "cajeta/0.5");
            ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);  // 5 min

            curl_slist* headers = applyAuth(curl, auth);
            std::string ctHeader = "Content-Type: " + contentType;
            headers = ::curl_slist_append(headers, ctHeader.c_str());
            ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            CURLcode rc = ::curl_easy_perform(curl);
            if (headers) ::curl_slist_free_all(headers);
            if (rc != CURLE_OK) {
                return err(std::string("HTTP POST ") + url + " failed: " +
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

    // ─── Phase 6d capability probe ────────────────────────────────

    llvm::Expected<RepoCapabilities> parseCapabilitiesJson(
        const std::string& jsonBody) {
        auto val = llvm::json::parse(jsonBody);
        if (!val) {
            return err("malformed cajeta-capabilities.json: " +
                       llvm::toString(val.takeError()));
        }
        const auto* obj = val->getAsObject();
        if (!obj) {
            return err("cajeta-capabilities.json is not a JSON object");
        }
        RepoCapabilities cap;
        cap.probed = true;
        if (const auto* arr = obj->getArray("protocol-versions")) {
            for (const auto& v : *arr) {
                if (auto s = v.getAsString()) {
                    cap.protocolVersions.emplace_back(s->str());
                }
            }
        }
        if (auto b = obj->getBoolean("bundle")) cap.bundle = *b;
        if (auto b = obj->getBoolean("revocation")) cap.revocation = *b;
        if (auto b = obj->getBoolean("content-addressed")) {
            cap.contentAddressed = *b;
        }
        if (auto s = obj->getString("transparency-log")) {
            cap.transparencyLogUrl = s->str();
        }
        if (const auto* mirrors = obj->getArray("mirrors")) {
            for (const auto& m : *mirrors) {
                const auto* mo = m.getAsObject();
                if (!mo) continue;
                RepoCapabilities::Mirror mi;
                if (auto s = mo->getString("url")) mi.url = s->str();
                if (auto s = mo->getString("region")) mi.region = s->str();
                if (!mi.url.empty()) cap.mirrors.push_back(std::move(mi));
            }
        }
        if (auto ttl = obj->getInteger("ttl")) {
            cap.ttl = std::chrono::seconds(*ttl);
        }
        return cap;
    }

    void HttpRepository::invalidateCapabilityCache() const {
        std::lock_guard<std::mutex> lk(state_->capMu);
        state_->cap.reset();
        state_->capExpiry = {};
    }

    llvm::Expected<RepoCapabilities> HttpRepository::capabilities() const {
        {
            std::lock_guard<std::mutex> lk(state_->capMu);
            if (state_->cap.has_value() &&
                std::chrono::steady_clock::now() < state_->capExpiry) {
                return *state_->cap;
            }
        }
        std::string url = joinUrl(baseUrl_,
                                  ".well-known/cajeta-capabilities.json");
        std::string body;
        auto code = getToString(state_->curl, url, auth_, body);
        if (!code) return code.takeError();
        RepoCapabilities cap;
        if (*code == 404) {
            // v1-only server — record + cache so we don't probe
            // every call. probed=true signals "asked and answered".
            cap.probed = true;
        } else if (*code >= 200 && *code < 300) {
            auto parsed = parseCapabilitiesJson(body);
            if (!parsed) return parsed.takeError();
            cap = std::move(*parsed);
        } else {
            return err("HTTP " + std::to_string(*code) +
                       " probing capabilities at " + url);
        }
        {
            std::lock_guard<std::mutex> lk(state_->capMu);
            state_->cap = cap;
            state_->capExpiry =
                std::chrono::steady_clock::now() + cap.ttl;
        }
        return cap;
    }

    // ─── Phase 6d /v2/resolve + /v2/blob ─────────────────────────

    llvm::Expected<ResolveMetadata> HttpRepository::v2Resolve(
        const std::string& packageName,
        const std::string& version) const {
        std::string url = joinUrl(baseUrl_,
            "v2/resolve?name=" + packageName + "&version=" + version);
        std::string body;
        auto code = getToString(state_->curl, url, auth_, body);
        if (!code) return code.takeError();
        if (*code < 200 || *code >= 300) {
            return err("HTTP " + std::to_string(*code) + " from " + url);
        }
        auto val = llvm::json::parse(body);
        if (!val) {
            return err("malformed /v2/resolve response from " + url +
                       ": " + llvm::toString(val.takeError()));
        }
        const auto* obj = val->getAsObject();
        if (!obj) {
            return err("/v2/resolve response from " + url +
                       " is not a JSON object");
        }
        ResolveMetadata md;
        if (auto s = obj->getString("sha256")) md.sha256 = s->str();
        if (auto n = obj->getInteger("size")) md.sizeBytes = *n;
        if (const auto* deps = obj->getArray("deps")) {
            for (const auto& d : *deps) {
                const auto* o = d.getAsObject();
                if (!o) continue;
                std::string n, v;
                if (auto s = o->getString("name")) n = s->str();
                if (auto s = o->getString("version")) v = s->str();
                if (!n.empty()) md.deps.emplace_back(n, v);
            }
        }
        if (const auto* caps = obj->getArray("capabilities")) {
            for (const auto& c : *caps) {
                if (auto s = c.getAsString()) {
                    md.capabilities.emplace_back(s->str());
                }
            }
        }
        if (auto s = obj->getString("published-at")) {
            md.publishedAt = s->str();
        }
        // The UNSIGNED view of ownership. Useful for diagnostics; never
        // sufficient to bind a publisher — a mirror writes this field as
        // freely as any other. The binding reads `ReleaseMetadata`, which
        // knows whether a root signed it (publisher-trust spec 4.4, 6.2).
        if (auto s = obj->getString("organization")) {
            md.organization = s->str();
        }
        if (auto b = obj->getBoolean("retracted")) md.retracted = *b;
        if (auto s = obj->getString("retracted-reason")) {
            md.retractedReason = s->str();
        }
        if (md.sha256.empty()) {
            return err("/v2/resolve response from " + url +
                       " missing 'sha256'");
        }
        return md;
    }

    llvm::Expected<std::string> HttpRepository::v2FetchBlob(
        const std::string& sha256) const {
        namespace fs = std::filesystem;
        fs::create_directories(cacheDir_);
        // Canonical sha256 format is "sha256:<hex>"; strip prefix
        // for the URL form, keep the full string for the file name
        // so the workstation cache sees the same key the resolver
        // recorded.
        std::string urlSha = sha256;
        const std::string prefix = "sha256:";
        if (urlSha.rfind(prefix, 0) == 0) {
            urlSha = urlSha.substr(prefix.size());
        }
        std::string url = joinUrl(baseUrl_, "v2/blob/" + urlSha);
        fs::path dest = fs::path(cacheDir_) / (urlSha + ".cja");
        auto code = getToFile(state_->curl, url, auth_, dest.string());
        if (!code) {
            std::error_code ec; fs::remove(dest, ec);
            return code.takeError();
        }
        if (*code < 200 || *code >= 300) {
            std::error_code ec; fs::remove(dest, ec);
            return err("HTTP " + std::to_string(*code) +
                       " fetching blob " + url);
        }
        return dest.string();
    }

    // ─── Phase 6d /v2/bundle ──────────────────────────────────────

    namespace {

        // Encode BundleRequest to JSON for the wire.
        std::string encodeBundleRequest(const BundleRequest& req) {
            llvm::json::Array haveArr;
            for (const auto& h : req.have) {
                haveArr.emplace_back(h);
            }
            llvm::json::Array wantArr;
            for (const auto& w : req.want) {
                llvm::json::Object entry;
                entry["name"] = w.name;
                entry["version"] = w.version;
                wantArr.emplace_back(std::move(entry));
            }
            llvm::json::Object body;
            body["have"] = std::move(haveArr);
            body["want"] = std::move(wantArr);
            body["transitive"] = req.transitive;
            body["format"] = req.format;
            std::string out;
            llvm::raw_string_ostream os(out);
            os << llvm::json::Value(std::move(body));
            return out;
        }

        // Walk an unpacked tar's TarEntry list and split into the
        // bundle.json index entry + the per-artifact blob entries.
        // Writes each blob to `destDir/<sha256>.cja` and returns
        // the typed BundleResponse.
        llvm::Expected<BundleResponse> consumeBundle(
            const std::vector<TarEntry>& entries,
            const std::string& destDir) {
            namespace fs = std::filesystem;
            const TarEntry* indexEntry = nullptr;
            std::unordered_map<std::string, const TarEntry*> blobs;
            for (const auto& e : entries) {
                if (e.name == "bundle.json") {
                    indexEntry = &e;
                } else {
                    blobs[e.name] = &e;
                }
            }
            if (!indexEntry) {
                return err("v2/bundle response is missing bundle.json");
            }
            auto val = llvm::json::parse(indexEntry->data);
            if (!val) {
                return err("malformed bundle.json: " +
                           llvm::toString(val.takeError()));
            }
            const auto* obj = val->getAsObject();
            if (!obj) {
                return err("bundle.json is not a JSON object");
            }
            BundleResponse out;
            fs::create_directories(destDir);
            if (const auto* arr = obj->getArray("entries")) {
                for (const auto& e : *arr) {
                    const auto* o = e.getAsObject();
                    if (!o) continue;
                    BundleEntry be;
                    if (auto s = o->getString("name")) be.name = s->str();
                    if (auto s = o->getString("version")) {
                        be.version = s->str();
                    }
                    if (auto s = o->getString("sha256")) {
                        be.sha256 = s->str();
                    }
                    if (be.sha256.empty()) {
                        return err("bundle.json entry missing sha256");
                    }
                    std::string urlSha = be.sha256;
                    const std::string prefix = "sha256:";
                    if (urlSha.rfind(prefix, 0) == 0) {
                        urlSha = urlSha.substr(prefix.size());
                    }
                    auto it = blobs.find(urlSha + ".cja");
                    if (it == blobs.end()) {
                        return err("bundle.json names entry " +
                                   urlSha +
                                   " but no matching tar member found");
                    }
                    fs::path dest = fs::path(destDir) /
                                    (urlSha + ".cja");
                    std::ofstream os(dest, std::ios::binary);
                    if (!os) {
                        return err("cannot open " + dest.string() +
                                   " for writing");
                    }
                    os.write(it->second->data.data(),
                             static_cast<std::streamsize>(
                                 it->second->data.size()));
                    be.artifactPath = dest.string();
                    out.entries.push_back(std::move(be));
                }
            }
            if (const auto* arr = obj->getArray("omitted")) {
                for (const auto& e : *arr) {
                    if (auto s = e.getAsString()) {
                        out.omitted.emplace_back(s->str());
                    }
                }
            }
            return out;
        }

    } // namespace

    llvm::Expected<BundleResponse> HttpRepository::v2Bundle(
        const BundleRequest& req,
        const std::string& destDir) const {
        std::string url = joinUrl(baseUrl_, "v2/bundle");
        std::string body;
        std::string requestBody = encodeBundleRequest(req);
        auto code = postToString(state_->curl, url, auth_,
                                 requestBody,
                                 "application/cajeta-bundle-request+json",
                                 body);
        if (!code) return code.takeError();
        if (*code < 200 || *code >= 300) {
            return err("HTTP " + std::to_string(*code) + " from " + url);
        }
        auto entries = readTarZstd(body);
        if (!entries) return entries.takeError();
        return consumeBundle(*entries, destDir);
    }

    llvm::Expected<BundleResponse> HttpRepository::v2LockfileDiff(
        const std::string& oldLockfileSha256,
        const std::string& newLockfileSha256,
        const std::string& destDir) const {
        std::string url = joinUrl(baseUrl_, "v2/lockfile-diff");
        llvm::json::Object reqObj;
        reqObj["old-lockfile-sha256"] = oldLockfileSha256;
        reqObj["new-lockfile-sha256"] = newLockfileSha256;
        std::string requestBody;
        {
            llvm::raw_string_ostream os(requestBody);
            os << llvm::json::Value(std::move(reqObj));
        }
        std::string body;
        auto code = postToString(state_->curl, url, auth_,
                                 requestBody,
                                 "application/json",
                                 body);
        if (!code) return code.takeError();
        if (*code == 404) {
            // Server hasn't snapshotted the old lockfile — surface
            // so the caller can retry as /v2/bundle.
            return err("HTTP 404: server has no snapshot for old "
                       "lockfile sha256 — fall back to /v2/bundle");
        }
        if (*code < 200 || *code >= 300) {
            return err("HTTP " + std::to_string(*code) + " from " + url);
        }
        auto entries = readTarZstd(body);
        if (!entries) return entries.takeError();
        return consumeBundle(*entries, destDir);
    }

    // ─── Phase 6d /v2/transparency-log ────────────────────────────

    llvm::Expected<TransparencyLogEntry> HttpRepository::v2TransparencyLog(
        const std::string& sha256) const {
        std::string urlSha = sha256;
        const std::string prefix = "sha256:";
        if (urlSha.rfind(prefix, 0) == 0) {
            urlSha = urlSha.substr(prefix.size());
        }
        std::string url = joinUrl(baseUrl_, "v2/transparency-log/" + urlSha);
        std::string body;
        auto code = getToString(state_->curl, url, auth_, body);
        if (!code) return code.takeError();
        if (*code == 404) {
            return err("no transparency-log entry for " + sha256 +
                       " — refusing install (publication not attested)");
        }
        if (*code < 200 || *code >= 300) {
            return err("HTTP " + std::to_string(*code) + " from " + url);
        }
        auto val = llvm::json::parse(body);
        if (!val) {
            return err("malformed /v2/transparency-log response: " +
                       llvm::toString(val.takeError()));
        }
        const auto* obj = val->getAsObject();
        if (!obj) {
            return err("/v2/transparency-log response is not a JSON object");
        }
        TransparencyLogEntry e;
        if (auto n = obj->getInteger("log-index")) e.logIndex = *n;
        if (auto s = obj->getString("log-timestamp")) {
            e.logTimestamp = s->str();
        }
        if (auto s = obj->getString("log-signature")) {
            e.logSignature = s->str();
        }
        if (auto s = obj->getString("key-id")) e.keyId = s->str();
        if (auto s = obj->getString("issuer")) e.issuer = s->str();
        if (e.logSignature.empty()) {
            return err("/v2/transparency-log response missing "
                       "'log-signature' — refusing install");
        }
        return e;
    }

    llvm::Expected<std::optional<std::string>>
    HttpRepository::publishedChecksum(const std::string& packageName,
                                      const std::string& version) const {
        // v1 servers have no resolve metadata to ask.
        auto caps = capabilities();
        if (!caps) {
            llvm::consumeError(caps.takeError());
            return std::optional<std::string>{};
        }
        if (!caps->supportsV2()) return std::optional<std::string>{};

        auto md = v2Resolve(packageName, version);
        if (!md) {
            llvm::consumeError(md.takeError());
            return std::optional<std::string>{};
        }
        std::string sha = md->sha256;
        if (sha.empty()) return std::optional<std::string>{};
        // Servers may send the digest bare or already prefixed; the
        // comparison side (ArtifactCache::sha256OfFile) always produces
        // the prefixed form.
        if (sha.rfind("sha256:", 0) != 0) sha = "sha256:" + sha;
        return std::optional<std::string>{sha};
    }

    // ─── publisher-trust §6.1 / §6.2 ─────────────────────────────

    llvm::Expected<std::optional<std::string>>
    HttpRepository::organizationKeys(const std::string& org) const {
        // An org name goes into a URL path; keep it to the character set a
        // dotted name actually uses rather than escaping it, so a name that
        // could change the path shape is refused instead of encoded.
        for (char c : org) {
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                   || (c >= '0' && c <= '9')
                   || c == '-' || c == '_' || c == '.';
            if (!ok) {
                return err("'" + org + "' is not a usable organization name");
            }
        }
        if (org.empty() || org.find("..") != std::string::npos) {
            return err("'" + org + "' is not a usable organization name");
        }

        auto caps = capabilities();
        if (!caps) return caps.takeError();
        // A v1-only server has no key-document surface at all. That is
        // absence, not failure: spec 5.4's legacy path is exactly this.
        if (!caps->supportsV2()) return std::optional<std::string>{};

        std::string url = joinUrl(baseUrl_, "v2/org-keys/" + org);
        std::string body;
        auto code = getToString(state_->curl, url, auth_, body);
        if (!code) return code.takeError();
        if (*code == 404) return std::optional<std::string>{};
        if (*code < 200 || *code >= 300) {
            return err("HTTP " + std::to_string(*code) +
                       " fetching the organization key document for '" + org +
                       "' from " + url);
        }
        return std::optional<std::string>{body};
    }

    llvm::Expected<std::optional<std::string>>
    HttpRepository::releaseMetadataJson(const std::string& packageName,
                                        const std::string& version) const {
        auto caps = capabilities();
        if (!caps) return caps.takeError();
        if (!caps->supportsV2()) return std::optional<std::string>{};

        std::string url = joinUrl(baseUrl_,
            "v2/resolve?name=" + packageName + "&version=" + version);
        std::string body;
        auto code = getToString(state_->curl, url, auth_, body);
        if (!code) return code.takeError();
        if (*code == 404) return std::optional<std::string>{};
        if (*code < 200 || *code >= 300) {
            return err("HTTP " + std::to_string(*code) + " from " + url);
        }
        return std::optional<std::string>{body};
    }

    std::string HttpRepository::origin() const {
        // scheme://host[:port]. Reducing to the origin is what makes the
        // binding usable: two clients configured with
        // "https://olla.cajeta.dev" and "https://olla.cajeta.dev/v2/" are
        // talking to the same server and must accept the same documents.
        auto scheme = baseUrl_.find("://");
        if (scheme == std::string::npos) return baseUrl_;
        auto slash = baseUrl_.find('/', scheme + 3);
        return slash == std::string::npos ? baseUrl_ : baseUrl_.substr(0, slash);
    }

    llvm::Expected<std::optional<std::string>>
    HttpRepository::revocations() const {
        auto caps = capabilities();
        if (!caps) return caps.takeError();
        if (!caps->supportsV2()) return std::optional<std::string>{};

        std::string url = joinUrl(baseUrl_, "v2/revocations");
        std::string body;
        auto code = getToString(state_->curl, url, auth_, body);
        if (!code) return code.takeError();
        // Absence reported faithfully. Whether it is fatal depends on the
        // `revocation` capability, and that decision lives in
        // revocationFor() rather than here — a driver that refused on its
        // own would refuse for repositories that never claimed to serve it.
        if (*code == 404) return std::optional<std::string>{};
        if (*code < 200 || *code >= 300) {
            return err("HTTP " + std::to_string(*code) + " from " + url);
        }
        return std::optional<std::string>{body};
    }

    llvm::Expected<std::optional<std::string>>
    HttpRepository::repositoryKeys() const {
        auto caps = capabilities();
        if (!caps) return caps.takeError();
        if (!caps->supportsV2()) return std::optional<std::string>{};

        std::string url = joinUrl(baseUrl_, "v2/repository-keys");
        std::string body;
        auto code = getToString(state_->curl, url, auth_, body);
        if (!code) return code.takeError();
        if (*code == 404) return std::optional<std::string>{};
        if (*code < 200 || *code >= 300) {
            return err("HTTP " + std::to_string(*code) +
                       " fetching the repository delegation from " + url);
        }
        return std::optional<std::string>{body};
    }

} // namespace cajeta::buildtool
