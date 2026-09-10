// The `test` action: runs a pre-built test binary, derives the
// passed/failed/crashed outputs from its exit status, and applies the
// optional coverage gate.

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/CoverageReport.h"

#include <llvm/Support/Error.h>

#include <cstdio>
#include <fstream>
#include <map>
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

    } // namespace

    class TestAction : public Action {
    public:
        std::string name() const override { return "test"; }

        // Runs `params.input` (the test binary) with the optional filter /
        // parallel / args flags, writes `params.report`, then applies the
        // coverage gate. A failed, crashed or under-threshold run is an Error.
        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& ctx) const override {

            auto inputRaw = params.getString("input");
            if (!inputRaw) {
                return err("test: missing required 'input' "
                           "(path to test binary)");
            }
            auto inputPath = ctx.substitute(inputRaw->str(), "test.input");
            if (!inputPath) return inputPath.takeError();

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
            std::string reportPath;
            if (auto v = params.getString("report")) {
                auto resolved = ctx.substitute(v->str(), "test.report");
                if (!resolved) return resolved.takeError();
                reportPath = *resolved;
            }
            // `coverage` is either a boolean (instrumentation only, no gate)
            // or the typed object {map, min, min-per-file, exclude, report}.
            const llvm::json::Object* coverageObj = nullptr;
            if (auto b = params.getBoolean("coverage")) {
                (void)b;
            } else {
                coverageObj = params.getObject("coverage");
            }

            std::string stdoutBuf, stderrBuf;
            SubprocessOptions so;
            so.argv = binaryArgs;
            so.outData = &stdoutBuf;
            so.errData = &stderrBuf;
            SubprocessResult res = runSubprocess(so);
            if (!res.launched) {
                return err("test: cannot execute '" + binaryArgs[0] + "': " +
                           res.error);
            }

            if (!stdoutBuf.empty()) {
                std::fwrite(stdoutBuf.data(), 1, stdoutBuf.size(), stdout);
            }
            if (!stderrBuf.empty()) {
                std::fwrite(stderrBuf.data(), 1, stderrBuf.size(), stderr);
            }

            int passed = 0, failed = 0, crashed = 0, exitCode = 0;
            if (res.exited) {
                exitCode = res.exitCode;
                if (exitCode == 0) passed = 1;
                else                failed = 1;
            } else if (res.signaled) {
                exitCode = 128 + res.signal;
                crashed = 1;
            } else {
                exitCode = -1;
                crashed = 1;
            }

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

            if (coverageObj) {
                std::string mapPath;
                if (auto v = coverageObj->getString("map")) {
                    auto resolved =
                        ctx.substitute(v->str(), "test.coverage.map");
                    if (!resolved) return resolved.takeError();
                    mapPath = *resolved;
                }
                if (!mapPath.empty()) {
                    std::ifstream in(mapPath, std::ios::binary);
                    if (!in) {
                        return err("test.coverage.map: cannot read '" +
                                   mapPath + "'");
                    }
                    std::stringstream ss; ss << in.rdbuf();
                    auto parsed = parseCoverageMap(ss.str());
                    if (!parsed) return parsed.takeError();

                    std::vector<std::string> excludes;
                    if (const auto* a =
                            coverageObj->getArray("exclude")) {
                        for (size_t i = 0; i < a->size(); ++i) {
                            auto s = (*a)[i].getAsString();
                            if (!s) {
                                return err("test.coverage.exclude["
                                           + std::to_string(i) +
                                           "]: must be a string");
                            }
                            auto resolved = ctx.substitute(
                                s->str(),
                                "test.coverage.exclude[" +
                                std::to_string(i) + "]");
                            if (!resolved) return resolved.takeError();
                            excludes.push_back(*resolved);
                        }
                    }
                    CoverageMap filtered =
                        applyExcludes(*parsed, excludes);

                    double overall = overallPercent(filtered);
                    r.outputs["coverage-percent"] =
                        std::to_string(overall);
                    r.outputs["coverage-file-count"] =
                        std::to_string(filtered.files.size());
                    r.outputs["coverage-grain"] = filtered.grain;

                    std::map<std::string, std::string> reports;
                    if (const auto* ro =
                            coverageObj->getObject("report")) {
                        for (const auto& [k, v] : *ro) {
                            auto s = v.getAsString();
                            if (!s) continue;
                            auto resolved = ctx.substitute(
                                s->str(),
                                "test.coverage.report." + k.str());
                            if (!resolved) return resolved.takeError();
                            reports[k.str()] = *resolved;
                        }
                    }
                    if (!reports.empty()) {
                        if (auto e = renderCoverageReports(
                                filtered, reports)) {
                            return std::move(e);
                        }
                        for (const auto& [fmt, path] : reports) {
                            r.outputs["coverage-report-" + fmt] = path;
                        }
                    }

                    double minOverall = -1.0, minPerFile = -1.0;
                    if (auto v = coverageObj->getNumber("min")) {
                        minOverall = *v;
                    } else if (auto v = coverageObj->getInteger("min")) {
                        minOverall = static_cast<double>(*v);
                    }
                    if (auto v =
                            coverageObj->getNumber("min-per-file")) {
                        minPerFile = *v;
                    } else if (auto v =
                            coverageObj->getInteger("min-per-file")) {
                        minPerFile = static_cast<double>(*v);
                    }
                    auto tr = checkThresholds(
                        filtered, minOverall, minPerFile);
                    if (tr.violated) return err(tr.detail);
                }
            }
            return r;
        }
    };

    std::unique_ptr<Action> makeTestAction() {
        return std::make_unique<TestAction>();
    }

} // namespace cajeta::buildtool
