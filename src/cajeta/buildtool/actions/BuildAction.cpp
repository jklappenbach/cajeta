// `build` — wrap the cajeta compiler binary. Produces one of
// three first-class artifacts (exploded-ir, archived-ir, or
// executable) per the `emit` param. See BuildTool.md Action
// catalog "The build action in detail" subsection.
//
// Phase 5a implementation: fork+exec the cajeta binary, translate
// declarative action params into the compiler's CLI flag set,
// capture the output path. IR cache + custom-flavor map
// composition land in Phase 5b.
//
// Entry-method resolution precedence:
//   1. `entry-method` param on the action
//   2. `binary` param → settings.build.binaries[name].entry-method
//   3. settings.build.entry-method (manifest default)
//   4. None of the above + emit=executable → hard error
//
// Default emit:
//   - entry-method resolved → executable
//   - otherwise              → archived-ir

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Flavor.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Resolver.h"

#include <llvm/Support/Error.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#ifndef CAJETA_VERSION
#define CAJETA_VERSION "0.0.0-unknown"
#endif

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // Find the cajeta binary path. Prefer /proc/self/exe on
        // Linux (always the running binary); fall back to "cajeta"
        // on PATH otherwise.
        std::string findCajetaBinary() {
            char buf[4096];
            ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                return std::string(buf);
            }
            return "cajeta";
        }

        // Resolve the action's effective entry method per the four-
        // step precedence in the header comment. Returns empty
        // string when none resolvable (caller decides if that's an
        // error based on emit).
        llvm::Expected<std::string> resolveEntryMethod(
            const llvm::json::Object& params,
            const Manifest* manifest) {
            if (auto v = params.getString("entry-method")) {
                return v->str();
            }
            if (auto v = params.getString("binary")) {
                if (!manifest) {
                    return err("build: 'binary' param requires a manifest "
                               "(settings.build.binaries lookup); none "
                               "provided to the runner");
                }
                auto sb = parseSettingsBuild(*manifest);
                if (!sb) return sb.takeError();
                auto it = sb->binaries.find(v->str());
                if (it == sb->binaries.end()) {
                    std::string available;
                    for (const auto& kv : sb->binaries) {
                        if (!available.empty()) available += ", ";
                        available += kv.first;
                    }
                    return err("build: 'binary' '" + v->str() +
                               "' not found in settings.build.binaries "
                               "(available: " +
                               (available.empty() ? "<none>" : available) +
                               ")");
                }
                return it->second.entryMethod;
            }
            if (manifest) {
                auto sb = parseSettingsBuild(*manifest);
                if (!sb) return sb.takeError();
                if (sb->entryMethod) return *sb->entryMethod;
            }
            return std::string("");
        }

        // Compute SHA-256 of a file's contents. Returns empty on
        // error (the caller can decide whether to surface it).
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

    } // namespace

    class BuildAction : public Action {
    public:
        std::string name() const override { return "build"; }

        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& ctx) const override {

            // Resolve entry method.
            auto entry = resolveEntryMethod(params, ctx.manifest());
            if (!entry) return entry.takeError();

            // Resolve emit. Defaults: executable when entry-method
            // resolved; archived-ir otherwise. exploded-ir always
            // explicit.
            std::string emit;
            if (auto v = params.getString("emit")) emit = v->str();
            else emit = entry->empty() ? "archived-ir" : "executable";

            if (emit != "exploded-ir" && emit != "archived-ir" &&
                emit != "executable") {
                return err("build: 'emit' must be one of "
                           "exploded-ir / archived-ir / executable; "
                           "got '" + emit + "'");
            }
            if (emit == "executable" && entry->empty()) {
                return err("build: emit='executable' requires an entry "
                           "method; supply 'entry-method', 'binary' "
                           "(resolved against settings.build.binaries), "
                           "or settings.build.entry-method");
            }

            // Resolve flavor. Phase 5b: string form (built-in or
            // custom-flavor name) OR object form
            // ({base, ...overrides}). resolveFlavor walks the chain,
            // detects cycles, and yields (effective base, override
            // map). Overrides are honored by the discriminator (so a
            // custom-flavor change re-keys the cache); compiler-side
            // flag emission for them lands in Phase 8.
            std::string flavor;
            llvm::json::Object flavorOverrides;
            {
                llvm::json::Object customFlavors;
                if (ctx.manifest()) {
                    auto sb = parseSettingsBuild(*ctx.manifest());
                    if (!sb) return sb.takeError();
                    customFlavors = sb->customFlavorsRaw;
                }
                llvm::json::Value flavorRef("release");
                if (const auto* v = params.get("flavor")) {
                    flavorRef = *v;
                }
                auto resolved = resolveFlavor(flavorRef, customFlavors);
                if (!resolved) return resolved.takeError();
                flavor = resolved->base;
                flavorOverrides = std::move(resolved->overrides);
            }

            // Resolve target.
            std::string target = "host";
            if (auto v = params.getString("target")) target = v->str();

            // Resolve profile (compile-time @Profile gating).
            std::string profile;
            if (auto v = params.getString("profile")) profile = v->str();

            // Resolve source / archive roots from settings.build (or
            // defaults).
            namespace fs = std::filesystem;
            std::string sourceRoot = "src/main/cajeta";
            std::string outputDir  = "build";
            if (ctx.manifest()) {
                auto sb = parseSettingsBuild(*ctx.manifest());
                if (!sb) return sb.takeError();
                if (sb->sourceRoot) sourceRoot = *sb->sourceRoot;
                if (sb->outputDir)  outputDir  = *sb->outputDir;
            }

            // Decide archive-root + output-path per emit. The
            // compiler binary takes <entry> <source-root> <archive-root>
            // positionally; -o overrides the output filename.
            fs::path archiveRoot;
            fs::path outputPath;
            std::string compilerEmit;
            std::string formatLabel;
            std::string version = ctx.manifest()
                                  ? ctx.manifest()->details.version
                                  : "unknown";
            std::string detailsName = ctx.manifest()
                                      ? ctx.manifest()->details.name
                                      : "out";

            if (emit == "exploded-ir") {
                compilerEmit = "ir";
                formatLabel = "exploded-ir";
                archiveRoot = fs::path(outputDir) / "ir";
            } else if (emit == "archived-ir") {
                compilerEmit = "cja";
                formatLabel = "archived-ir";
                archiveRoot = fs::path(outputDir) / "archive";
                outputPath = archiveRoot /
                             (detailsName + "-" + version + ".cja");
            } else {
                compilerEmit = "exe";
                formatLabel = "executable";
                archiveRoot = fs::path(outputDir) / "exe";
                outputPath = archiveRoot / detailsName;
            }

            // Override output path if the user specified one.
            if (auto v = params.getString("output-path")) {
                outputPath = v->str();
            }

            // Ensure the output directory exists.
            std::error_code ec;
            fs::create_directories(archiveRoot, ec);
            if (ec) {
                return err("build: cannot create '" + archiveRoot.string() +
                           "': " + ec.message());
            }

            // Build the compiler argv. We invoke the same binary
            // (/proc/self/exe) — the dispatcher's
            // looksLikeTaskInvocation correctly falls through because
            // argv[1] is `--mode=...` (starts with `-`) which means
            // the compiler frontend takes over.
            std::string cajetaBin = findCajetaBinary();

            std::vector<std::string> argv;
            argv.push_back(cajetaBin);
            argv.push_back("--mode=" + flavor);
            argv.push_back("--emit=" + compilerEmit);
            if (target != "host") {
                argv.push_back("--target=" + target);
            }
            if (!profile.empty()) {
                argv.push_back("--profile=" + profile);
            }

            // Phase 8: emit the effective property bundle as
            // `--<key>=<value>` flags. The built-in defaults come from
            // builtinFlavorProperties; overrides (custom-flavor chain
            // + inline map) win.
            {
                ResolvedFlavor rf;
                rf.base = flavor;
                rf.overrides = flavorOverrides;
                auto effective = effectiveProperties(rf);
                if (!effective) return effective.takeError();
                for (auto& f : toCompilerFlags(*effective)) {
                    argv.push_back(std::move(f));
                }
            }

            // Resolve transitive dependencies (Phase 6b). The manifest's
            // own directory is the project root — that's where the
            // local artifact cache lives. Skip cleanly when no deps
            // declared; surface any resolver error as a build failure.
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
                if (!resolved->empty()) {
                    std::string joined;
                    for (const auto& r : *resolved) {
                        if (!joined.empty()) joined += ",";
                        joined += r.artifactPath;
                    }
                    argv.push_back("--classpath=" + joined);
                }
            }

            if (!outputPath.empty()) {
                argv.push_back("-o");
                argv.push_back(outputPath.string());
            }
            // Positional args: <entry-method> <source-root> <archive-root>
            argv.push_back(entry->empty() ? std::string("*") : *entry);
            argv.push_back(sourceRoot);
            argv.push_back(archiveRoot.string());

            std::vector<char*> argp;
            for (auto& a : argv) argp.push_back(a.data());
            argp.push_back(nullptr);

            // Fork + exec. Compiler stdout/stderr pass through to
            // the parent terminal so the developer sees the output.
            pid_t pid = ::fork();
            if (pid < 0) {
                return err(std::string("build: fork failed: ") +
                           std::strerror(errno));
            }
            if (pid == 0) {
                ::execvp(cajetaBin.c_str(), argp.data());
                std::string msg = "build: cannot exec '" + cajetaBin +
                                  "': " + std::strerror(errno) + "\n";
                ::write(STDERR_FILENO, msg.data(), msg.size());
                _exit(127);
            }

            int status = 0;
            while (::waitpid(pid, &status, 0) < 0) {
                if (errno != EINTR) {
                    return err(std::string("build: waitpid: ") +
                               std::strerror(errno));
                }
            }
            int exitCode = WIFEXITED(status) ? WEXITSTATUS(status)
                         : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                         : -1;
            if (exitCode != 0) {
                return err("build: compiler exited " +
                           std::to_string(exitCode));
            }

            // Capture output.
            ActionResult r;
            r.outputs["format"] = formatLabel;
            r.outputs["flavor"] = flavor;
            if (!profile.empty()) r.outputs["profile"] = profile;
            if (!outputPath.empty()) {
                r.outputs["path"] = outputPath.string();
                std::error_code ec2;
                if (fs::exists(outputPath, ec2)) {
                    r.outputs["sha256"] = sha256OfFile(outputPath.string());
                    r.outputs["size"]   = std::to_string(
                        fs::file_size(outputPath, ec2));
                }
            } else {
                // exploded-ir — path points at the directory.
                r.outputs["path"] = archiveRoot.string();
            }
            return r;
        }
    };

    std::unique_ptr<Action> makeBuildAction() {
        return std::make_unique<BuildAction>();
    }

} // namespace cajeta::buildtool
