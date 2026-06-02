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

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
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
            req["context"] = std::move(context);

            std::string out;
            llvm::raw_string_ostream os(out);
            os << llvm::json::Value(std::move(req));
            os.flush();
            return out;
        }

        // Read a fd to EOF in chunks; return what was read so far on
        // EINTR / EOF.
        void drainFd(int fd, std::string& out) {
            char buf[4096];
            for (;;) {
                ssize_t n = ::read(fd, buf, sizeof(buf));
                if (n > 0) {
                    out.append(buf, static_cast<size_t>(n));
                    continue;
                }
                if (n == 0) break;
                if (errno == EINTR) continue;
                break;
            }
        }

        // Write `bytes` to fd, blocking until done. Returns false on
        // a real I/O error (the caller fails the dispatch).
        bool writeAll(int fd, const std::string& bytes) {
            size_t written = 0;
            while (written < bytes.size()) {
                ssize_t n = ::write(fd, bytes.data() + written,
                                    bytes.size() - written);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    return false;
                }
                if (n == 0) return false;
                written += static_cast<size_t>(n);
            }
            return true;
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
                // Findings carry through as a structured output the
                // task aggregator (Phase 7's lint task work) reads.
                // v1 stores them as JSON in outputs.findings as a
                // single string — TaskContext doesn't yet have a
                // typed findings channel.
                state.result.outputs["findings"] +=
                    trimmed + "\n";
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

        if (plugin.binaryPath.empty()) {
            return err("plugin '" + plugin.name +
                       "' has no binary declared in its sidecar " +
                       "(details.plugin.binary) — cannot dispatch '" +
                       actionName + "'");
        }

        std::string requestJson = serializeRequest(
            plugin, actionName, params, ctx);

        int inPipe[2];   // parent writes, child reads
        int outPipe[2];  // child writes, parent reads
        int errPipe[2];  // child writes stderr, parent reads
        if (::pipe(inPipe) < 0) {
            return err(std::string("plugin: pipe(stdin): ") +
                       std::strerror(errno));
        }
        if (::pipe(outPipe) < 0) {
            ::close(inPipe[0]); ::close(inPipe[1]);
            return err(std::string("plugin: pipe(stdout): ") +
                       std::strerror(errno));
        }
        if (::pipe(errPipe) < 0) {
            ::close(inPipe[0]); ::close(inPipe[1]);
            ::close(outPipe[0]); ::close(outPipe[1]);
            return err(std::string("plugin: pipe(stderr): ") +
                       std::strerror(errno));
        }

        pid_t pid = ::fork();
        if (pid < 0) {
            ::close(inPipe[0]); ::close(inPipe[1]);
            ::close(outPipe[0]); ::close(outPipe[1]);
            ::close(errPipe[0]); ::close(errPipe[1]);
            return err(std::string("plugin: fork: ") +
                       std::strerror(errno));
        }
        if (pid == 0) {
            ::dup2(inPipe[0],  STDIN_FILENO);
            ::dup2(outPipe[1], STDOUT_FILENO);
            ::dup2(errPipe[1], STDERR_FILENO);
            ::close(inPipe[0]);  ::close(inPipe[1]);
            ::close(outPipe[0]); ::close(outPipe[1]);
            ::close(errPipe[0]); ::close(errPipe[1]);
            // Single-arg exec: the plugin binary takes no positional
            // arguments. Everything it needs is on stdin.
            ::execl(plugin.binaryPath.c_str(),
                    plugin.binaryPath.c_str(),
                    static_cast<char*>(nullptr));
            std::string msg = "plugin: cannot execute '" +
                              plugin.binaryPath + "': " +
                              std::strerror(errno) + "\n";
            ::write(STDERR_FILENO, msg.data(), msg.size());
            _exit(127);
        }

        // Parent closes child-side fds + writes request, then reads
        // back stdout/stderr. The order is: write all of stdin, close
        // it, then drain stdout/stderr in sequence. Plugins that emit
        // huge amounts on stdout before draining stdin would deadlock
        // — but cajeta plugins' params are bounded by manifest size,
        // so the simple serial pattern works for v1.
        ::close(inPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[1]);

        if (!writeAll(inPipe[1], requestJson)) {
            ::close(inPipe[1]);
            ::close(outPipe[0]);
            ::close(errPipe[0]);
            // Reap the child even on write failure to avoid zombies.
            int status = 0;
            ::waitpid(pid, &status, 0);
            return err("plugin: failed to write request to '" +
                       plugin.binaryPath + "'");
        }
        ::close(inPipe[1]);

        std::string stdoutBuf;
        std::string stderrBuf;
        drainFd(outPipe[0], stdoutBuf);
        drainFd(errPipe[0], stderrBuf);
        ::close(outPipe[0]);
        ::close(errPipe[0]);

        int status = 0;
        for (;;) {
            pid_t w = ::waitpid(pid, &status, 0);
            if (w < 0) {
                if (errno == EINTR) continue;
                return err(std::string("plugin: waitpid: ") +
                           std::strerror(errno));
            }
            break;
        }

        // Forward the plugin's stderr verbatim — that's where its
        // crash traces / "binary not found" land.
        if (!stderrBuf.empty()) {
            ::write(STDERR_FILENO, stderrBuf.data(), stderrBuf.size());
        }

        if (WIFSIGNALED(status)) {
            return err("plugin '" + plugin.name + "' crashed (signal " +
                       std::to_string(WTERMSIG(status)) + ") while " +
                       "dispatching '" + actionName + "'");
        }
        int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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
