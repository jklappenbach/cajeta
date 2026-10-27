// Link-line conventions for a macOS --emit=exe. Pure functions, so they are
// tested on every host; the call site applies them only under __APPLE__.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace cajeta {

    // The OpenSSL arguments for linking a user program. A release ships
    // libssl.a and libcrypto.a in cajeta-deps/ beside the compiler binary (the
    // same private directory its bundled dylibs load from), and the
    // program links them by path, so it runs on a Mac without Homebrew. Without
    // both archives (a development build) the configured directory is searched.
    std::vector<std::string> macOpenSslLinkArgs(
        const std::string& compilerExeDir,
        const std::string& configuredLibDir,
        const std::function<bool(const std::string&)>& exists);

}
