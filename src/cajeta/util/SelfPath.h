// The path of the running executable, in one place for every caller.
#pragma once

#include <string>

namespace cajeta::util {

    // Returns /proc/self/exe on Linux, GetModuleFileName on Windows and
    // _NSGetExecutablePath on macOS; empty when the platform will not say.
    std::string runningExecutablePath();

} // namespace cajeta::util
