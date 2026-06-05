#include "cajeta/cli/DocCommand.h"

#ifdef CAJETA_HAS_DOC
#include "cajetadoc/Cli.h"
#else
#include <iostream>
#endif

namespace cajeta {
namespace doc {

int dispatchDoc(int argc, const char* const* argv) {
#ifdef CAJETA_HAS_DOC
    // Drop the leading "cajeta" so the shared CLI sees argv[0] == "doc" and
    // begins option parsing at the source-root argument — i.e. `cajeta doc
    // <root> …` is handled identically to `cajetadoc <root> …`.
    return cajetadoc::runCli(argc - 1, argv + 1);
#else
    (void)argc;
    (void)argv;
    std::cerr << "cajeta doc: documentation generator not built "
                 "(configure with -DCAJETA_BUILD_CAJETADOC=ON)\n";
    return 2;
#endif
}

} // namespace doc
} // namespace cajeta
