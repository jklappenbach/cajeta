// `cajeta fetch` / `cajeta vendor` — see NativeCommands.h.

#include "cajeta/cli/NativeCommands.h"
#include "cajeta/buildtool/NativeProvision.h"

#include <llvm/Support/Error.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace cajeta {

    namespace {
        std::string homeNativeCache() {
            const char* h = std::getenv("HOME");
            return std::string(h ? h : ".") + "/.cajeta/native";
        }
    } // namespace

    int dispatchNative(int argc, const char* argv[]) {
        const std::string verb = argv[1];
        std::vector<std::string> a;
        for (int i = 2; i < argc; ++i) a.emplace_back(argv[i]);

        if (verb == "fetch") {
            if (a.size() < 4) {
                std::cerr << "usage: cajeta fetch <lib> <version> <platform> "
                             "<source> [sha256]\n"
                             "  Verifies + caches a native artifact into "
                          << homeNativeCache() << "/<lib>/<version>/<platform>/\n";
                return 2;
            }
            std::string sha = a.size() >= 5 ? a[4] : "";
            auto r = buildtool::fetchNativeToCache(
                homeNativeCache(), a[0], a[1], a[2], a[3], sha);
            if (!r) {
                std::cerr << "cajeta fetch: " << llvm::toString(r.takeError())
                          << "\n";
                return 1;
            }
            std::cout << "fetched " << a[0] << " " << a[1] << " (" << a[2]
                      << ") -> " << *r << "\n";
            return 0;
        }

        if (verb == "vendor") {
            if (a.size() < 2) {
                std::cerr << "usage: cajeta vendor <platform> <source> "
                             "[project-native-dir=native]\n"
                             "  Materializes a native artifact into the project "
                             "native/<platform>/ dir.\n";
                return 2;
            }
            std::string projDir = a.size() >= 3 ? a[2] : "native";
            auto r = buildtool::vendorNativeArtifact(projDir, a[0], a[1]);
            if (!r) {
                std::cerr << "cajeta vendor: " << llvm::toString(r.takeError())
                          << "\n";
                return 1;
            }
            std::cout << "vendored -> " << *r << "\n";
            return 0;
        }

        std::cerr << "cajeta: unknown native verb '" << verb << "'\n";
        return 2;
    }

} // namespace cajeta
