// `cajeta archive <subcommand>` — archive-management CLI surface. The feature
// catalog and exit-code semantics live in docs/ArchiveManagement.md.

#pragma once

namespace cajeta {

    // Dispatches `cajeta archive <subcommand> [args...]` — argc/argv unchanged from
    // main, subcommand at argv[2]. Returns the process exit code (§4).
    int dispatchArchive(int argc, const char* argv[]);

} // namespace cajeta
