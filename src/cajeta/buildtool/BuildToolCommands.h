// Top-of-main dispatcher for cajeta build-tool subcommands.
//
// The cajeta binary today is the compiler. The build tool surface
// (`cajeta build`, `cajeta test`, `cajeta info`, etc.) attaches here as
// a short-circuit at the start of main, matching the existing pattern
// used by `cajeta archive` (src/cajeta/cli/ArchiveCommands.h).
// Eventually `cajeta` becomes the umbrella binary and `cajetac` the
// compiler back-end; this dispatch moves to the umbrella side without
// the implementation changing.
//
// Phase 0 implements only `cajeta info`. Task invocation,
// `cajeta build`, `cajeta tasks`, etc. follow in later phases per
// plans/buildtool/build-tool-plan.md.

#pragma once

namespace cajeta::buildtool {

    // Returns true if `argv[1]` is a build-tool subcommand this
    // dispatcher knows about (in which case it has consumed the
    // invocation and set `*exitCodeOut`). Returns false when the
    // invocation should fall through to the compiler entry point.
    //
    // Built-in subcommands recognized as of Phase 0:
    //   cajeta info [--manifest=<path>] [--properties]
    //
    // More land in subsequent phases.
    bool dispatchBuildTool(int argc, const char* argv[], int* exitCodeOut);

} // namespace cajeta::buildtool
