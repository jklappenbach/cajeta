// Plugin subprocess runtime — see PluginRuntime.h for the surface.
//
// ── Protocol spec (v1) ───────────────────────────────────────────
//
// Direction: parent (build tool) → child (plugin binary).
//
//   Stdin (single JSON object):
//     {
//       "version": 1,
//       "action":  "<namespaced action name>",
//       "entry":   "<symbol path from sidecar's entries map, or empty>",
//       "params":  { ... substituted action params ... },
//       "context": {
//         "workdir":         "<abs path to project root>",
//         "project-name":    "<consumer's details.name>",
//         "project-version": "<consumer's details.version>",
//         "capabilities":    [ ... allowlist intersection ... ]
//       }
//     }
//
// Direction: child → parent.
//
//   Stdout (one JSON object per line; trailing newline required):
//
//     {"kind": "log",   "level": "info|warn|debug", "message": "..."}
//     {"kind": "warn",                              "message": "..."}
//     {"kind": "write",                             "text":    "..."}
//     {"kind": "output", "key": "...",              "value":   "..."}
//     {"kind": "finding",
//       "rule":     "...",
//       "severity": "error|warning|info",
//       "file":     "...",
//       "line":     <int>,
//       "column":   <int>,
//       "message":  "..."}
//
//     {"kind": "result", "status": "ok"}                  // success
//     {"kind": "result", "status": "error", "message": "..."}  // logical failure
//
//   Stderr: free-form text. Forwarded verbatim to the build tool's
//   stderr so users see crash traces / "binary not found" / etc.
//
// Exit codes:
//   0   — plugin completed cleanly. Result kind decides logical
//         success/failure (a "result" record with status=error is
//         a logical fail; the plugin still exits 0).
//   non-zero — plugin crashed or refused to start. Surfaces to the
//             build tool as a hard error from invokePluginAction.
//
// ── Why this shape ──
//
// - Single request JSON keeps the parent → child handshake atomic.
//   No streaming of params; the plugin gets everything before doing
//   work. Aligns with how plugins are written — synchronous
//   functions in cajeta source, not coroutines.
// - JSON-line response lets the plugin emit progress as it goes:
//   coverage's per-file findings, lint's per-file warnings stream
//   to the parent live without waiting for whole-action completion.
// - `kind` discriminator makes parsing one-pass + extensible: future
//   record kinds (progress percentage, sub-action invocation) don't
//   break old parents.

#include "cajeta/buildtool/PluginRuntime.h"

#include "cajeta/buildtool/JsonC.h"
#include "cajeta/buildtool/OllaStore.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#if !defined(_WIN32)
#  include <unistd.h>
#endif
#include <cstring>
#include <iostream>
#include <cstdio>
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

        // ── compile-from-cja: the default plugin distribution model ──
        //
        // A plugin ships as a .cja like any other package; a native binary
        // is a DERIVED artifact. When the sidecar declares `main` (a static
        // no-arg protocol entry) and no explicit `binary`, the runtime:
        //
        //   1. auto-homes the artifact + sidecar into the local olla store
        //      (so the cache has a stable, name/version-addressed home),
        //   2. AOT-compiles it once — a synthesized one-class source shim
        //      calls `main`, because entry-point lookup reads user sources,
        //      not classpath archives — into <store>/<name>/<version>/bin/,
        //   3. stamps the binary with the artifact's sha and reuses it until
        //      the artifact changes.
        //
        // An explicit sidecar `binary` skips all of this (the escape hatch
        // for plugins that ship prebuilt executables).

        std::string runningExecutable() {
#if !defined(_WIN32)
            char buf[4096];
            ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = 0;
                return std::string(buf);
            }
#endif
            return "cajeta";
        }

        llvm::Expected<std::string> ensurePluginBinary(
            const ResolvedPlugin& plugin) {
            namespace fs = std::filesystem;

            OllaStore store(OllaStore::resolveRoot());

            // 1. Auto-home the artifact (idempotent).
            if (!store.read(plugin.name, plugin.version)) {
                fs::path tmpManifest =
                    fs::temp_directory_path() /
                    ("cajeta-plugin-sidecar-" + plugin.name + ".json");
                {
                    std::ofstream out(tmpManifest, std::ios::binary);
                    if (!out) {
                        return err("plugin " + plugin.name +
                                   ": cannot stage sidecar for the store");
                    }
                    out << plugin.manifestJson;
                }
                auto homed = store.write(plugin.name, plugin.version,
                                         plugin.artifactPath,
                                         tmpManifest.string());
                std::error_code ec;
                fs::remove(tmpManifest, ec);
                if (!homed) return homed.takeError();
            }

            fs::path verDir = fs::path(store.root()) / plugin.name /
                              plugin.version;
            fs::path binDir = verDir / "bin";
            fs::path bin = binDir / plugin.name;
            fs::path stamp = binDir / (plugin.name + ".sha256");

            // 2. Reuse when the cached binary matches this artifact.
            {
                std::error_code ec;
                if (fs::is_regular_file(bin, ec)) {
                    std::ifstream in(stamp);
                    std::string prior;
                    if (in) std::getline(in, prior);
                    if (!prior.empty() && prior == plugin.sha256) {
                        return bin.string();
                    }
                }
            }

            // 3. Compile. Split `main` into pkg.Class + method for the shim.
            auto lastDot = plugin.mainEntry.find_last_of('.');
            if (lastDot == std::string::npos || lastDot == 0 ||
                lastDot + 1 >= plugin.mainEntry.size()) {
                return err("plugin " + plugin.name +
                           ": details.plugin.main must be pkg.Class.method, "
                           "got '" + plugin.mainEntry + "'");
            }
            std::string cls = plugin.mainEntry.substr(0, lastDot);
            std::string method = plugin.mainEntry.substr(lastDot + 1);
            auto clsDot = cls.find_last_of('.');
            std::string clsShort = (clsDot == std::string::npos)
                                       ? cls
                                       : cls.substr(clsDot + 1);

            std::error_code ec;
            fs::path synthRoot = binDir / ".synth" / "src";
            fs::path synthPkg = synthRoot / "cajeta" / "plugin" / "synth";
            fs::path buildTmp = binDir / ".synth" / "out";
            fs::create_directories(synthPkg, ec);
            fs::create_directories(buildTmp, ec);
            {
                std::ofstream out(synthPkg / "Main.cajeta");
                if (!out) {
                    return err("plugin " + plugin.name +
                               ": cannot write the entry shim");
                }
                out << "package cajeta.plugin.synth;\n\n"
                    << "import " << cls << ";\n\n"
                    << "/** Synthesized entry shim: entry-point lookup reads\n"
                    << " *  user sources, so this one-liner bridges to the\n"
                    << " *  plugin archive's declared main. */\n"
                    << "public class Main {\n"
                    << "    public static void main() {\n"
                    << "        " << clsShort << "." << method << "();\n"
                    << "    }\n"
                    << "}\n";
            }

            std::string classpath = plugin.artifactPath;
            for (const auto& d : plugin.depArtifacts) {
                classpath += "," + d;
            }

            std::cout << "[plugin] compiling " << plugin.name << "@"
                      << plugin.version << " from its archive (one-time; "
                      << "cached in the local olla store)\n";

            SubprocessOptions opt;
            opt.argv = {runningExecutable(),
                        "--emit=exe",
                        "--classpath=" + classpath,
                        "-o", bin.string(),
                        "cajeta.plugin.synth.Main.main",
                        synthRoot.string(),
                        buildTmp.string()};
            std::string outData, errData;
            opt.outData = &outData;
            opt.errData = &errData;
            auto res = runSubprocess(opt);
            if (!res.launched || res.exitCode != 0) {
                std::string tail = errData.size() > 800
                                       ? errData.substr(errData.size() - 800)
                                       : errData;
                return err("plugin " + plugin.name +
                           ": compiling the archive failed: " + tail);
            }

            // 4. Stamp for reuse.
            {
                std::ofstream out(stamp);
                out << plugin.sha256 << "\n";
            }
            return bin.string();
        }

        // Build the request JSON the plugin reads from stdin.
        std::string serializeRequest(
            const ResolvedPlugin& plugin,
            const std::string& actionName,
            const llvm::json::Object& params,
            const TaskContext& ctx) {
            llvm::json::Object req;
            req["version"] = 1;
            req["action"]  = actionName;
            auto entryIt = plugin.entries.find(actionName);
            req["entry"] = (entryIt != plugin.entries.end())
                               ? entryIt->second
                               : std::string();
            // Copy params verbatim — they're already substituted by
            // the TaskRunner before we get here.
            req["params"] = llvm::json::Value(
                llvm::json::Object(params));

            llvm::json::Object context;
            const Manifest* m = ctx.manifest();
            std::string workdir;
            if (m) {
                // Best-effort: workdir is the consumer's manifest's
                // parent dir. We don't carry that on TaskContext yet,
                // so plugins requiring an absolute path fall back to
                // `.` and resolve themselves. v1 acceptable; harder
                // wiring once TaskRunner threads it through.
                workdir = ".";
                context["project-name"]    = m->details.name;
                context["project-version"] = m->details.version;
            } else {
                workdir = ".";
                context["project-name"]    = std::string();
                context["project-version"] = std::string();
            }
            context["workdir"] = workdir;
            llvm::json::Array caps;
            for (const auto& c : plugin.capabilities) {
                caps.push_back(c);
            }
            context["capabilities"] = std::move(caps);

            // Toolchain paths, so a plugin can orchestrate compilation
            // without machine-specific configuration: `cajeta` is this
            // process; `llc` is the toolchain LLVM this binary was built
            // against; `cc` resolves from PATH.
            llvm::json::Object toolchain;
            toolchain["cajeta"] = runningExecutable();
#ifdef CAJETA_LLVM_TOOLS_BIN
            toolchain["llc"] = std::string(CAJETA_LLVM_TOOLS_BIN) + "/llc";
            toolchain["llvm-dis"] =
                std::string(CAJETA_LLVM_TOOLS_BIN) + "/llvm-dis";
#else
            toolchain["llc"] = std::string("llc");
            toolchain["llvm-dis"] = std::string("llvm-dis");
#endif
            toolchain["cc"] = std::string("cc");
            context["toolchain"] = std::move(toolchain);

            // The plugin's own resolved artifacts — its archive and its
            // dependency closure — so it can extract bundled bitcode
            // (e.g. a probe runtime) from the packages it shipped in,
            // instead of knowing an install location.
            llvm::json::Object pluginObj;
            pluginObj["artifact"] = plugin.artifactPath;
            llvm::json::Array deps;
            for (const auto& d : plugin.depArtifacts) {
                deps.push_back(d);
            }
            pluginObj["deps"] = std::move(deps);
            context["plugin"] = std::move(pluginObj);
            req["context"] = std::move(context);

            std::string out;
            llvm::raw_string_ostream os(out);
            os << llvm::json::Value(std::move(req));
            os.flush();
            return out;
        }

        struct ProtocolState {
            bool resultSeen = false;
            bool resultOk = true;
            std::string resultMessage;
            ActionResult result;
        };

        // Parse one line of the plugin's stdout stream into the
        // protocol state. Lines that don't parse as JSON-objects with
        // a string `kind` are reported back as protocol errors.
        llvm::Error applyResponseLine(
            const std::string& line,
            ProtocolState& state,
            TaskContext& /*ctx*/) {
            // Allow blank lines — plugin emitters might add them for
            // readability when piping through a debugger.
            std::string trimmed = line;
            while (!trimmed.empty() &&
                   (trimmed.back() == '\r' || trimmed.back() == '\n' ||
                    trimmed.back() == ' '  || trimmed.back() == '\t')) {
                trimmed.pop_back();
            }
            if (trimmed.empty()) return llvm::Error::success();

            auto parsed = parseJsonC(trimmed);
            if (!parsed) {
                llvm::consumeError(parsed.takeError());
                return err("plugin response line is not valid JSON: " +
                           trimmed);
            }
            const auto* obj = parsed->getAsObject();
            if (!obj) {
                return err("plugin response line is not a JSON object: " +
                           trimmed);
            }
            auto kind = obj->getString("kind");
            if (!kind) {
                return err("plugin response line missing 'kind': " +
                           trimmed);
            }

            // Dispatch by kind. Unknown kinds are accepted but ignored
            // (forward-compat: future plugin versions can emit new
            // record kinds without breaking older build tools).
            std::string k = kind->str();
            if (k == "log") {
                // Verbose-mode log — write to stderr so it doesn't
                // pollute structured outputs.
                auto msg = obj->getString("message");
                if (msg) {
                    std::cerr << "[plugin] " << msg->str() << "\n";
                }
            } else if (k == "warn") {
                auto msg = obj->getString("message");
                if (msg) {
                    std::cerr << "warning: " << msg->str() << "\n";
                }
            } else if (k == "write") {
                auto text = obj->getString("text");
                if (text) {
                    std::cout << text->str();
                }
            } else if (k == "output") {
                auto key   = obj->getString("key");
                auto value = obj->getString("value");
                if (!key || !value) {
                    return err("plugin 'output' record missing key or value: " +
                               trimmed);
                }
                state.result.outputs[key->str()] = value->str();
            } else if (k == "finding") {
                // Structured findings — parsed into the typed
                // ActionResult.findings list. The lint task
                // aggregates these across actions; the test task
                // gates on coverage findings.
                ActionFinding f;
                if (auto s = obj->getString("rule")) f.rule = s->str();
                if (auto s = obj->getString("severity")) {
                    f.severity = s->str();
                }
                if (auto s = obj->getString("file")) f.file = s->str();
                if (auto n = obj->getInteger("line")) {
                    f.line = static_cast<int>(*n);
                }
                if (auto n = obj->getInteger("column")) {
                    f.column = static_cast<int>(*n);
                }
                if (auto s = obj->getString("message")) {
                    f.message = s->str();
                }
                if (f.severity.empty()) f.severity = "info";
                state.result.findings.push_back(std::move(f));
            } else if (k == "result") {
                state.resultSeen = true;
                auto status = obj->getString("status");
                if (!status) {
                    return err("plugin 'result' record missing 'status': " +
                               trimmed);
                }
                if (status->str() == "ok") {
                    state.resultOk = true;
                } else if (status->str() == "error") {
                    state.resultOk = false;
                    auto msg = obj->getString("message");
                    state.resultMessage = msg ? msg->str()
                                              : std::string("plugin reported error");
                } else {
                    return err("plugin 'result' record has unknown "
                               "status '" + status->str() +
                               "' (expected 'ok' or 'error'): " +
                               trimmed);
                }
            }
            // Unknown kind: ignore (forward-compat).
            return llvm::Error::success();
        }

    } // namespace

    llvm::Expected<ActionResult> invokePluginAction(
        const ResolvedPlugin& plugin,
        const std::string& actionName,
        const llvm::json::Object& params,
        TaskContext& ctx) {

        // Explicit `binary` wins; otherwise `main` selects the default
        // distribution model — compile the archive on first use and cache
        // the binary in the local olla store.
        std::string binaryPath = plugin.binaryPath;
        if (binaryPath.empty() && !plugin.mainEntry.empty()) {
            auto built = ensurePluginBinary(plugin);
            if (!built) return built.takeError();
            binaryPath = *built;
        }
        if (binaryPath.empty()) {
            return err("plugin '" + plugin.name +
                       "' declares neither details.plugin.binary nor "
                       "details.plugin.main — cannot dispatch '" +
                       actionName + "'");
        }

        std::string requestJson = serializeRequest(
            plugin, actionName, params, ctx);

        // Single-arg spawn: the plugin binary takes no positional arguments —
        // everything it needs is on stdin. Feed it the request, capture its
        // stdout/stderr. Plugins that emit huge amounts on stdout before
        // draining stdin could deadlock, but cajeta plugins' params are bounded
        // by manifest size, so the simple serial pattern works for v1.
        std::string stdoutBuf;
        std::string stderrBuf;
        SubprocessOptions so;
        so.argv = {binaryPath};
        so.stdinData = &requestJson;
        so.outData = &stdoutBuf;
        so.errData = &stderrBuf;
        SubprocessResult procRes = runSubprocess(so);
        if (!procRes.launched) {
            return err("plugin: cannot execute '" + binaryPath +
                       "': " + procRes.error);
        }

        // Forward the plugin's stderr verbatim — that's where its
        // crash traces / "binary not found" land.
        if (!stderrBuf.empty()) {
            std::fwrite(stderrBuf.data(), 1, stderrBuf.size(), stderr);
        }

        if (procRes.signaled) {
            return err("plugin '" + plugin.name + "' crashed (signal " +
                       std::to_string(procRes.signal) + ") while " +
                       "dispatching '" + actionName + "'");
        }
        int exitCode = procRes.exited ? procRes.exitCode : -1;
        if (exitCode != 0) {
            return err("plugin '" + plugin.name + "' exited " +
                       std::to_string(exitCode) +
                       " while dispatching '" + actionName + "'");
        }

        // Parse the JSON-line response stream.
        ProtocolState state;
        std::istringstream lines(stdoutBuf);
        std::string line;
        while (std::getline(lines, line)) {
            if (auto e = applyResponseLine(line, state, ctx)) {
                return std::move(e);
            }
        }

        if (!state.resultSeen) {
            return err("plugin '" + plugin.name +
                       "' produced no 'result' record for '" +
                       actionName + "' (protocol violation)");
        }
        if (!state.resultOk) {
            return err("cajeta.plugin: " + plugin.name + "." +
                       actionName + ": " + state.resultMessage);
        }
        return state.result;
    }

} // namespace cajeta::buildtool
