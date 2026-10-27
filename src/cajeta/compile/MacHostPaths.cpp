#include "MacHostPaths.h"

namespace cajeta {

    std::vector<std::string> macOpenSslLinkArgs(
            const std::string& compilerExeDir,
            const std::string& configuredLibDir,
            const std::function<bool(const std::string&)>& exists) {
        const std::string deps = compilerExeDir + "/cajeta-deps/";
        const std::string ssl = deps + "libssl.a";
        const std::string crypto = deps + "libcrypto.a";
        if (exists(ssl) && exists(crypto)) return {ssl, crypto};
        std::vector<std::string> args;
        if (!configuredLibDir.empty()) args.push_back("-L" + configuredLibDir);
        args.push_back("-lssl");
        args.push_back("-lcrypto");
        return args;
    }

}
