// `cajeta ide <subcommand>` — IDE plugin management (plan/installer-plan.md §7).
//
// The IntelliJ IDEA plugin is embedded in the compiler binary (mirror of the
// embedded stdlib / runtime bitcode), so `cajeta ide install` works from any
// distribution form — package manager, MSI, .pkg, or a bare tarball — with no
// separate download. This is the cross-platform install path (D8): the only
// route on Linux, and an alternative to the Marketplace everywhere.
//
// Subcommands:
//   install     extract the bundled plugin into every detected IntelliJ IDEA
//   uninstall   remove a previously installed bundled plugin
//   list        show detected IDEA installs and whether the plugin is present
//
// `--plugins-dir=<path>` overrides auto-detection (targets one explicit
// plugins directory) — for power users and testing.

#pragma once

namespace cajeta {

    // Dispatch `cajeta ide <subcommand> [args...]`. argc/argv are passed
    // unchanged from main; the subcommand is at argv[2]. Returns the process
    // exit code.
    int dispatchIde(int argc, const char* argv[]);

} // namespace cajeta
