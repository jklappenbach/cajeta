// `cajeta ide <subcommand>` — IDE plugin management: install / uninstall / list
// for the IntelliJ plugin embedded in the compiler binary, with
// `--plugins-dir=<path>` overriding IDEA auto-detection. See installer-plan §7.

#pragma once

namespace cajeta {

    // Dispatches `cajeta ide <subcommand> [args...]` — argc/argv unchanged from
    // main, subcommand at argv[2]. Returns the process exit code.
    int dispatchIde(int argc, const char* argv[]);

} // namespace cajeta
