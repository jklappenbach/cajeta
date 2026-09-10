// `cajeta stdlib <list|extract <dir>>` — access to the `.cajeta` sources embedded in
// the compiler binary: `list` prints one relative path per line, `extract` writes the
// tree under <dir> plus the `.cajeta-stdlib.json` marker the IDE keys its cache on.

#pragma once

namespace cajeta {

    // Dispatches `cajeta stdlib <verb> [args...]` — argc/argv unchanged from main,
    // verb at argv[2]. Returns the process exit code.
    int dispatchStdlib(int argc, const char* argv[]);

} // namespace cajeta
