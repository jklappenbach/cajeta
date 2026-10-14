#pragma once

#include <string>
#include <vector>

namespace cajeta {
    // `cajeta calibrate` (xpu-tile-manifest §14.7): run `program` once recording its kernel
    // launches, then again replaying each launch solo, writing a calibration snapshot to `out`.
    // Returns a process exit code.
    int calibrate(const std::string& program, const std::vector<std::string>& args,
                  const std::string& out, const std::string& work);

    // argv[1] == "calibrate". Returns a process exit code.
    int dispatchCalibrate(int argc, const char* argv[]);
}
