// Plugin wire protocol v1 — record shapes; see PluginRuntime.h for the surface.
// Parent → child, stdin, one JSON object:
//   {"version": 1,
//    "action":  "<namespaced action name>",
//    "entry":   "<symbol path from the sidecar's entries map, or empty>",
//    "params":  { ... substituted action params ... },
//    "context": {"workdir":         "<abs path to project root>",
//                "project-name":    "<consumer's details.name>",
//                "project-version": "<consumer's details.version>",
//                "capabilities":    [ ... allowlist intersection ... ],
//                "classpath":       "<consumer's resolved dep .cja paths,
//                                     comma-joined, the same string passed
//                                     as --classpath; absent when there are
//                                     none or resolution failed>",
//                "toolchain":       {"cajeta":.., "llc":.., "llvm-dis":..,
//                                    "cc":..},
//                "plugin":          {"artifact": "<this plugin's .cja>",
//                                    "deps": [ ... its own closure ... ]}}}
//
// Child → parent, stdout, one JSON object per line, trailing newline required:
//   {"kind": "log",     "level": "info|warn|debug", "message": "..."}
//   {"kind": "warn",    "message": "..."}
//   {"kind": "write",   "text": "..."}
//   {"kind": "output",  "key": "...", "value": "..."}
//   {"kind": "finding", "rule": "...", "severity": "error|warning|info",
//                       "file": "...", "line": <int>, "column": <int>,
//                       "message": "..."}
//   {"kind": "result",  "status": "ok"}
//   {"kind": "result",  "status": "error", "message": "..."}

#include "cajeta/buildtool/PluginRuntime.h"

#include "cajeta/buildtool/JsonC.h"
#include "cajeta/buildtool/OllaStore.h"
#include "cajeta/buildtool/DiagnosticFormat.h"
#include "cajeta/buildtool/PluginRecord.h"
#include "cajeta/error/Diagnostics.h"
#include "cajeta/buildtool/Resolver.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/Program.h>
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
#include <cstdlib>
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
        // A plugin ships as a .cja and the binary is DERIVED: auto-homed into the olla
        // store, AOT-compiled through a shim, reused until its sha. `binary` opts out.

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

        // Resolve an LLVM tool (`llc`, `llvm-dis`) to a path that exists on THIS
        // machine: $CAJETA_LLVM_BIN, the baked LLVM tools dir, this binary's own
        // dir, then PATH. Falls back to the bare name so a spawn names the tool.
        std::string resolveLlvmTool(const char* tool) {
            namespace fs = std::filesystem;
            std::error_code ec;
            // generic_string() throughout: these paths are advertised to plugins
            // in JSON, where a native '\\' needs escaping; spawns accept '/'.
            if (const char* env = ::getenv("CAJETA_LLVM_BIN")) {
                fs::path p = fs::path(env) / tool;
                if (fs::is_regular_file(p, ec)) return p.generic_string();
            }
#ifdef CAJETA_LLVM_TOOLS_BIN
            {
                fs::path p = fs::path(CAJETA_LLVM_TOOLS_BIN) / tool;
                if (fs::is_regular_file(p, ec)) return p.generic_string();
            }
#endif
            {
                fs::path self = runningExecutable();
                if (self.is_absolute()) {
                    fs::path p = self.parent_path() / tool;
                    if (fs::is_regular_file(p, ec)) return p.generic_string();
                }
            }
            if (auto onPath = llvm::sys::findProgramByName(tool)) {
                return fs::path(*onPath).generic_string();
            }
            return tool;
        }

        // The plugin's executable. Auto-homes the artifact, reuses the cached
        // binary while its stamp matches the artifact AND its dependency
        // closure, and otherwise compiles a shim that calls the `main` entry.
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

            // 2. Reuse only when the stamp covers the artifact AND its closure:
            // a dep-only update must invalidate the cache too.
            std::string stampPayload = plugin.sha256;
            for (const auto& d : plugin.depArtifacts) {
                stampPayload += "+" + ArtifactCache::sha256OfFile(d);
            }
            {
                std::error_code ec;
                if (fs::is_regular_file(bin, ec)) {
                    std::ifstream in(stamp);
                    std::string prior;
                    if (in) std::getline(in, prior);
                    if (!prior.empty() && prior == stampPayload) {
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

            // 4. Stamp for reuse (artifact + dependency closure).
            {
                std::ofstream out(stamp);
                out << stampPayload << "\n";
            }
            return bin.string();
        }

        // Build the request JSON the plugin reads from stdin: version, action,
        // entry, params, and the context block spelled out at the top of file.
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
            req["params"] = llvm::json::Value(
                llvm::json::Object(params));

            llvm::json::Object context;
            const Manifest* m = ctx.manifest();
            std::string workdir;
            if (m) {
                // Best-effort: TaskContext does not carry the manifest's parent
                // dir yet, so a plugin gets `.` and resolves it itself.
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

            // Toolchain paths, so a plugin can orchestrate compilation without
            // machine-specific configuration; `cajeta` is this process.
            llvm::json::Object toolchain;
            toolchain["cajeta"] = runningExecutable();
            toolchain["llc"] = resolveLlvmTool("llc");
            toolchain["llvm-dis"] = resolveLlvmTool("llvm-dis");
            toolchain["cc"] = std::string("cc");
            context["toolchain"] = std::move(toolchain);

            // The CONSUMER's resolved dependency classpath — the same
            // comma-joined string BuildAction passes as `--classpath`. Omitted
            // rather than fatal when resolution fails.
            if (m) {
                std::string projectRoot = projectRootFromManifest(*m);
                if (auto deps = resolveProjectDependencies(*m, projectRoot)) {
                    std::string joined;
                    for (const auto& d : *deps) {
                        if (!joined.empty()) joined += ",";
                        joined += d.artifactPath;
                    }
                    if (!joined.empty()) {
                        context["classpath"] = std::move(joined);
                    }
                } else {
                    llvm::consumeError(deps.takeError());
                }
            }

            // The plugin's OWN archive and dependency closure, so it can extract
            // bundled bitcode without knowing an install location.
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

            // A record this build cannot read is DROPPED, never fatal.
            int dropped = 0;
            std::string firstDroppedReason;
            std::string firstDroppedLine;   // raw; quoted at report time
        };

        // Record a dropped line. Only the FIRST is kept: one warning names it
        // and counts the rest, so a flood of bad records costs one warning.
        void dropRecord(ProtocolState& state,
                        const std::string& reason,
                        const std::string& line) {
            if (state.dropped == 0) {
                state.firstDroppedReason = reason;
                state.firstDroppedLine = line;
            }
            ++state.dropped;
        }

        // One warning per plugin per invocation. The line is plugin-controlled
        // and malformed by definition, so it goes through quoteUntrustedLine.
        void reportDropped(const ProtocolState& state,
                           const std::string& pluginName) {
            if (state.dropped == 0) return;
            std::cerr << "warning: plugin '" << pluginName << "' emitted "
                      << state.dropped << " record"
                      << (state.dropped == 1 ? "" : "s")
                      << " this build could not read; dropped, action"
                         " continued. First: "
                      << state.firstDroppedReason << ": \""
                      << quoteUntrustedLine(state.firstDroppedLine)
                      << "\"\n";
        }

        // Findings carry error | warning | info; the stream carries note for info.
        const char* diagnosticSeverity(const std::string& findingSeverity) {
            if (findingSeverity == "error")   return "error";
            if (findingSeverity == "warning") return "warning";
            return "note";
        }

        // Flatten plugin-controlled text onto ONE console line: newlines and
        // control characters become spaces, so a message cannot split a record.
        std::string oneLine(const std::string& text) {
            std::string out;
            out.reserve(text.size());
            for (unsigned char c : text) {
                out += (c == '\n' || c == '\r' || c == '\t' || c < 0x20)
                           ? ' '
                           : static_cast<char>(c);
            }
            return out;
        }

        // A finding in the compiler's own <producer>: <location>: <tag>:
        // <message> grammar, with the PLUGIN's name as producer and the rule in
        // trailing brackets. A location is emitted only when there is one.
        std::string renderFinding(const ActionFinding& f,
                                  const std::string& pluginName) {
            std::string out = pluginName;
            out += ": ";
            if (!f.file.empty() && f.line > 0) {
                out += f.file;
                out += ":" + std::to_string(f.line);
                out += ":" + std::to_string(f.column);
                out += ": ";
            }
            out += oneLine(f.severity.empty() ? "info" : f.severity);
            out += ": ";
            out += oneLine(f.message);
            if (!f.rule.empty()) { out += " [" + oneLine(f.rule) + "]"; }
            return out;
        }

        // Ingest one line of the plugin's stdout into `state`. NOTHING here
        // fails the action: a line either dispatches or is dropped and counted,
        // and the caller warns once at the end.
        void applyResponseLine(
            const std::string& line,
            ProtocolState& state,
            bool jsonMode,
            const std::string& pluginName,
            TaskContext& /*ctx*/) {
            std::string trimmed = line;
            while (!trimmed.empty() &&
                   (trimmed.back() == '\r' || trimmed.back() == '\n' ||
                    trimmed.back() == ' '  || trimmed.back() == '\t')) {
                trimmed.pop_back();
            }
            if (trimmed.empty()) return;

            // A line that never attempted a record is `printf` debugging, not a
            // protocol violation; the discriminator is the leading brace.
            if (trimmed[0] != '{') {
                if (jsonMode) {
                    cajeta::emitJsonDiagnostic("note", "", trimmed);
                } else {
                    std::cout << "[plugin] " << trimmed << "\n";
                }
                return;
            }

            auto parsed = parseJsonC(trimmed);
            if (!parsed) {
                llvm::consumeError(parsed.takeError());
                dropRecord(state, "not valid JSON", trimmed);
                return;
            }
            const auto* obj = parsed->getAsObject();
            if (!obj) {
                dropRecord(state, "not a JSON object", trimmed);
                return;
            }

            // ONE definition of valid: the same check the conformance suite
            // asserts, so a record reaching dispatch has its required fields.
            const auto check = checkPluginRecord(*obj);
            if (check.verdict != RecordVerdict::Valid) {
                // An unknown kind is a NEWER plugin, not a malformed record.
                dropRecord(state, check.reason, trimmed);
                return;
            }

            std::string k = obj->getString("kind")->str();
            if (k == "log") {
                // Progress, not a problem: info and debug go to stdout, and
                // only a warn-level log keeps the error channel.
                auto msg = obj->getString("message");
                if (msg) {
                    auto level = obj->getString("level");
                    const bool isWarn = level && level->str() == "warn";
                    if (jsonMode) {
                        cajeta::emitJsonDiagnostic(isWarn ? "warning" : "note",
                                                   "", msg->str());
                    } else {
                        std::ostream& os = isWarn ? std::cerr : std::cout;
                        os << "[plugin] " << msg->str() << "\n";
                    }
                }
            } else if (k == "warn") {
                auto msg = obj->getString("message");
                if (msg) {
                    if (jsonMode) {
                        cajeta::emitJsonDiagnostic("warning", "", msg->str());
                    } else {
                        std::cerr << "warning: " << msg->str() << "\n";
                    }
                }
            } else if (k == "write") {
                auto text = obj->getString("text");
                if (text) {
                    if (jsonMode) {
                        cajeta::emitJsonWrite(text->str());
                    } else {
                        std::cout << text->str();
                    }
                }
            } else if (k == "output") {
                const std::string key = obj->getString("key")->str();
                const std::string value = obj->getString("value")->str();
                state.result.outputs[key] = value;
                if (jsonMode) {
                    // Structural both ways: `${id.key}` and a stream consumer.
                    cajeta::emitJsonOutput(key, value);
                }
            } else if (k == "finding") {
                // Structured findings, typed into ActionResult.findings.
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
                if (!jsonMode) {
                    // A finding is a problem: the error channel, and NO
                    // `[plugin] ` prefix, which would read as narration.
                    std::cerr << renderFinding(f, pluginName) << "\n";
                }
                if (jsonMode) {
                    // emitJsonDiagnostic writes null for an empty file and a
                    // non-positive line/column, so absence stays absence.
                    cajeta::emitJsonDiagnostic(diagnosticSeverity(f.severity),
                                               f.rule, f.message, f.file,
                                               f.line, f.column);
                }
                state.result.findings.push_back(std::move(f));
            } else if (k == "result") {
                const std::string status = obj->getString("status")->str();
                if (status == "ok" || status == "error") {
                    if (jsonMode) {
                        // The plugin action's verdict, not the build's.
                        auto m = obj->getString("message");
                        cajeta::emitJsonResult(status, m ? m->str() : "");
                    }
                }
                if (status == "ok") {
                    state.resultSeen = true;
                    state.resultOk = true;
                } else if (status == "error") {
                    state.resultSeen = true;
                    state.resultOk = false;
                    auto msg = obj->getString("message");
                    state.resultMessage = msg ? msg->str()
                                              : std::string("plugin reported error");
                } else {
                    // Dropped like everything else — and `resultSeen` stays
                    // false, so the action still fails as "produced no result".
                    dropRecord(state,
                               "'result' record has unknown status '" +
                                   status + "' (expected 'ok' or 'error')",
                               trimmed);
                }
            }
        }

    } // namespace

    llvm::Expected<ActionResult> invokePluginAction(
        const ResolvedPlugin& plugin,
        const std::string& actionName,
        const llvm::json::Object& params,
        TaskContext& ctx) {

        // Explicit `binary` wins; otherwise `main` compiles and caches on first use.
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

        // The plugin takes no positional arguments: everything is on stdin.
        // Serial feed-then-drain, which cajeta's bounded params make safe.
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

        // Forward the plugin's stderr verbatim — crash traces land there.
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
        // Provenance is stamped HERE, from the plugin the build tool chose to
        // invoke, and never read from the record. RAII, because the ingest
        // returns early on dropped records and a missed reset mis-attributes.
        const bool jsonMode = diagnosticFormat() == DiagFormat::Json;
        {
            cajeta::JsonSourceScope provenance(plugin.name, plugin.version);
            while (std::getline(lines, line)) {
                applyResponseLine(line, state, jsonMode, plugin.name, ctx);
            }
        }
        reportDropped(state, plugin.name);

        if (!state.resultSeen) {
            return err("plugin '" + plugin.name +
                       "' produced no 'result' record for '" +
                       actionName + "' (protocol violation)");
        }
        if (!state.resultOk) {
            return err("cajeta.plugin: " + plugin.name + "." +
                       actionName + ": " + state.resultMessage);
        }

        // An `error` finding fails the task that produced it, checked AFTER the
        // plugin's own result. Every finding has already been reported by the
        // read loop, so failing here cannot truncate the report explaining it.
        int errorFindings = 0;
        const ActionFinding* firstError = nullptr;
        for (const auto& f : state.result.findings) {
            if (f.severity != "error") continue;
            ++errorFindings;
            if (firstError == nullptr) firstError = &f;
        }
        if (errorFindings > 0) {
            // Named by Olla key, so a failing build says WHICH plugin failed it.
            std::string why = "cajeta.plugin: " + plugin.name + "." +
                              actionName + ": " +
                              std::to_string(errorFindings) +
                              (errorFindings == 1 ? " error finding"
                                                  : " error findings");
            if (firstError != nullptr) {
                why += " (first: " + renderFinding(*firstError, plugin.name) + ")";
            }
            return err(why);
        }

        return state.result;
    }

} // namespace cajeta::buildtool
