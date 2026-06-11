// `cajeta archive <subcommand>` — archive-management CLI surface.
// Full feature catalog and exit-code semantics live in
// docs/ArchiveManagement.md.
//
// Today's binary still uses `cajeta` as the compiler entry; archive
// subcommands sit at the top of main() and short-circuit when argv[1]
// is "archive". When BuildTool.md ships and the binary splits into
// `cajetac` (compiler) + `cajeta` (umbrella), this dispatch moves to
// the umbrella side — the implementation here links against the same
// libcajeta_lib that the compiler does, so nothing in this file
// changes during the migration.

#pragma once

namespace cajeta {

    // Dispatch `cajeta archive <subcommand> [args...]`.
    // argc/argv are passed unchanged from main; this function locates
    // the subcommand at argv[2] and routes to the right handler.
    // Returns the process exit code (see ArchiveManagement.md §4).
    int dispatchArchive(int argc, const char* argv[]);

} // namespace cajeta
