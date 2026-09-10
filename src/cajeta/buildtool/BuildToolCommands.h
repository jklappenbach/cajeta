// Top-of-main dispatcher for cajeta build-tool subcommands: the build-tool
// surface attaches as a short-circuit at the start of main, the same pattern
// `cajeta archive` uses, and falls through to the compiler when it matches none.

#pragma once

namespace cajeta::buildtool {

    // True when `argv[1]` is a known build-tool subcommand, in which case the
    // invocation is consumed and `*exitCodeOut` set. False means the caller
    // should fall through to the compiler entry point.
    bool dispatchBuildTool(int argc, const char* argv[], int* exitCodeOut);

} // namespace cajeta::buildtool
