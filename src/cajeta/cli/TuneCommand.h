// `cajeta tune --reset <dir>` — empty a tune store (cajeta.xpu.Autotune's
// layout, `<dir>/<device key>/<name>`), so every tuned value and cached shape
// is measured again on next use (xpu-tile-shape-selection spec §7.12).
#pragma once

#include <ostream>
#include <string>

namespace cajeta {
    // Remove every device directory under `dir` and its entries, leaving any
    // other file in `dir` alone. Reports the count to `out`; nonzero when
    // `dir` is not a directory.
    int tuneReset(const std::string& dir, std::ostream& out);

    // argv[1] == "tune". Returns a process exit code.
    int dispatchTune(int argc, const char* argv[]);
}
