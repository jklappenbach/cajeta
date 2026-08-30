// The path of the running executable.
//
// Two callers need it for different reasons and must not each carry their own
// platform block: `cajeta artifact-path` re-invokes this binary, and
// `--lint-server` stamps its own binary's identity on the ready handshake so a
// client can tell a resident daemon apart from the compiler it means to call.
#pragma once

#include <string>

namespace cajeta::util {

    // /proc/self/exe on Linux, GetModuleFileName on Windows, _NSGetExecutablePath
    // on macOS. Empty when the platform will not say.
    std::string runningExecutablePath();

} // namespace cajeta::util
