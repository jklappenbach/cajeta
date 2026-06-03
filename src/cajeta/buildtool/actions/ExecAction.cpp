// The `exec` action — the build-tool's escape hatch. Spawns a
// subprocess, waits for it to exit, captures stdout/stderr, and
// publishes them as outputs alongside the exit code. See
// BuildTool.md "Action catalog" `exec` row.
//
// Phase 3a implementation: fork + exec + waitpid on POSIX. Windows
// support lands later. No sandboxing yet (Phase 11). No retry
// (Phase 9).

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Sandbox.h"
#include "cajeta/buildtool/Subprocess.h"

#include <llvm/Support/Error.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

    } // namespace

    class ExecAction : public Action {
    public:
        std::string name() const override { return "exec"; }

        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& ctx) const override {

            // Required: command. Optional: args (array), working-dir,
            // env (object).
            auto cmdRaw = params.getString("command");
            if (!cmdRaw) {
                return err("exec: missing required 'command' field");
            }
            auto cmd = ctx.substitute(cmdRaw->str(), "exec.command");
            if (!cmd) return cmd.takeError();

            std::vector<std::string> argStrings;
            argStrings.push_back(*cmd);
            if (const auto* a = params.getArray("args")) {
                for (size_t i = 0; i < a->size(); ++i) {
                    auto s = (*a)[i].getAsString();
                    if (!s) {
                        return err("exec: 'args[" + std::to_string(i) +
                                   "]' must be a string");
                    }
                    auto resolved = ctx.substitute(
                        s->str(),
                        "exec.args[" + std::to_string(i) + "]");
                    if (!resolved) return resolved.takeError();
                    argStrings.push_back(*resolved);
                }
            }

            std::string workingDir;
            if (auto wd = params.getString("working-dir")) {
                auto resolved = ctx.substitute(wd->str(), "exec.working-dir");
                if (!resolved) return resolved.takeError();
                workingDir = *resolved;
            }

            std::vector<std::string> envEntries;
            if (const auto* envObj = params.getObject("env")) {
                for (const auto& kv : *envObj) {
                    auto s = kv.second.getAsString();
                    if (!s) {
                        return err("exec: env." + kv.first.str() +
                                   " must be a string");
                    }
                    auto resolved = ctx.substitute(
                        s->str(), "exec.env." + kv.first.str());
                    if (!resolved) return resolved.takeError();
                    envEntries.push_back(kv.first.str() + "=" + *resolved);
                }
            }

            // Phase 11: sandbox wrap, opt-in via `sandbox: true`.
            // The exec action is the user's escape hatch — auto-
            // wrapping it would change long-standing behaviour for
            // every shell-style invocation. Internal actions
            // (build / package / upload / publish) consult the
            // sandbox abstraction directly per their declared
            // capability set.
            bool wantSandbox = false;
            if (auto sb = params.getBoolean("sandbox"); sb && *sb) {
                wantSandbox = true;
            }
            if (wantSandbox) {
                SandboxPolicy pol;
                pol.capabilities = {Capability::Process,
                                    Capability::Filesystem,
                                    Capability::Env};
                if (auto net = params.getBoolean("network");
                    net && *net) {
                    pol.capabilities.insert(Capability::Network);
                }
                pol.projectRoot = workingDir.empty() ? "." : workingDir;
                const char* disable = std::getenv("CAJETA_NO_SANDBOX");
                pol.disabled = (disable && *disable);
                auto wrap = wrapInSandbox(pol, argStrings, envEntries);
                if (!wrap) return wrap.takeError();
                argStrings = std::move(wrap->argv);
                envEntries  = std::move(wrap->envEntries);
            }

            // Spawn the command, capturing stdout/stderr. cwd + env apply.
            std::string stdoutBuf;
            std::string stderrBuf;
            SubprocessOptions so;
            so.argv = argStrings;
            if (!workingDir.empty()) so.cwd = &workingDir;
            if (!envEntries.empty()) so.env = &envEntries;
            so.outData = &stdoutBuf;
            so.errData = &stderrBuf;
            SubprocessResult res = runSubprocess(so);
            if (!res.launched) {
                return err("exec: cannot execute '" + argStrings[0] + "': " +
                           res.error);
            }

            // Forward captured output to the parent's streams so the
            // developer sees what the action did. We still keep the
            // captures in the action's outputs for ${id.stdout} /
            // ${id.stderr} threading.
            if (!stdoutBuf.empty()) {
                std::fwrite(stdoutBuf.data(), 1, stdoutBuf.size(), stdout);
            }
            if (!stderrBuf.empty()) {
                std::fwrite(stderrBuf.data(), 1, stderrBuf.size(), stderr);
            }

            int exitCode = res.code();

            ActionResult r;
            r.stdoutLog = stdoutBuf;
            r.stderrLog = stderrBuf;
            r.outputs["stdout"] = std::move(stdoutBuf);
            r.outputs["stderr"] = std::move(stderrBuf);
            r.outputs["exit-code"] = std::to_string(exitCode);

            if (exitCode != 0) {
                return err("exec: '" + argStrings[0] + "' exited " +
                           std::to_string(exitCode) +
                           (r.stderrLog.empty() ? std::string()
                                                : ":\n" + r.stderrLog));
            }
            return r;
        }
    };

    std::unique_ptr<Action> makeExecAction() {
        return std::make_unique<ExecAction>();
    }

} // namespace cajeta::buildtool
