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
//         "capabilities":    [ ... allowlist intersection ... ],
//         "classpath":       "<consumer's resolved dep .cja paths, comma-
//                             joined; the same string BuildAction passes
//                             as --classpath. Absent when the project has
//                             no dependencies or resolution failed.>",
//         "toolchain":       { "cajeta": ..., "llc": ..., "llvm-dis": ...,
//                              "cc": ... },
//         "plugin":          { "artifact": "<this plugin's .cja>",
//                              "deps": [ ... its own closure ... ] }
//       }
//     }
//
// Direction: child → parent.
//
//   Stdout (one JSON object per line; trailing newline required):
//
//     {"kind": "log",   "level": "info|warn|debug", "message": "..."}
//        info/debug -> stdout (progress). warn -> stderr (a problem).
//        Failure is reported by the `result` record, never by a log.
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

        // Resolve an LLVM tool (`llc`, `llvm-dis`) to a path that exists
        // on THIS machine. The build-time LLVM dir is baked into the
        // binary, but it only exists where the binary was built — for a
        // released toolchain that is the CI runner. Order:
        //
        //   1. $CAJETA_LLVM_BIN/<tool>       (explicit user override)
        //   2. <baked LLVM tools dir>/<tool> (from-source builds)
        //   3. <dir of this binary>/<tool>   (bundled distributions)
        //   4. PATH
        //
        // Falls back to the bare name so the eventual spawn failure names
        // the missing tool instead of a phantom absolute path.
        std::string resolveLlvmTool(const char* tool) {
            namespace fs = std::filesystem;
            std::error_code ec;
            // generic_string() throughout: these paths are advertised to
            // plugins in JSON, where a native '\\' both needs escaping and
            // defeats substring checks; Windows spawns accept '/' fine.
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

            // 2. Reuse when the cached binary matches this artifact AND its
            // dependency closure — a dep-only update (same plugin version,
            // new library build) must invalidate the cache too, or stale
            // library code keeps running silently.
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
            // process; `llc`/`llvm-dis` are resolved against this machine
            // (the baked build-time dir is a candidate, not the answer —
            // see resolveLlvmTool); `cc` resolves from PATH.
            llvm::json::Object toolchain;
            toolchain["cajeta"] = runningExecutable();
            toolchain["llc"] = resolveLlvmTool("llc");
            toolchain["llvm-dis"] = resolveLlvmTool("llvm-dis");
            toolchain["cc"] = std::string("cc");
            context["toolchain"] = std::move(toolchain);

            // The CONSUMER's resolved dependency classpath.
            //
            // A plugin that compiles the consumer's sources — a coverage
            // instrumenter, a doc generator, a linter with a type view —
            // needs exactly what BuildAction passes as `--classpath`, and
            // until this existed the context carried everything EXCEPT
            // that: the toolchain paths, the plugin's own archive, its own
            // dependency closure. So `cajeta.coverage.instrument` compiled
            // a project that used a test framework and died on
            // `unknown type 'Runner'`, and the only workaround was to
            // hand-write `~/.olla/<name>/<version>/<name>-<version>.cja`
            // into the task and keep it in step with the manifest by hand.
            //
            // Same source as BuildAction's `--classpath`, same comma-joined
            // shape, so a plugin can forward it to the compiler verbatim.
            // Resolution failure is NOT fatal here: a plugin that does not
            // compile anything should not stop working because a dependency
            // is unreachable, so the key is omitted and the plugin reports
            // whatever it reports.
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

            // §3 — a record this build cannot read is DROPPED, never fatal.
            // Failing the action on one bad line would mean a plugin that
            // emits a malformed record stops working the day the build tool
            // learns to notice it, which is the opposite of what a protocol
            // spec is for. `dev.cajeta.coverage` 0.5.2 is in the wild
            // emitting exactly that.
            int dropped = 0;
            std::string firstDroppedReason;
            std::string firstDroppedLine;   // raw; quoted at report time
        };

        // Record a dropped line. Only the FIRST is kept: the warning names
        // one line and counts the rest, so a plugin emitting a thousand bad
        // records costs one warning rather than a flood that buries the
        // build's real output.
        void dropRecord(ProtocolState& state,
                        const std::string& reason,
                        const std::string& line) {
            if (state.dropped == 0) {
                state.firstDroppedReason = reason;
                state.firstDroppedLine = line;
            }
            ++state.dropped;
        }

        // One warning per plugin per invocation (spec §4 use case 5).
        //
        // The line is bytes the plugin controls and is malformed by
        // definition, so it goes through `quoteUntrustedLine` rather than
        // into the stream verbatim — a bad record must not be able to break
        // the diagnostic reporting it (spec §4.1).
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

        // Parse one line of the plugin's stdout stream into the protocol
        // state.
        //
        // NOTHING here fails the action (§3). A line either dispatches or is
        // dropped and counted; the caller warns once at the end. Before this,
        // a single unreadable line aborted the whole dispatch, which made the
        // build tool a ceiling on every plugin that had ever shipped a typo.
        // Findings carry error | warning | info; the diagnostic stream carries
        // error | warning | note. INFO -> note is the mapping `Severity`'s own
        // docstring already states for SARIF, not a new choice made here.
        const char* diagnosticSeverity(const std::string& findingSeverity) {
            if (findingSeverity == "error")   return "error";
            if (findingSeverity == "warning") return "warning";
            return "note";
        }

        // A finding, in the compiler's own text grammar (spec §6).
        //
        // The compiler writes
        //     cajeta: src/A.cajeta:12:5: CAJETA_ERROR_X: message
        //     cajeta: CAJETA_ERROR_X: message            (unlocated)
        // so the slots are <producer>: <location>: <tag>: <message>. A finding
        // fills the producer slot with the PLUGIN's name — which is its Olla
        // key and its `plugins` entry, so a reader goes straight from the line
        // to the manifest — and the tag slot with its severity, which is what
        // §6 use case 1 asks for and what makes a finding legible as a
        // problem rather than as narration.
        //
        // Naming the producer is what keeps a plugin finding from being read
        // as compiler output, and what tells two plugins' findings apart in
        // one task: every line says who said it.
        //
        // The rule goes in trailing brackets — the clang-tidy convention —
        // because the tag slot is spoken for. Omitted when the finding has no
        // rule, rather than printed as an empty pair.
        //
        // Location is emitted ONLY when there is one. A fabricated 0:0 would
        // make the IDE's filter navigate somewhere, which is worse than not
        // navigating at all.
        // A finding's message is plugin-controlled text on ONE console line.
        // A raw newline in it would split the rendering in two, leaving an
        // unattributed second line that reads as its own diagnostic — the
        // §4.1 problem one layer up, reached through a WELL-FORMED record
        // rather than a malformed one, so validation never sees it.
        //
        // Control characters become spaces rather than escapes: this is text a
        // person reads, and `caf\xc3\xa9` would be a worse rendering of a
        // legitimate message than `café`. `quoteUntrustedLine` is the right
        // tool for a line that is malformed by definition, not for a valid
        // message that merely contains a newline.
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

        void applyResponseLine(
            const std::string& line,
            ProtocolState& state,
            bool jsonMode,
            const std::string& pluginName,
            TaskContext& /*ctx*/) {
            // Allow blank lines — plugin emitters might add them for
            // readability when piping through a debugger.
            std::string trimmed = line;
            while (!trimmed.empty() &&
                   (trimmed.back() == '\r' || trimmed.back() == '\n' ||
                    trimmed.back() == ' '  || trimmed.back() == '\t')) {
                trimmed.pop_back();
            }
            if (trimmed.empty()) return;

            // A line that never even attempted a record is `printf`
            // debugging, not a protocol violation, and it keeps working
            // (spec §4 use case 4). The discriminator is the leading brace:
            // anything else was not trying to be JSON, so reporting it as
            // malformed would warn about the one case that is deliberate.
            // (It still fails CONFORMANCE — being lenient at runtime and
            // strict there is what lets the protocol tighten without
            // breaking anyone mid-build.)
            if (trimmed[0] != '{') {
                if (jsonMode) {
                    // Still narration, so still a note — but structured, and
                    // attributed, so a consumer can filter it out.
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

            // ONE definition of valid (plan §0.2.1). The runtime dispatches
            // on exactly the rules the conformance suite asserts, so
            // "conforms to spec" cannot mean two different things depending
            // on who is asking — and the ad-hoc `getString` guards that used
            // to live in each arm below are gone, because a record that
            // reaches the dispatch has its required fields.
            const auto check = checkPluginRecord(*obj);
            if (check.verdict != RecordVerdict::Valid) {
                // Malformed and unknown-kind are both dropped, but they are
                // not the same thing: an unknown kind is a NEWER plugin
                // talking to an older build tool, and refusing it would make
                // every build tool a ceiling on every plugin.
                dropRecord(state, check.reason, trimmed);
                return;
            }

            std::string k = obj->getString("kind")->str();
            if (k == "log") {
                // Progress, not a problem — stdout unless the record says
                // otherwise.
                //
                // These went to stderr on the reasoning that logs "pollute
                // structured outputs". They do not: structured results travel
                // as `output` and `result` records, and a plugin reports
                // failure through `result`, never through a log. What the old
                // routing actually produced was every line of an ordinary
                // cajeta-coco run —
                //
                //   [plugin] coco: [1/6] reference pass
                //   [plugin] coco: [3/6] instrumenting 6 of 10 modules
                //
                // — arriving on the error channel. IntelliJ's Build window
                // colours by stream and has no third state, so a successful
                // coverage run rendered as a wall of red and read as failure.
                //
                // `level` is honoured: an info/debug log is progress, a
                // warn-level log is a problem and keeps the error channel.
                auto msg = obj->getString("message");
                if (msg) {
                    auto level = obj->getString("level");
                    const bool isWarn = level && level->str() == "warn";
                    if (jsonMode) {
                        // A log is a note (spec §5). Its `level` is honoured
                        // rather than flattened: text mode already routes a
                        // warn-level log to the error channel, and JSON mode
                        // knowing less than text mode would make the
                        // structured stream the worse of the two.
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
                        // Its own kind, never a message: the plugin composed
                        // this for a human to read as-is, and wrapping it in a
                        // diagnostic would add a prefix it did not ask for.
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
                    // Structural in both directions: the value still reaches
                    // the invoking task through `${id.key}`, AND it reaches a
                    // stream consumer as data rather than as prose.
                    cajeta::emitJsonOutput(key, value);
                }
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
                if (!jsonMode) {
                    // Text mode: a finding is a problem, so it goes to the
                    // error channel like a compiler diagnostic — and it does
                    // NOT carry the `[plugin] ` progress prefix, which is what
                    // keeps the IDE's stream classifier from painting it as
                    // narration.
                    std::cerr << renderFinding(f, pluginName) << "\n";
                }
                if (jsonMode) {
                    // A located finding becomes a NAVIGABLE diagnostic; an
                    // unlocated one becomes a diagnostic with no location.
                    // `emitJsonDiagnostic` writes null for an empty file and a
                    // non-positive line/column, so absence stays absence
                    // rather than becoming a position of 0:0 that navigates
                    // somewhere wrong.
                    cajeta::emitJsonDiagnostic(diagnosticSeverity(f.severity),
                                               f.rule, f.message, f.file,
                                               f.line, f.column);
                }
                state.result.findings.push_back(std::move(f));
            } else if (k == "result") {
                const std::string status = obj->getString("status")->str();
                if (status == "ok" || status == "error") {
                    if (jsonMode) {
                        // Its own kind, attributed. The compiler's terminal
                        // `result` says whether the BUILD succeeded; this one
                        // says whether the plugin action did, and `source`
                        // is what tells them apart.
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
                    // A status this build cannot interpret is not a report.
                    // Dropped rather than fatal like everything else — and
                    // `resultSeen` stays false, so the action still fails as
                    // "produced no result", which is what actually happened.
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
        // 4.2.3 — provenance is stamped HERE, from the plugin the build tool
        // chose to invoke, and never read from the record. A plugin claiming
        // to be the compiler, or to be another plugin, has the claim
        // discarded: the field it emits is not a field this code reads.
        //
        // RAII because the ingest below has an early return on every dropped
        // record; a missed reset would attribute the rest of the build to
        // this plugin, which reads as a plugin emitting things it never did.
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
        return state.result;
    }

} // namespace cajeta::buildtool
