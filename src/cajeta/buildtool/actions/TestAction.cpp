// The `test` action — wraps a pre-built test binary, runs it, and
// parses pass/fail counts. Phase 7a scope: minimum viable test
// execution. Structured reporting (parse a JUnit / SARIF stream
// from the test binary's stdout) lands once the plugin runtime is
// in place — the action interface today is the shape every later
// slice extends.
//
// Spec (BuildTool.md "Action catalog" `test` row):
//   Required: —
//   Optional: input, filter, parallel, args, report, coverage
//   Outputs:  passed, failed, crashed, report-path, exit-code
//
// Today the action treats the binary's exit code as the verdict:
//   exit 0  → passed=1, failed=0, crashed=0
//   exit !0 → passed=0, failed=1, crashed=0   (clean fail)
//   killed  → passed=0, failed=0, crashed=1   (signal/abort)
//
// When the binary later supports the structured-findings protocol
// (Phase 7b), the per-test pass/fail counts come from the JSON
// stream instead of being derived from exit code.

#include "cajeta/buildtool/Action.h"

#include <llvm/Support/Error.h>

#include <cerrno>
#include <cstring>
#include <fstream>
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

        // Drain a pipe to EOF.
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

    class TestAction : public Action {
    public:
        std::string name() const override { return "test"; }

        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& ctx) const override {

            // `input` is the path to the test binary. We don't require
            // it at the manifest level — a future variant can derive
            // the path from `settings.build` defaults — but for v1 the
            // caller threads it via `${art.path}` from a prior build
            // action.
            auto inputRaw = params.getString("input");
            if (!inputRaw) {
                return err("test: missing required 'input' "
                           "(path to test binary)");
            }
            auto inputPath = ctx.substitute(inputRaw->str(), "test.input");
            if (!inputPath) return inputPath.takeError();

            // Optional params. `filter` and `parallel` are forwarded
            // to the binary via well-known flags (`--filter=`,
            // `--parallel=`); test frameworks that don't recognize
            // them ignore the args, so this is a no-op when the
            // binary isn't cajeta-style aware.
            std::vector<std::string> binaryArgs;
            binaryArgs.push_back(*inputPath);

            if (auto v = params.getString("filter")) {
                auto resolved = ctx.substitute(v->str(), "test.filter");
                if (!resolved) return resolved.takeError();
                binaryArgs.push_back("--filter=" + *resolved);
            }
            if (auto v = params.getInteger("parallel")) {
                binaryArgs.push_back(
                    "--parallel=" + std::to_string(*v));
            } else if (auto vs = params.getString("parallel")) {
                auto resolved = ctx.substitute(
                    vs->str(), "test.parallel");
                if (!resolved) return resolved.takeError();
                binaryArgs.push_back("--parallel=" + *resolved);
            }
            // `args` array — any extra arguments to forward verbatim.
            if (const auto* a = params.getArray("args")) {
                for (size_t i = 0; i < a->size(); ++i) {
                    auto s = (*a)[i].getAsString();
                    if (!s) {
                        return err("test: 'args[" + std::to_string(i) +
                                   "]' must be a string");
                    }
                    auto resolved = ctx.substitute(
                        s->str(),
                        "test.args[" + std::to_string(i) + "]");
                    if (!resolved) return resolved.takeError();
                    binaryArgs.push_back(*resolved);
                }
            }
            // `report` is the path the action writes its summary
            // report to. Optional — empty means no report written.
            std::string reportPath;
            if (auto v = params.getString("report")) {
                auto resolved = ctx.substitute(v->str(), "test.report");
                if (!resolved) return resolved.takeError();
                reportPath = *resolved;
            }
            // `coverage` is reserved for the cajeta.coverage plugin
            // (Phase 7d). Accept the param so callers can write it
            // today; the native test action ignores it.
            (void)params.getBoolean("coverage");

            std::vector<char*> argv;
            argv.reserve(binaryArgs.size() + 1);
            for (auto& a : binaryArgs) argv.push_back(a.data());
            argv.push_back(nullptr);

            int outPipe[2], errPipe[2];
            if (::pipe(outPipe) < 0) {
                return err(std::string("test: pipe(stdout): ") +
                           std::strerror(errno));
            }
            if (::pipe(errPipe) < 0) {
                ::close(outPipe[0]); ::close(outPipe[1]);
                return err(std::string("test: pipe(stderr): ") +
                           std::strerror(errno));
            }

            pid_t pid = ::fork();
            if (pid < 0) {
                ::close(outPipe[0]); ::close(outPipe[1]);
                ::close(errPipe[0]); ::close(errPipe[1]);
                return err(std::string("test: fork: ") +
                           std::strerror(errno));
            }
            if (pid == 0) {
                ::dup2(outPipe[1], STDOUT_FILENO);
                ::dup2(errPipe[1], STDERR_FILENO);
                ::close(outPipe[0]); ::close(outPipe[1]);
                ::close(errPipe[0]); ::close(errPipe[1]);
                ::execvp(argv[0], argv.data());
                std::string msg = "test: cannot execute '" +
                                  binaryArgs[0] + "': " +
                                  std::strerror(errno) + "\n";
                ::write(STDERR_FILENO, msg.data(), msg.size());
                _exit(127);
            }

            ::close(outPipe[1]);
            ::close(errPipe[1]);

            std::string stdoutBuf, stderrBuf;
            drainFd(outPipe[0], stdoutBuf);
            drainFd(errPipe[0], stderrBuf);
            ::close(outPipe[0]);
            ::close(errPipe[0]);

            // Forward to parent streams so the developer sees the
            // test output live-ish (drain-then-forward isn't truly
            // live, but it's close enough until we add line
            // streaming alongside structured findings).
            if (!stdoutBuf.empty()) {
                ::write(STDOUT_FILENO,
                        stdoutBuf.data(), stdoutBuf.size());
            }
            if (!stderrBuf.empty()) {
                ::write(STDERR_FILENO,
                        stderrBuf.data(), stderrBuf.size());
            }

            int status = 0;
            for (;;) {
                pid_t w = ::waitpid(pid, &status, 0);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    return err(std::string("test: waitpid: ") +
                               std::strerror(errno));
                }
                break;
            }

            int passed = 0, failed = 0, crashed = 0, exitCode = 0;
            if (WIFEXITED(status)) {
                exitCode = WEXITSTATUS(status);
                if (exitCode == 0) passed = 1;
                else                failed = 1;
            } else if (WIFSIGNALED(status)) {
                exitCode = 128 + WTERMSIG(status);
                crashed = 1;
            } else {
                exitCode = -1;
                crashed = 1;
            }

            // Write the report file when requested. Simple v1 format:
            // a single-line summary that downstream callers can grep.
            // Once the structured-findings stream is wired in (Phase
            // 7b), this becomes a richer document.
            if (!reportPath.empty()) {
                std::ofstream out(reportPath, std::ios::binary | std::ios::trunc);
                if (out) {
                    out << "binary: " << *inputPath << "\n"
                        << "passed: " << passed << "\n"
                        << "failed: " << failed << "\n"
                        << "crashed: " << crashed << "\n"
                        << "exit-code: " << exitCode << "\n";
                }
            }

            ActionResult r;
            r.stdoutLog = stdoutBuf;
            r.stderrLog = stderrBuf;
            r.outputs["passed"]    = std::to_string(passed);
            r.outputs["failed"]    = std::to_string(failed);
            r.outputs["crashed"]   = std::to_string(crashed);
            r.outputs["exit-code"] = std::to_string(exitCode);
            r.outputs["report-path"] = reportPath;

            if (failed || crashed) {
                std::string detail = (crashed
                    ? "test: '" + *inputPath +
                      "' crashed (signal exit "
                    : "test: '" + *inputPath + "' failed (exit ");
                detail += std::to_string(exitCode) + ")";
                if (!r.stderrLog.empty()) {
                    detail += ":\n" + r.stderrLog;
                }
                return err(detail);
            }
            return r;
        }
    };

    std::unique_ptr<Action> makeTestAction() {
        return std::make_unique<TestAction>();
    }

} // namespace cajeta::buildtool
