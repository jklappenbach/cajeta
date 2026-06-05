// `package` — universal format-conversion verb. Phase 9 ships:
//
//   tarball       file/dir → .tar.zst (default) or .tar.gz
//   zip           file/dir → .zip  (shells to /usr/bin/zip)
//   container     executable → OCI image-layout (writeOciImage)
//   uber-archive  .cja + transitive deps → single .cja
//
// Deferred to compiler integration (clean v1-cut errors):
//   obj-tree, uber-ir, static-lib, shared-lib
//
// Deferred per-platform installer formats (clean v1-cut errors):
//   deb, rpm, msi, app-bundle, pkg, dmg, appimage, flatpak, snap
//
// Cache key on (input-sha256, format, format-specific-params)
// keeps repeated packages of unchanged input cheap.

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/OciImage.h"
#include "cajeta/buildtool/Resolver.h"
#include "cajeta/buildtool/repo/TarZstd.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
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
            std::string s;
            s.reserve(outLen * 2);
            for (unsigned i = 0; i < outLen; ++i) {
                s += hexd[(digest[i] >> 4) & 0xF];
                s += hexd[digest[i] & 0xF];
            }
            return s;
        }

        std::string sha256OfBytes(const std::string& bytes) {
            unsigned char digest[SHA256_DIGEST_LENGTH];
            EVP_MD_CTX* ctx = EVP_MD_CTX_new();
            EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
            EVP_DigestUpdate(ctx, bytes.data(), bytes.size());
            unsigned int outLen = 0;
            EVP_DigestFinal_ex(ctx, digest, &outLen);
            EVP_MD_CTX_free(ctx);
            static const char* hexd = "0123456789abcdef";
            std::string s;
            s.reserve(outLen * 2);
            for (unsigned i = 0; i < outLen; ++i) {
                s += hexd[(digest[i] >> 4) & 0xF];
                s += hexd[digest[i] & 0xF];
            }
            return s;
        }

        // Collect every file under `root` (recursively) into TarEntry
        // records. Paths are relative to root. Root may also be a
        // single file — in that case the entry's name is the file's
        // basename.
        llvm::Expected<std::vector<TarEntry>> collectEntries(
            const std::filesystem::path& root) {
            namespace fs = std::filesystem;
            std::error_code ec;
            std::vector<TarEntry> out;
            if (!fs::exists(root, ec)) {
                return err("package: input '" + root.string() +
                           "' does not exist");
            }
            if (fs::is_regular_file(root, ec)) {
                std::ifstream in(root, std::ios::binary);
                std::ostringstream ss; ss << in.rdbuf();
                out.push_back({root.filename().string(), ss.str()});
                return out;
            }
            if (!fs::is_directory(root, ec)) {
                return err("package: input '" + root.string() +
                           "' is neither a regular file nor a directory");
            }
            for (auto& it : fs::recursive_directory_iterator(root, ec)) {
                if (ec) return err("recurse '" + root.string() +
                                   "': " + ec.message());
                if (!it.is_regular_file()) continue;
                auto rel = fs::relative(it.path(), root, ec);
                std::ifstream in(it.path(), std::ios::binary);
                std::ostringstream ss; ss << in.rdbuf();
                out.push_back({rel.string(), ss.str()});
            }
            return out;
        }

        // Run `argv[0] argv[1..]`. Returns the child's exit code; on
        // exec failure the error message names the missing tool so
        // CI failures point at the actionable cause ("install zip").
        llvm::Expected<int> runChild(const std::vector<std::string>& argv) {
            std::vector<char*> argp;
            for (auto& a : argv) argp.push_back(
                const_cast<char*>(a.c_str()));
            argp.push_back(nullptr);
            pid_t pid = ::fork();
            if (pid < 0) {
                return err(std::string("fork: ") + std::strerror(errno));
            }
            if (pid == 0) {
                ::execvp(argp[0], argp.data());
                std::string msg = "cannot exec '" + std::string(argp[0]) +
                                  "': " + std::strerror(errno) + "\n";
                ::write(STDERR_FILENO, msg.data(), msg.size());
                _exit(127);
            }
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0) {
                if (errno != EINTR) {
                    return err(std::string("waitpid: ") +
                               std::strerror(errno));
                }
            }
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            return -1;
        }

        // Compute a stable cache key for (input-sha256, format,
        // format-specific params). Used to short-circuit repeated
        // packages of unchanged input.
        std::string cacheKeyFor(const std::string& inputSha,
                                const std::string& format,
                                const llvm::json::Object& params) {
            std::string key = inputSha + "|" + format;
            // Stable ordering: collect the keys that are format-
            // specific (everything other than `input` + `format` +
            // `id`). Sort, then append.
            static const std::set<std::string> ignore = {
                "input", "format", "id", "spec",
            };
            std::vector<std::string> keys;
            for (const auto& kv : params) {
                if (ignore.count(kv.first.str())) continue;
                keys.push_back(kv.first.str());
            }
            std::sort(keys.begin(), keys.end());
            std::string acc = key;
            for (const auto& k : keys) {
                std::string s;
                llvm::raw_string_ostream os(s);
                os << *params.get(k);
                acc += "|" + k + "=" + s;
            }
            return sha256OfBytes(acc);
        }

    } // namespace

    class PackageAction : public Action {
    public:
        std::string name() const override { return "package"; }

        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& ctx) const override {

            auto inputV = params.getString("input");
            if (!inputV) return err("package: missing required 'input'");
            auto formatV = params.getString("format");
            if (!formatV) return err("package: missing required 'format'");
            std::string input = inputV->str();
            std::string format = formatV->str();

            // Action-validation: detect input/format mismatch early
            // rather than mid-pipeline. The five "needs compiler IR"
            // formats refuse non-IR inputs cleanly.
            namespace fs = std::filesystem;
            std::error_code ec;
            if (!fs::exists(input, ec)) {
                return err("package: input '" + input +
                           "' does not exist");
            }
            bool inputIsDir = fs::is_directory(input, ec);

            // Deferred formats: per-platform installer + IR-shaped
            // formats. v1 honors the action contract (it's a known
            // verb) but errors clearly so the user can swap to a
            // shipping format without guessing.
            static const std::set<std::string> deferredFormats = {
                // Compiler-integration v1 cuts:
                "obj-tree", "uber-ir", "static-lib", "shared-lib",
                // Per-platform installer formats (Phase 9 deferred slices):
                "deb", "rpm", "msi", "app-bundle", "pkg", "dmg",
                "appimage", "flatpak", "snap",
            };
            if (deferredFormats.count(format)) {
                return err("package: format '" + format +
                           "' is a Phase 9 deferred slice (not yet "
                           "implemented in v1; see plans/buildtool/build-tool-plan.md "
                           "'Phase 9 — package action — deferred slices')");
            }

            // Resolve output path: explicit `output-path` wins; else
            // synthesized from input + format.
            std::string outputPath;
            if (auto v = params.getString("output-path")) {
                outputPath = v->str();
            } else {
                fs::path stem = fs::path(input).filename().stem();
                fs::path dir = fs::path(input).parent_path();
                if (dir.empty()) dir = ".";
                if      (format == "tarball")      outputPath = (dir / (stem.string() + ".tar.zst")).string();
                else if (format == "zip")          outputPath = (dir / (stem.string() + ".zip")).string();
                else if (format == "container")    outputPath = (dir / (stem.string() + ".oci")).string();
                else if (format == "uber-archive") outputPath = (dir / (stem.string() + ".cja")).string();
                else                               outputPath = (dir / stem.string()).string();
            }

            // Tarball-specific switch.
            std::string compression = "zstd";
            if (auto v = params.getString("compression")) {
                std::string c = v->str();
                if (c == "zstd" || c == "gzip" || c == "gz") {
                    compression = (c == "gz") ? "gzip" : c;
                } else {
                    return err("package: tarball: 'compression' must be "
                               "'zstd' or 'gzip' (got '" + c + "')");
                }
            }

            // Cache key + lookup. When the cache hit lives at the
            // same outputPath, we return without recomputing.
            std::string inputSha = inputIsDir
                ? std::string{}   // dirs aren't single-file-hashable
                : sha256OfFile(input);
            std::string cacheKey = cacheKeyFor(inputSha, format, params);

            // Cache hit detection: outputPath exists AND a sidecar
            // `.cajeta-package-key` file records the same key.
            if (fs::exists(outputPath, ec)) {
                fs::path keyFile = fs::path(outputPath).string() + ".pkgkey";
                std::ifstream kf(keyFile);
                std::string priorKey;
                std::getline(kf, priorKey);
                if (priorKey == cacheKey) {
                    ActionResult r;
                    r.outputs["path"]   = outputPath;
                    r.outputs["format"] = format;
                    r.outputs["sha256"] = sha256OfFile(outputPath);
                    r.outputs["cache"]  = "hit";
                    return r;
                }
            }

            // ─── Format dispatch ──────────────────────────────

            if (format == "tarball") {
                auto entries = collectEntries(input);
                if (!entries) return entries.takeError();
                std::string bytes;
                if (compression == "zstd") {
                    auto z = writeTarZstd(*entries);
                    if (!z) return z.takeError();
                    bytes = std::move(*z);
                } else {
                    // gzip: shell to `tar -czf`. POSIX tar handles
                    // both regular files and directory recursion in
                    // one tool — no need to maintain a second tar
                    // writer for this path.
                    std::vector<std::string> argv{
                        "tar", "-czf", outputPath, "-C",
                        fs::path(input).parent_path().string().empty()
                            ? std::string(".")
                            : fs::path(input).parent_path().string(),
                        fs::path(input).filename().string(),
                    };
                    auto code = runChild(argv);
                    if (!code) return code.takeError();
                    if (*code != 0) {
                        return err("package: tarball(gz): tar exited " +
                                   std::to_string(*code));
                    }
                    // Done — outputPath already filled by tar; skip
                    // the manual write below.
                    ActionResult r;
                    r.outputs["path"]   = outputPath;
                    r.outputs["format"] = format;
                    r.outputs["sha256"] = sha256OfFile(outputPath);
                    r.outputs["cache"]  = "miss";
                    {
                        std::ofstream kf(outputPath + ".pkgkey");
                        kf << cacheKey;
                    }
                    return r;
                }
                std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
                if (!out) return err("package: cannot write '" + outputPath + "'");
                out.write(bytes.data(),
                          static_cast<std::streamsize>(bytes.size()));
                ActionResult r;
                r.outputs["path"]   = outputPath;
                r.outputs["format"] = format;
                r.outputs["sha256"] = sha256OfFile(outputPath);
                r.outputs["cache"]  = "miss";
                {
                    std::ofstream kf(outputPath + ".pkgkey");
                    kf << cacheKey;
                }
                return r;
            }

            if (format == "zip") {
                // Shell to /usr/bin/zip. -r recurses dirs; -j junks
                // paths only when explicitly asked (we honor structure
                // by default).
                fs::remove(outputPath, ec);   // zip refuses overwrite
                std::vector<std::string> argv;
                if (inputIsDir) {
                    argv = {"zip", "-r", outputPath, "."};
                    // We chdir into the input dir so paths are
                    // input-relative inside the zip.
                    fs::path here = fs::current_path();
                    fs::current_path(input, ec);
                    auto code = runChild(argv);
                    fs::current_path(here, ec);
                    if (!code) return code.takeError();
                    if (*code != 0) {
                        return err("package: zip exited " +
                                   std::to_string(*code) +
                                   " (is `zip` installed?)");
                    }
                } else {
                    argv = {"zip", "-j", outputPath, input};
                    auto code = runChild(argv);
                    if (!code) return code.takeError();
                    if (*code != 0) {
                        return err("package: zip exited " +
                                   std::to_string(*code));
                    }
                }
                ActionResult r;
                r.outputs["path"]   = outputPath;
                r.outputs["format"] = format;
                r.outputs["sha256"] = sha256OfFile(outputPath);
                r.outputs["cache"]  = "miss";
                {
                    std::ofstream kf(outputPath + ".pkgkey");
                    kf << cacheKey;
                }
                return r;
            }

            if (format == "container") {
                if (inputIsDir) {
                    return err("package(container): input must be a "
                               "single executable file, not a directory "
                               "('" + input + "')");
                }
                OciImageSpec spec;
                spec.executablePath = input;
                if (auto v = params.getString("entrypoint-name"))
                    spec.entrypointName = v->str();
                if (auto v = params.getString("base"))
                    spec.baseHint = v->str();
                if (auto v = params.getString("tag"))
                    spec.tag = v->str();
                if (const auto* arr = params.getArray("expose")) {
                    for (const auto& p : *arr) {
                        if (auto s = p.getAsString())
                            spec.expose.push_back(s->str());
                    }
                }
                if (const auto* env = params.getObject("env")) {
                    for (const auto& kv : *env) {
                        if (auto s = kv.second.getAsString())
                            spec.env[kv.first.str()] = s->str();
                    }
                }
                if (const auto* labels = params.getObject("labels")) {
                    for (const auto& kv : *labels) {
                        if (auto s = kv.second.getAsString())
                            spec.labels[kv.first.str()] = s->str();
                    }
                }
                auto res = writeOciImage(outputPath, spec);
                if (!res) return res.takeError();
                ActionResult r;
                r.outputs["path"]     = res->layoutDir;
                r.outputs["format"]   = format;
                r.outputs["manifest"] = res->manifestDigest;
                r.outputs["config"]   = res->configDigest;
                r.outputs["layer"]    = res->layerDigest;
                r.outputs["cache"]    = "miss";
                {
                    std::ofstream kf(
                        (fs::path(outputPath) /
                         ".cajeta-package-key").string());
                    kf << cacheKey;
                }
                return r;
            }

            if (format == "uber-archive") {
                if (inputIsDir) {
                    return err("package(uber-archive): input must be a "
                               "single .cja file (the entry archive), "
                               "not a directory");
                }
                // Bundle entry archive + every transitive dep .cja.
                std::vector<TarEntry> entries;
                {
                    std::ifstream in(input, std::ios::binary);
                    std::ostringstream ss; ss << in.rdbuf();
                    entries.push_back(
                        {fs::path(input).filename().string(), ss.str()});
                }
                if (ctx.manifest()) {
                    std::string projectRoot = ".";
                    if (!ctx.manifest()->sourcePath.empty()) {
                        auto parent = fs::path(ctx.manifest()->sourcePath)
                                          .parent_path();
                        if (!parent.empty()) projectRoot = parent.string();
                    }
                    auto resolved = resolveProjectDependencies(
                        *ctx.manifest(), projectRoot);
                    if (!resolved) return resolved.takeError();
                    // Manifest JSON inside the bundle: name/version/sha
                    // per entry so consumers know what they have.
                    llvm::json::Array bundleArr;
                    for (const auto& d : *resolved) {
                        std::ifstream in(d.artifactPath, std::ios::binary);
                        std::ostringstream ss; ss << in.rdbuf();
                        std::string bytes = ss.str();
                        std::string entryName =
                            fs::path(d.artifactPath).filename().string();
                        entries.push_back({entryName, bytes});
                        bundleArr.push_back(llvm::json::Object{
                            {"name",     d.name},
                            {"version",  d.version},
                            {"sha256",   "sha256:" + sha256OfBytes(bytes)},
                            {"artifact", entryName},
                        });
                    }
                    // Index file lists transitive entries.
                    std::string idx;
                    llvm::raw_string_ostream os(idx);
                    os << llvm::formatv("{0:2}",
                        llvm::json::Value(std::move(bundleArr)));
                    entries.push_back({"bundle.json", os.str()});
                }
                auto z = writeTarZstd(entries);
                if (!z) return z.takeError();
                std::ofstream out(outputPath,
                                  std::ios::binary | std::ios::trunc);
                if (!out) return err("package: cannot write '" + outputPath + "'");
                out.write(z->data(),
                          static_cast<std::streamsize>(z->size()));
                ActionResult r;
                r.outputs["path"]   = outputPath;
                r.outputs["format"] = format;
                r.outputs["sha256"] = sha256OfFile(outputPath);
                r.outputs["cache"]  = "miss";
                r.outputs["entries"] = std::to_string(entries.size());
                {
                    std::ofstream kf(outputPath + ".pkgkey");
                    kf << cacheKey;
                }
                return r;
            }

            return err("package: unknown format '" + format +
                       "' (v1: tarball, zip, container, uber-archive; "
                       "deferred: deb/rpm/msi/app-bundle/pkg/dmg/appimage/"
                       "flatpak/snap/obj-tree/uber-ir/static-lib/shared-lib)");
        }
    };

    std::unique_ptr<Action> makePackageAction() {
        return std::make_unique<PackageAction>();
    }

} // namespace cajeta::buildtool
