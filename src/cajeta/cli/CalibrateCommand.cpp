#include "CalibrateCommand.h"
#include "cajeta/buildtool/Subprocess.h"
#include "cajeta/util/Environment.h"

#include <filesystem>
#include <iostream>
#include <map>

namespace cajeta {
    namespace {
        int runPass(const std::string& program, const std::vector<std::string>& args,
                    const std::map<std::string, std::string>& extra, const char* pass) {
            buildtool::SubprocessOptions opt;
            opt.argv.push_back(program);
            opt.argv.insert(opt.argv.end(), args.begin(), args.end());
            util::EnvironmentScope scope;
            scope.apply(extra, true);
            buildtool::SubprocessResult r = buildtool::runSubprocess(opt);
            if (!r.launched) {
                std::cerr << "cajeta calibrate: cannot run " << program << ": " << r.error << "\n";
                return 2;
            }
            if (r.code() != 0) {
                std::cerr << "cajeta calibrate: the " << pass << " pass exited with " << r.code() << "\n";
                return 1;
            }
            return 0;
        }
    } // namespace

    int calibrate(const std::string& program, const std::vector<std::string>& args,
                  const std::string& out, const std::string& work) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::remove_all(work, ec);
        fs::create_directories(work, ec);
        if (ec) {
            std::cerr << "cajeta calibrate: cannot create " << work << ": " << ec.message() << "\n";
            return 2;
        }
        int rc = runPass(program, args, {{"CAJETA_XPU_RECORD", work},
                                         {"CAJETA_XPU_RECORD_PER_KERNEL", "2"},
                                         {"CAJETA_XPU_RECORD_MAX_BYTES", "1073741824"}}, "record");
        if (rc != 0) return rc;
        if (!fs::exists(fs::path(work) / "index.tsv")) {
            std::cerr << "cajeta calibrate: " << program << " launched no recordable kernel\n";
            return 1;
        }
        return runPass(program, args, {{"CAJETA_XPU_CALIBRATE_REPLAY", work},
                                       {"CAJETA_XPU_CALIBRATE_OUT", out}}, "replay");
    }

    int dispatchCalibrate(int argc, const char* argv[]) {
        std::string program, out, work;
        std::vector<std::string> args;
        bool rest = false;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (rest) {
                args.push_back(a);
            } else if (a == "--") {
                rest = true;
            } else if (a.rfind("--out=", 0) == 0) {
                out = a.substr(6);
            } else if (a.rfind("--work=", 0) == 0) {
                work = a.substr(7);
            } else if (program.empty() && a.rfind("--", 0) != 0) {
                program = a;
            } else {
                std::cerr << "cajeta calibrate: unknown argument '" << a << "'\n";
                return 2;
            }
        }
        if (program.empty()) {
            std::cerr << "usage: cajeta calibrate <program> [--out=<snapshot>] [--work=<dir>] [-- <program args>]\n"
                         "  Runs <program> twice: once recording its kernel launches, once replaying each solo\n"
                         "  on this device. The snapshot (default <program>.calibration) is what\n"
                         "  `cajeta build --with-calibration=<snapshot>` embeds for this device.\n";
            return 2;
        }
        if (out.empty()) out = program + ".calibration";
        if (work.empty()) work = out + ".record";
        return calibrate(program, args, out, work);
    }
}
