// The version an lld names, read from its --version line.
#pragma once

#include <cctype>
#include <string>

namespace cajeta {

    /// The major version in an lld --version line (`LLD 23.0.0 (...)`, `AMD LLD 22.0.0 (...)`),
    /// 0 when the line names none.
    inline int lldMajorOf(const std::string& line) {
        auto at = line.find("LLD ");
        if (at == std::string::npos) return 0;
        at += 4;
        int major = 0;
        bool any = false;
        while (at < line.size() && std::isdigit(static_cast<unsigned char>(line[at]))) {
            major = major * 10 + (line[at] - '0');
            ++at;
            any = true;
        }
        return any ? major : 0;
    }
}
