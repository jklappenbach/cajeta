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

#include <llvm/Support/Error.h>

#include <cstdlib>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // Read all available bytes from `fd` into `out` until EOF.
        // Used for stdout/stderr capture.
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

            // Phase 11: sandbox wrap. The exec action requests the
            // (process, filesystem, env) capability set; network is
            // off by default and toggled via the action param
            // `network: true` (kept opt-in so a tool that calls
            // home is loud about it).
            {
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

            // Build argv / envp arrays as null-terminated C strings.
            std::vector<char*> argv;
            argv.reserve(argStrings.size() + 1);
            for (auto& a : argStrings) argv.push_back(a.data());
            argv.push_back(nullptr);

            std::vector<char*> envp;
            if (!envEntries.empty()) {
                envp.reserve(envEntries.size() + 1);
                for (auto& e : envEntries) envp.push_back(e.data());
                envp.push_back(nullptr);
            }

            // Pipes for stdout / stderr capture.
            int outPipe[2], errPipe[2];
            if (::pipe(outPipe) < 0) {
                return err(std::string("exec: pipe(stdout): ") + std::strerror(errno));
            }
            if (::pipe(errPipe) < 0) {
                ::close(outPipe[0]); ::close(outPipe[1]);
                return err(std::string("exec: pipe(stderr): ") + std::strerror(errno));
            }

            pid_t pid = ::fork();
            if (pid < 0) {
                ::close(outPipe[0]); ::close(outPipe[1]);
                ::close(errPipe[0]); ::close(errPipe[1]);
                return err(std::string("exec: fork: ") + std::strerror(errno));
            }

            if (pid == 0) {
                // Child. Wire pipe write-ends to stdout/stderr; close
                // read-ends; chdir if requested; exec.
                ::dup2(outPipe[1], STDOUT_FILENO);
                ::dup2(errPipe[1], STDERR_FILENO);
                ::close(outPipe[0]); ::close(outPipe[1]);
                ::close(errPipe[0]); ::close(errPipe[1]);

                if (!workingDir.empty()) {
                    if (::chdir(workingDir.c_str()) != 0) {
                        std::string msg = "exec: chdir('" + workingDir + "'): " +
                                          std::strerror(errno) + "\n";
                        ::write(STDERR_FILENO, msg.data(), msg.size());
                        _exit(127);
                    }
                }

                if (envp.empty()) {
                    ::execvp(argv[0], argv.data());
                } else {
                    ::execvpe(argv[0], argv.data(), envp.data());
                }
                // execvp returns only on failure.
                std::string msg = "exec: cannot execute '" +
                                  argStrings[0] + "': " +
                                  std::strerror(errno) + "\n";
                ::write(STDERR_FILENO, msg.data(), msg.size());
                _exit(127);
            }

            // Parent. Close write-ends; drain pipes; wait.
            ::close(outPipe[1]);
            ::close(errPipe[1]);

            std::string stdoutBuf;
            std::string stderrBuf;
            drainFd(outPipe[0], stdoutBuf);
            drainFd(errPipe[0], stderrBuf);
            ::close(outPipe[0]);
            ::close(errPipe[0]);

            // Forward captured output to the parent's streams so the
            // developer sees what the action did. We still keep the
            // captures in the action's outputs for ${id.stdout} /
            // ${id.stderr} threading.
            if (!stdoutBuf.empty()) {
                ::write(STDOUT_FILENO, stdoutBuf.data(), stdoutBuf.size());
            }
            if (!stderrBuf.empty()) {
                ::write(STDERR_FILENO, stderrBuf.data(), stderrBuf.size());
            }

            int status = 0;
            for (;;) {
                pid_t w = ::waitpid(pid, &status, 0);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    return err(std::string("exec: waitpid: ") + std::strerror(errno));
                }
                break;
            }

            int exitCode = 0;
            if (WIFEXITED(status)) {
                exitCode = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                exitCode = 128 + WTERMSIG(status);
            } else {
                exitCode = -1;
            }

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
