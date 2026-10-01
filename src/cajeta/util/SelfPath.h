// The path and identity of the running executable, in one place for every caller.
#pragma once

#include <string>

namespace cajeta::util {

    // Returns /proc/self/exe on Linux, GetModuleFileName on Windows and
    // _NSGetExecutablePath on macOS; empty when the platform will not say.
    std::string runningExecutablePath();

    // VERSION+GIT_HASH plus the binary's content identity: "+bid=<build id>" from
    // the ELF note or Mach-O UUID, else "+bin=<size>:<mtime>" of the executable.
    const std::string& compilerIdentity();

} // namespace cajeta::util
