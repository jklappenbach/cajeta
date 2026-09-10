// The `cajeta kernel` verb (jupyter-kernel spec 3.1).
#pragma once

#include <string>

namespace cajeta::kernel {

    // `cajeta kernel [-f <file>]`; with no connection file one is generated, written to the runtime dir and printed.
    int dispatchKernel(int argc, const char* argv[]);

    // The `kernel.json` a Jupyter kernelspec directory needs, pointing at `executable`.
    std::string kernelSpecJson(const std::string& executable);

    // Installs that kernelspec into the user's Jupyter data directory, refusing to
    // overwrite unless `force`; returns the kernel.json path, or empty with `error` set.
    std::string installKernelSpec(const std::string& executable, bool force,
                                  std::string* error);

}  // namespace cajeta::kernel
