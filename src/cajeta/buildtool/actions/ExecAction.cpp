// The `exec` action — the build-tool's escape hatch. Spawns a subprocess, waits
// for it, and publishes its stdout/stderr as outputs beside the exit code.

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

            // Required: command. Optional: args, working-dir, env.
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

            // Sandboxing here is opt-in: auto-wrapping the user's escape hatch
            // would change behaviour for every shell-style invocation. Internal
            // actions consult the sandbox abstraction directly instead.
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

            // Forwarded to the parent's streams as well as kept as outputs, so
            // the developer sees the run AND `${id.stdout}` still threads.
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
