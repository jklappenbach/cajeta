#include "cajeta/compile/WindowsHostPaths.h"

#include <cctype>

namespace cajeta {

    std::string msysDriveToWindows(const std::string& path) {
        // `/x` or `/x/...` with x a letter; `//server` and `/home` are not drives.
        if (path.size() < 2 || path[0] != '/'
                || !std::isalpha(static_cast<unsigned char>(path[1]))) {
            return path;
        }
        if (path.size() > 2 && path[2] != '/') {
            return path;
        }
        std::string out(1, static_cast<char>(std::toupper(static_cast<unsigned char>(path[1]))));
        out += ":/";
        if (path.size() > 3) {
            out += path.substr(3);
        }
        return out;
    }

    std::vector<std::string> msysLinkDriverFallbacks(
            const std::vector<std::string>& roots,
            const std::function<bool(const std::string&)>& exists) {
        std::vector<std::string> out;
        for (const auto& root : roots) {
            for (const char* drv : {"gcc.exe", "clang.exe"}) {
                std::string p = root + "/mingw64/bin/" + drv;
                if (exists(p)) {
                    out.push_back(p);
                }
            }
        }
        return out;
    }

}
