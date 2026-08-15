//
// jupyter-kernel U5 (spec 3.1) — the `cajeta kernel` verb.
//
#pragma once

#include <string>

namespace cajeta::kernel {

    // `cajeta kernel [-f <file> | --connection-file=<file>]`. With no
    // connection file, one is generated (free ports, fresh key), written next
    // to the user's runtime dir and printed, so the kernel can be attached to
    // by hand. Returns the process exit code.
    int dispatchKernel(int argc, const char* argv[]);

    // The `kernel.json` a Jupyter kernelspec directory needs, pointing at
    // `executable`. Written by `cajeta init --kernel`.
    std::string kernelSpecJson(const std::string& executable);

    // Install that kernelspec into the user's Jupyter data directory, so
    // `jupyter kernelspec list` and Lab's launcher find it. Returns the
    // kernel.json path written, or an empty string with a reason in `error`.
    // Refuses to overwrite an existing spec unless `force`.
    std::string installKernelSpec(const std::string& executable, bool force,
                                  std::string* error);

}  // namespace cajeta::kernel
