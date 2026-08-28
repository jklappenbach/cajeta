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
#include "cajeta/buildtool/DiagnosticFormat.h"
#include "cajeta/buildtool/Flavor.h"
#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/IrCache.h"
#include "cajeta/error/Diagnostics.h"
#include "cajeta/buildtool/Lockfile.h"   // sha256Hex
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Reproducibility.h"
#include "cajeta/buildtool/Resolver.h"
#include "cajeta/buildtool/SourceDigest.h"

#include <algorithm>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

#include <llvm/Support/Error.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "cajeta/buildtool/Subprocess.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>  // GetModuleFileNameA (running-exe path)
#else
#  include <unistd.h>   // readlink (/proc/self/exe running-exe path)
#endif

#ifndef CAJETA_VERSION
#define CAJETA_VERSION "0.0.0-unknown"
#endif

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // Find the cajeta binary path. Prefer the running executable
        // (/proc/self/exe on Linux, GetModuleFileName on Windows); fall
        // back to "cajeta" on PATH otherwise.
        std::string findCajetaBinary() {
            char buf[4096];
#if defined(_WIN32)
            DWORD n = ::GetModuleFileNameA(nullptr, buf, sizeof(buf));
            if (n > 0 && n < sizeof(buf)) {
                return std::string(buf, n);
            }
#else
            ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                return std::string(buf);
            }
#endif
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
            SettingsOutput outCfg;
            if (ctx.manifest()) {
                auto sb = parseSettingsBuild(*ctx.manifest());
                if (!sb) return sb.takeError();
                if (sb->sourceRoot) sourceRoot = *sb->sourceRoot;
                if (sb->outputDir)  outputDir  = *sb->outputDir;
                // settings.output (spec §3.3). Validated on LOAD, so a bad
                // value stops the build here rather than at first write.
                auto parsed = parseSettingsOutput(*ctx.manifest());
                if (!parsed) return parsed.takeError();
                outCfg = std::move(*parsed);
            }
            // `root` is the one knob most projects touch; the other three
            // override it individually. settings.build.output-dir stays
            // honoured as the legacy spelling of the same idea, so projects
            // that already set it keep working — output.root wins when both
            // are present, being the newer and more specific setting.
            if (outCfg.root) outputDir = *outCfg.root;
            fs::path interRoot = outCfg.intermediates
                ? fs::path(*outCfg.intermediates) : fs::path(outputDir) / "obj";
            fs::path artRoot = outCfg.artifacts
                ? fs::path(*outCfg.artifacts) : fs::path(outputDir) / "archive";
            // <root>/exe, not §3.1's build/bin: unit 2 kept the executable
            // where the toolchain skill and check-guide-part1.sh expect it.
            // The KEY is `binaries`, so adopting bin later is a default
            // change rather than a new setting.
            fs::path binRoot = outCfg.binaries
                ? fs::path(*outCfg.binaries) : fs::path(outputDir) / "exe";

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

            // build-output-layout §3.1 separates the two roles that used to
            // share one directory. `archiveRoot` is the ARTIFACT home — where
            // the deliverable lands and what the task reports. `compilerOut`
            // is the INTERMEDIATES home — the compiler's third positional,
            // where it writes per-class objects, bitcode and staging.
            //
            // They were the same path, so an exe build left its objects
            // beside the binary. Beyond the tidiness, that is what made
            // `details.name` equal to a top-level package name unlinkable:
            // the exe `build/exe/t` and the object tree `build/exe/t/` are
            // the same name, and the linker reported "cannot open output
            // file build/exe/t: Is a directory". Separating the roles removes
            // the shared parent, which is what cajeta-five's
            // buildtool-exe-package-name-collision spec predicted would fix
            // it outright.
            //
            // Exploded IR is the exception on purpose: there the emitted IR
            // tree IS the deliverable, so its artifact home and its output
            // directory are legitimately one path.
            fs::path compilerOut;
            if (emit == "exploded-ir") {
                compilerEmit = "ir";
                formatLabel = "exploded-ir";
                archiveRoot = fs::path(outputDir) / "ir";
                compilerOut = archiveRoot;
            } else if (emit == "archived-ir") {
                compilerEmit = "cja";
                formatLabel = "archived-ir";
                archiveRoot = artRoot;
                compilerOut = interRoot;
                outputPath = archiveRoot /
                             (detailsName + "-" + version + ".cja");
            } else {
                compilerEmit = "exe";
                formatLabel = "executable";
                archiveRoot = binRoot;
                compilerOut = interRoot;
                outputPath = archiveRoot / detailsName;
            }

            // Override output path if the user specified one.
            if (auto v = params.getString("output-path")) {
                outputPath = v->str();
            }

            // Ensure both homes exist — the artifact's and the
            // intermediates'. They are the same path only for exploded IR.
            std::error_code ec;
            fs::create_directories(archiveRoot, ec);
            if (ec) {
                return err("build: cannot create '" + archiveRoot.string() +
                           "': " + ec.message());
            }
            fs::create_directories(compilerOut, ec);
            if (ec) {
                return err("build: cannot create '" + compilerOut.string() +
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
            // Forward the diagnostic format (json-diagnostics-spec §2): output-only,
            // so it's kept out of the reproducibility flag set (identical artifact
            // either way) and just makes the child emit NDJSON on stderr, which
            // passes straight through to our stderr for the IDE plugin to consume.
            if (diagnosticFormat() == DiagFormat::Json) {
                argv.push_back("--diag-format=json");
            }
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
                std::string projectRoot =
                    projectRootFromManifest(*ctx.manifest());
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

            // Phase 11: append the reproducibility flag set. Order
            // is fixed (vocabulary-order, not host-locale-dependent)
            // so the argv we emit hashes the same across hosts.
            {
                std::string projectRootForRepro =
                    ctx.manifest() ? projectRootFromManifest(*ctx.manifest())
                                   : std::string(".");
                for (auto& f : reproducibilityFlags(
                                   ctx.properties(),
                                   projectRootForRepro)) {
                    argv.push_back(std::move(f));
                }
            }
            // Point skill embedding (skill-discovery D.3) at the PROJECT root
            // (where cajeta.json and the hand-authored skills/ dir live) — the
            // positional source root below is the deeper src/main/cajeta, which
            // has no skills/.
            if (ctx.manifest()) {
                argv.push_back("--skill-root=" +
                               projectRootFromManifest(*ctx.manifest()));
            }

            // Incremental compilation (incremental-compilation plan Phase 4;
            // DEFAULT-ON per Phase 5 — `incremental: false` opts out).
            // Authors a cache-manifest-v1 for the compiler: per-source
            // transitive digest → IrCache slot; clean when both slots exist.
            // The discriminator comes from the compiler itself
            // (--print-cache-discriminator probe on the exact flag set built
            // above) so flag resolution is never re-derived here.
            // Layered in FRONT (Phase 0): the whole-artifact cache — same
            // digests folded into one whole-build key; a hit re-publishes
            // the cached artifact without any compile at all.
            // `no-cache: true` bypasses both layers.
            bool noCache = false;
            if (auto v = params.getBoolean("no-cache")) noCache = *v;
            bool incremental = !noCache;
            bool incrementalExplicit = false;
            if (!noCache) {
                if (auto v = params.getBoolean("incremental")) {
                    incremental = *v;
                    incrementalExplicit = true;
                }
            }
            std::string projectRoot = ctx.manifest()
                ? projectRootFromManifest(*ctx.manifest()) : std::string(".");
            IrCache irCache((fs::path(projectRoot) / ".cajeta" / "cache"
                             / "ir").string());
            int cleanCount = 0, sourceCount = 0;
            std::string probeOut;
            if (incremental) {
                std::vector<std::string> probeArgv = argv;
                probeArgv.push_back("--print-cache-discriminator");
                SubprocessOptions probeOpt;
                probeOpt.argv = probeArgv;
                probeOpt.outData = &probeOut;
                SubprocessResult probeRes = runSubprocess(probeOpt);
                bool probeOk = probeRes.launched && probeRes.code() == 0;
                if (probeOk) {
                    while (!probeOut.empty() && (probeOut.back() == '\n'
                                                 || probeOut.back() == '\r'))
                        probeOut.pop_back();
                    probeOk = !probeOut.empty();
                }
                if (!probeOk) {
                    // Explicitly requested → fail loud. Default-on → a full
                    // (non-incremental) build is always sound; degrade so
                    // e.g. an older toolchain on PATH can't brick builds.
                    if (incrementalExplicit) {
                        return err("build: cache-discriminator probe failed"
                                   " (--print-cache-discriminator)");
                    }
                    llvm::errs() << "warning: build: cache-discriminator"
                                    " probe failed — building without the"
                                    " IR cache\n";
                    incremental = false;
                }
            }
            fs::path artifactSlot;   // set when the whole-artifact layer is live
            if (incremental) {
                SourceDigestRegistry digests({sourceRoot});
                std::vector<std::pair<std::string, std::string>> perSource;
                std::error_code ec2;
                for (auto& e : fs::recursive_directory_iterator(
                                   sourceRoot, ec2)) {
                    if (!e.is_regular_file()
                        || e.path().extension() != ".cajeta") continue;
                    auto digest = digests.digestOf(e.path().string());
                    if (!digest) return digest.takeError();
                    perSource.emplace_back(
                        fs::relative(e.path(), sourceRoot).generic_string(),
                        *digest);
                }
                sourceCount = static_cast<int>(perSource.size());
                std::sort(perSource.begin(), perSource.end());

                // Non-source inputs the archive EMBEDS. `skills/` is authored
                // beside cajeta.json, outside the positional source root, so
                // the .cajeta walk above never sees it — editing a skill left
                // the key unchanged and the cache re-published an artifact
                // carrying the OLD skill, byte-identical and silently stale.
                // Documentation that ships inside the artifact is a build
                // input like any other. (The generated skills/index.json is
                // derived from these files' front matter, so digesting the
                // sources covers it.)
                std::vector<std::pair<std::string, std::string>> perResource;
                if (ctx.manifest()) {
                    fs::path skillRoot =
                        fs::path(projectRootFromManifest(*ctx.manifest()))
                        / "skills";
                    std::error_code ec3;
                    if (fs::is_directory(skillRoot, ec3)) {
                        for (auto& e : fs::recursive_directory_iterator(
                                           skillRoot, ec3)) {
                            if (!e.is_regular_file()) continue;
                            std::string d = ArtifactCache::sha256OfFile(
                                e.path().string());
                            if (d.empty()) continue;
                            perResource.emplace_back(
                                "skills/" + fs::relative(e.path(), skillRoot)
                                                .generic_string(),
                                d);
                        }
                    }
                }
                std::sort(perResource.begin(), perResource.end());

                // Phase 0 whole-artifact layer — single-file artifacts only
                // (exe/cja); exploded-ir has no one artifact to re-publish.
                // Key = discriminator ⊕ entry ⊕ every (path, digest) over
                // sources AND embedded resources: the discriminator already
                // folds flags/emit/target/profile/classpath, but NOT the entry
                // method (positional) and not what gets embedded.
                if (!outputPath.empty()) {
                    std::string wholeInput = probeOut;
                    wholeInput.push_back('\0');
                    wholeInput += *entry;
                    wholeInput.push_back('\0');
                    for (auto& [rel, digest] : perSource) {
                        wholeInput += rel + "=" + digest + "\n";
                    }
                    // Separated from the source block so a source path can
                    // never collide with a resource path in the key.
                    wholeInput.push_back('\0');
                    for (auto& [rel, digest] : perResource) {
                        wholeInput += rel + "=" + digest + "\n";
                    }
                    std::string wholeDigest = sha256Hex(wholeInput);
                    const std::string pre = "sha256:";
                    if (wholeDigest.rfind(pre, 0) == 0)
                        wholeDigest = wholeDigest.substr(pre.size());
                    artifactSlot = fs::path(projectRoot) / ".cajeta"
                                 / "cache" / "artifact" / wholeDigest
                                 / outputPath.filename();
                    if (fs::exists(artifactSlot)) {
                        fs::create_directories(outputPath.parent_path(), ec2);
                        fs::copy_file(
                            artifactSlot, outputPath,
                            fs::copy_options::overwrite_existing, ec2);
                        if (!ec2) {
                            fs::permissions(outputPath,
                                fs::perms::owner_exec | fs::perms::group_exec
                                    | fs::perms::others_exec,
                                fs::perm_options::add, ec2);
                            // Direct line (not just an action output): task
                            // output echo depends on the task DECLARING
                            // outputs, and the skip must be visible always.
                            llvm::outs() << "[cache] hit — re-published "
                                         << outputPath.string() << "\n";
                            // A cached build spawns no compiler, so it emits no
                            // phase records — the IDE would show an instant green
                            // check under an empty tree. Say, structurally, that
                            // the output came from cache.
                            if (diagnosticFormat() == DiagFormat::Json) {
                                emitJsonCacheHit(outputPath.string());
                            }
                            ActionResult hit;
                            hit.outputs["format"] = formatLabel;
                            hit.outputs["flavor"] = flavor;
                            if (!profile.empty())
                                hit.outputs["profile"] = profile;
                            hit.outputs["cache"] = "hit";
                            hit.outputs["path"] = outputPath.string();
                            hit.outputs["sha256"] =
                                sha256OfFile(outputPath.string());
                            std::error_code szEc;
                            hit.outputs["size"] = std::to_string(
                                fs::file_size(outputPath, szEc));
                            return hit;
                        }
                        // Unreadable slot → fall through to a real build.
                    }
                }

                llvm::json::Array sources;
                for (auto& [rel, digest] : perSource) {
                    std::string bcSlot = fs::absolute(
                        irCache.keyFor(probeOut, digest)).string();
                    // Obligations + native object ride beside the .bc under
                    // the same key (the .o slot is Phase 6-alt: clean modules
                    // skip target lowering when it's populated).
                    std::string stem = bcSlot.substr(0, bcSlot.size() - 3);
                    std::string oblSlot = stem + ".obligations";
                    bool clean = fs::exists(bcSlot) && fs::exists(oblSlot);
                    if (clean) cleanCount++;
                    sources.push_back(llvm::json::Object{
                        {"path", rel},
                        {"clean", clean},
                        {"bc", bcSlot},
                        {"obligations", oblSlot},
                        {"obj", stem + ".o"}});
                }
                llvm::json::Object manifestJson{
                    {"version", "cache-manifest-v1"},
                    {"discriminator", probeOut},
                    {"sources", std::move(sources)}};
                fs::path manifestPath = fs::path(outputDir)
                                      / "cache-manifest.json";
                fs::create_directories(manifestPath.parent_path(), ec2);
                {
                    std::error_code fec;
                    llvm::raw_fd_ostream os(manifestPath.string(), fec);
                    if (fec) {
                        return err("build: cannot write cache manifest '"
                                   + manifestPath.string() + "': "
                                   + fec.message());
                    }
                    os << llvm::json::Value(std::move(manifestJson));
                }
                argv.push_back("--cache-manifest="
                               + fs::absolute(manifestPath).string());
            }

            // Positional args: <entry-method> <source-root> <archive-root>
            argv.push_back(entry->empty() ? std::string("*") : *entry);
            argv.push_back(sourceRoot);
            // The compiler's third positional is its OUTPUT DIRECTORY, i.e.
            // where intermediates go — not where the deliverable lands, which
            // `-o` above already set.
            argv.push_back(compilerOut.string());

            // Run the compiler. Its stdout/stderr pass through to the parent
            // terminal so the developer sees the output.
            SubprocessOptions so;
            so.argv = argv;
            SubprocessResult res = runSubprocess(so);
            if (!res.launched) {
                return err("build: cannot exec '" + cajetaBin + "': " +
                           res.error);
            }
            int exitCode = res.code();
            if (exitCode != 0) {
                return err("build: compiler exited " +
                           std::to_string(exitCode));
            }

            // Phase 0: record (whole digest → artifact) so the next
            // no-change build re-publishes without compiling.
            if (!artifactSlot.empty() && !outputPath.empty()) {
                std::error_code ec3;
                if (fs::exists(outputPath, ec3)) {
                    fs::create_directories(artifactSlot.parent_path(), ec3);
                    fs::path tmp = artifactSlot;
                    tmp += ".tmp";
                    fs::copy_file(outputPath, tmp,
                                  fs::copy_options::overwrite_existing, ec3);
                    if (!ec3) fs::rename(tmp, artifactSlot, ec3);
                }
            }

            // Post-build cache eviction per settings.build.cache. Runs only
            // after a successful incremental build (the compiler just wrote
            // fresh slots; oldest-first LRU trims to policy).
            if (incremental && ctx.manifest()) {
                auto sb = parseSettingsBuild(*ctx.manifest());
                if (!sb) return sb.takeError();
                if (sb->cacheMaxBytes > 0 || sb->cacheMaxAgeSeconds > 0) {
                    IrCache::EvictionPolicy policy;
                    policy.maxBytes = sb->cacheMaxBytes;
                    policy.maxAge =
                        std::chrono::seconds(sb->cacheMaxAgeSeconds);
                    auto evicted = irCache.evict(policy);
                    if (!evicted) return evicted.takeError();
                }
            }

            // Capture output.
            ActionResult r;
            r.outputs["format"] = formatLabel;
            r.outputs["flavor"] = flavor;
            if (incremental) {
                r.outputs["cache-clean"] = std::to_string(cleanCount) + "/"
                                         + std::to_string(sourceCount);
            }
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
