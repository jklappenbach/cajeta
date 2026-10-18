// Host-path conventions a native Windows cajeta.exe meets when driven from
// Git Bash or an MSYS2 shell. Pure functions, so they are tested on every host;
// the call sites apply them only under _WIN32.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace cajeta {

    // An MSYS drive path, `/d/x/y`, as the Windows path `D:/x/y`; any other
    // spelling comes back unchanged. Git Bash converts a lone path argument
    // itself but not the entries of a comma-separated list, so
    // `--classpath=/d/a.cja,/d/b.cja` reaches cajeta.exe in this form.
    std::string msysDriveToWindows(const std::string& path);

    // The MSYS2 mingw64 C drivers under each of `roots` that `exists` reports,
    // gcc before clang: the link-driver fallbacks after the PATH names, for a
    // shell such as Git Bash whose PATH holds no C compiler.
    std::vector<std::string> msysLinkDriverFallbacks(
        const std::vector<std::string>& roots,
        const std::function<bool(const std::string&)>& exists);

}
