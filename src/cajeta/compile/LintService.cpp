// See LintService.h.

#include "cajeta/compile/LintService.h"

#include "cajeta/error/Diagnostics.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include <llvm/Support/JSON.h>

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/util/SelfPath.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CompilerMode.h"
#include "cajeta/compile/StdlibReuseCore.h"
#include "cajeta/error/DiagnosticEngine.h"
#include "cajeta/error/Exception.h"
#include "cajeta/type/CajetaType.h"

#ifdef _WIN32
#  include <io.h>
#  include <process.h>
#  define CAJETA_DUP _dup
#  define CAJETA_DUP2 _dup2
#  define CAJETA_CLOSE _close
#  define CAJETA_FILENO _fileno
#  define CAJETA_GETPID _getpid
#else
#  include <unistd.h>
#  define CAJETA_DUP dup
#  define CAJETA_DUP2 dup2
#  define CAJETA_CLOSE close
#  define CAJETA_FILENO fileno
#  define CAJETA_GETPID getpid
#endif

namespace cajeta::lintservice {

    int runLintDriver(Compiler& compiler, const std::string& file,
                      const std::string& sourceRoot, const std::string& shadow,
                      bool skipContextRegistration,
                      const std::function<void()>& afterContextRegistration) {
        const bool jsonDiag =
            compiler.getFlags().diagFormat == DiagFormat::Json;
        // The envelope emitters gate on a PROCESS-wide switch, but a lint's
        // format is a per-run decision, so the gate is scoped to this run.
        struct JsonGateScope {
            bool prev;
            explicit JsonGateScope(bool on) : prev(jsonProgressEnabled()) {
                setJsonProgressEnabled(on);
            }
            ~JsonGateScope() { setJsonProgressEnabled(prev); }
        } gate(jsonDiag);
        // Each lint response is its OWN stream, unlatched, so the warm
        // server's Nth request looks like a fresh process's first.
        emitStreamRecord();
        DiagnosticEngine engine;
        DiagnosticEngine::setActive(&engine);
        try {
            compiler.lint(file, sourceRoot, shadow, skipContextRegistration,
                          afterContextRegistration);
        } catch (SyntaxErrorException&) {
            DiagnosticEngine::setActive(nullptr);
            // A version line with no records is the plugin's signal to KEEP
            // its previous index for this file (ide-symbol-index §7).
            compiler.emitLintXrefStream(file, sourceRoot, shadow);
            emitJsonResult("error", "syntax errors");
            return 1;
        } catch (Exception& e) {
            engine.report("error", e.getErrorId(), e.getMessage(),
                          e.getFile(), e.getLine(), e.getColumn());
        } catch (const std::exception& e) {
            engine.report("error", "", e.what());
        } catch (const char* msg) {
            // Legacy raw throws still exist in type resolution; one escaping
            // here would terminate() the resident server mid-request.
            engine.report("error", "", msg ? msg : "unknown error");
        }
        DiagnosticEngine::setActive(nullptr);
        engine.emit(jsonDiag);
        compiler.emitLintXrefStream(file, sourceRoot, shadow);
        const int rc = engine.hasErrors() ? 1 : 0;
        emitJsonResult(rc == 0 ? "ok" : "error");
        return rc;
    }

    int warmLint(const LintRequest& req) {
        auto& core = StdlibReuseCore::instance();
        // A stale shared context makes the next Compiler skip resetGlobals.
        struct SharedContextGuard {
            ~SharedContextGuard() { Compiler::setSharedContext(nullptr); }
        } guard;

        core.ensurePrimed();
        core.restoreBaseline();
        Compiler::setSharedContext(core.context());

        Compiler compiler;
        compiler.getMutableFlags().diagFormat =
            req.jsonDiagnostics ? DiagFormat::Json : DiagFormat::Text;
        compiler.getMutableFlags().emitXref = req.emitXref ? "-" : "";
        for (const auto& cp : req.classpath) compiler.addClasspath(cp);

        int rc = runLintDriver(compiler, req.file, req.sourceRoot, req.shadow);

        // LLVM struct types are context-owned and outlive the request's
        // modules. Stdlib-resident names are kept; this request's are not.
        CajetaType::releaseThrownTransientStructNames();
        return rc;
    }

    // ---- sibling-context reuse (spec §4) -----------------------------------

    namespace {

        std::string canonical(const std::string& p) {
            std::error_code ec;
            auto c = std::filesystem::weakly_canonical(p, ec);
            return ec ? p : c.string();
        }

        /// The single under-root path the sweep omits: the --shadow original
        /// if set, else the target file itself.
        std::set<std::string> exclusionSet(const LintRequest& req) {
            std::set<std::string> s;
            s.insert(canonical(req.shadow.empty() ? req.file : req.shadow));
            return s;
        }

        /// Every .cajeta file under `root` except `excluded`, stamped by
        /// (mtime, size) — together the sweep's invalidation key.
        std::map<std::string, std::pair<std::int64_t, std::uintmax_t>>
        scanRootStamps(const std::string& root,
                       const std::set<std::string>& excluded) {
            namespace fs = std::filesystem;
            std::map<std::string, std::pair<std::int64_t, std::uintmax_t>> out;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(root, ec), end;
                     it != end; it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file()) continue;
                if (it->path().extension() != ".cajeta") continue;
                std::string canon = canonical(it->path().string());
                if (excluded.count(canon)) continue;
                std::error_code sec, mec;
                std::uintmax_t size = fs::file_size(it->path(), sec);
                auto mtime = fs::last_write_time(it->path(), mec);
                if (sec || mec) continue;
                out[canon] = { mtime.time_since_epoch().count(), size };
            }
            return out;
        }

    } // namespace

    LintOutcome warmLintServed(const LintRequest& req, SiblingContext& ctx) {
        LintOutcome out;
        struct SharedContextGuard {
            ~SharedContextGuard() { Compiler::setSharedContext(nullptr); }
        } guard;

        auto& core = StdlibReuseCore::instance();
        core.ensurePrimed();

        auto runTarget = [&](bool warm) {
            Compiler compiler;
            compiler.getMutableFlags().diagFormat =
                req.jsonDiagnostics ? DiagFormat::Json : DiagFormat::Text;
            compiler.getMutableFlags().emitXref = req.emitXref ? "-" : "";
            for (const auto& cp : req.classpath) compiler.addClasspath(cp);
            std::function<void()> afterContext;
            if (!warm) {
                // Snapshot right after the sweep, before the target parses.
                afterContext = [&]() { core.captureContextBaseline(); };
            }
            out.rc = runLintDriver(compiler, req.file, req.sourceRoot, req.shadow,
                                   /*skipContextRegistration=*/warm, afterContext);
            CajetaType::releaseThrownTransientStructNames();
        };

        if (req.sourceRoot.empty()) {
            core.restoreBaseline();
            Compiler::setSharedContext(core.context());
            runTarget(/*warm=*/false);
            out.siblingsReparsed = 0;
            core.invalidateContextBaseline();
            ctx.valid = false;
            return out;
        }

        auto excluded = exclusionSet(req);
        auto stamps = scanRootStamps(req.sourceRoot, excluded);

        const bool hot = core.contextBaselineValid() && ctx.valid
            && ctx.root == req.sourceRoot && ctx.excluded == excluded
            && ctx.classpath == req.classpath && ctx.stamps == stamps;

        if (hot) {
            core.restoreContextBaseline();
            Compiler::setSharedContext(core.context());
            runTarget(/*warm=*/true);
            out.siblingsReparsed = 0;
        } else {
            core.restoreBaseline();
            Compiler::setSharedContext(core.context());
            runTarget(/*warm=*/false);
            out.siblingsReparsed = static_cast<int>(stamps.size());
            if (core.contextBaselineValid()) {
                ctx.root = req.sourceRoot;
                ctx.excluded = std::move(excluded);
                ctx.stamps = std::move(stamps);
                ctx.classpath = req.classpath;
                ctx.valid = true;
            } else {
                ctx.valid = false;
            }
        }
        return out;
    }

    // ---- the server loop (spec §2) -----------------------------------------

    namespace {

        /// JSON-escapes `s`, surrounding quotes included.
        std::string jsonEscape(const std::string& s) {
            std::string out;
            llvm::raw_string_ostream os(out);
            os << llvm::json::Value(s);
            os.flush();
            return out;
        }

        /// Writes a `{"kind":"error",...}` record to stdout, flushed per
        /// line. `id` follows kind so a consumer can correlate the request.
        void emitErrorRecord(std::optional<int64_t> id, const char* code,
                             const std::string& message) {
            std::string o = "{\"kind\":\"error\"";
            if (id) o += ",\"id\":" + std::to_string(*id);
            o += ",\"code\":" + jsonEscape(code);
            o += ",\"message\":" + jsonEscape(message);
            o += "}\n";
            std::cout << o << std::flush;
        }

        std::string readCaptureFile(const std::filesystem::path& p) {
            std::ifstream in(p, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        /// Runs `fn` with fd 2 redirected to a temp file and returns what it
        /// wrote — the diagnostics and xref stream that ARE the response
        /// payload. Restores fd 2 on every path; empty if the file won't open.
        std::string captureStderr(const std::function<void()>& fn) {
            namespace fs = std::filesystem;
            static uint64_t counter = 0;
            fs::path capPath = fs::temp_directory_path()
                / ("cajeta_lintsrv_cap_" + std::to_string(CAJETA_GETPID())
                   + "_" + std::to_string(counter++));

            std::fflush(stderr);
            std::cerr.flush();
            int savedFd = CAJETA_DUP(CAJETA_FILENO(stderr));
            FILE* redirect = std::fopen(capPath.string().c_str(), "wb");
            if (!redirect) { CAJETA_CLOSE(savedFd); fn(); return ""; }
            CAJETA_DUP2(CAJETA_FILENO(redirect), CAJETA_FILENO(stderr));

            fn();

            std::fflush(stderr);
            std::cerr.flush();
            CAJETA_DUP2(savedFd, CAJETA_FILENO(stderr));
            CAJETA_CLOSE(savedFd);
            std::fclose(redirect);

            std::string payload = readCaptureFile(capPath);
            std::error_code ec;
            fs::remove(capPath, ec);
            return payload;
        }

    } // namespace

    int runLintServer(const ServerOptions& opts) {
        StdlibReuseCore::instance().ensurePrimed();
        Compiler::setSharedContext(nullptr);

        // The identity is the running file's CONTENT, hashed now because by
        // the time a request arrives the file may already have been replaced.
        // Hand-assembled: llvm::json::Object would not put `kind` first.
        std::string selfPath = cajeta::util::runningExecutablePath();
        std::string selfId =
            selfPath.empty() ? std::string()
                             : buildtool::ArtifactCache::sha256OfFile(selfPath);

        std::string readyRec =
            "{\"kind\":\"server\",\"proto\":{\"major\":1,\"minor\":1},"
            "\"state\":\"ready\"";
        if (!selfId.empty()) {
            std::error_code sizeEc;
            auto sz = std::filesystem::file_size(selfPath, sizeEc);
            readyRec += ",\"binary\":{\"path\":" + jsonEscape(selfPath)
                      + ",\"id\":" + jsonEscape(selfId);
            if (!sizeEc)
                readyRec += ",\"size\":" + std::to_string(sz);
            readyRec += "}";
        }
        readyRec += "}";
        std::cout << readyRec << "\n" << std::flush;

        SiblingContext ctx;
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;

            auto parsed = llvm::json::parse(line);
            if (!parsed) {
                llvm::consumeError(parsed.takeError());
                emitErrorRecord(std::nullopt, "malformed-request",
                                "request line is not valid JSON");
                continue;
            }
            const llvm::json::Object* obj = parsed->getAsObject();
            if (!obj) {
                emitErrorRecord(std::nullopt, "malformed-request",
                                "request must be a JSON object");
                continue;
            }

            std::optional<int64_t> id;
            if (auto v = obj->getInteger("id")) id = *v;

            auto kind = obj->getString("kind");
            if (!kind) {
                emitErrorRecord(id, "missing-kind",
                                "request has no \"kind\"");
                continue;
            }
            if (*kind == "shutdown") return 0;
            if (*kind != "lint") {
                emitErrorRecord(id, "unknown-kind",
                                "unknown request kind: " + kind->str());
                continue;
            }

            auto file = obj->getString("file");
            if (!file) {
                emitErrorRecord(id, "missing-file",
                                "lint request has no \"file\"");
                continue;
            }
            std::string filePath = file->str();
            if (!std::filesystem::exists(filePath)) {
                emitErrorRecord(id, "file-not-found", "no such file: " + filePath);
                continue;
            }

            LintRequest req;
            req.file = filePath;
            req.sourceRoot = opts.sourceRoot;
            req.classpath = opts.classpath;
            if (auto sh = obj->getString("shadow")) req.shadow = sh->str();
            req.jsonDiagnostics = opts.jsonDiagnostics;
            req.emitXref = obj->getBoolean("emitXref").value_or(false);
            req.classpath = opts.classpath;

            LintOutcome outcome;
            std::string payload = captureStderr(
                [&]() { outcome = warmLintServed(req, ctx); });
            std::cout << payload;
            std::cout << "{\"kind\":\"done\",\"id\":" << (id ? *id : 0)
                      << ",\"siblingsReparsed\":" << outcome.siblingsReparsed
                      << "}\n" << std::flush;
        }
        return 0;
    }

} // namespace cajeta::lintservice
