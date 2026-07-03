// Cajeta build-tool toolchain provisioning + dispatch — Phase 14.
//
// The toolchain block lets the manifest pin the cajeta compiler
// version + distribution so the build is reproducible across hosts
// that have multiple cajeta versions installed. When the running
// binary doesn't match the resolved pin, the build tool transparently
// re-execs into the pinned version (rustup model).
//
// Surfaces:
//
//   1. `settings.toolchain` manifest block — version + distribution
//      + channel + sha256 + fetch policy + repository override.
//   2. `.cajeta-toolchain` project-local one-line override file —
//      shorthand `<distribution>:<version>` (e.g. `official:1.0.3`).
//   3. CAJETA_NO_DISPATCH env escape hatch — runs the PATH binary
//      regardless of pin.
//   4. `cajeta toolchain {list,install,remove,default,pin,which,
//      show}` subcommands.
//
// Toolchain store layout:
//
//   ~/.cajeta/toolchains/
//     <distribution>/
//       <version>/
//         bin/cajeta           (the actual compiler binary)
//         lib/…                (runtime libraries)
//         share/…              (templates, samples)
//     current -> <dist>/<version>/    (the default symlink)
//
// Reserved distribution names: official, nightly, lts, system.

#pragma once

#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>

#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // The fetch policy a manifest pin asks the dispatcher to apply
    // when the resolved toolchain isn't already installed.
    enum class FetchPolicy {
        Auto,    // download + verify + install + dispatch
        Warn,    // warn-and-proceed with the running toolchain
        Error,   // refuse + suggest the install command
        Off,     // skip the check entirely (advanced; CI scenarios)
    };

    // Parsed `settings.toolchain` block. All fields are optional
    // except `version`. `distribution` defaults to "official".
    struct ToolchainPin {
        std::string version;            // required ("1.0.3", "2.0.0-rc1", …)
        std::string distribution;       // default "official"
        std::optional<std::string> channel;       // "stable" / "beta" / "nightly" / "lts"
        std::optional<std::string> sha256;        // pin to a specific archive checksum
        FetchPolicy fetch = FetchPolicy::Auto;
        std::optional<std::string> fromRepo;      // repository name to fetch from
    };

    // Result of resolving the effective toolchain pin. Precedence:
    //   1. `.cajeta-toolchain` project-local override (if present)
    //   2. `settings.toolchain` manifest block (if present)
    //   3. No pin → empty (caller dispatches to the running binary)
    struct ResolvedToolchain {
        bool hasPin = false;
        ToolchainPin pin;
        // Path the override was loaded from when (1) applied;
        // path of the manifest when (2) applied; empty otherwise.
        std::string sourcePath;
    };

    // String representations for FetchPolicy. Round-trip safe:
    // `fetchPolicyFromString(fetchPolicyToString(p)) == p`.
    std::string fetchPolicyToString(FetchPolicy p);
    std::optional<FetchPolicy> fetchPolicyFromString(
        const std::string& s);

    // Returns true iff `distribution` is one of the reserved names
    // the registry protocol owns. User-named distributions are
    // rejected from using these.
    bool isReservedDistribution(const std::string& distribution);
    const std::vector<std::string>& reservedDistributions();

    // Parse the `settings.toolchain` block out of a manifest.
    // Returns std::nullopt when the block is absent. Errors on
    // missing required fields / unknown subfields / invalid
    // fetch-policy strings / reserved distribution misuse.
    llvm::Expected<std::optional<ToolchainPin>> parseToolchainPin(
        const Manifest& m);

    // Read a `.cajeta-toolchain` file. The format is a single line
    // `<distribution>:<version>` (with optional leading/trailing
    // whitespace + a trailing newline). Errors on malformed
    // contents; returns std::nullopt when the file is absent.
    llvm::Expected<std::optional<ToolchainPin>>
    readToolchainOverrideFile(const std::string& projectRoot);

    // Resolve the effective toolchain pin for a given manifest +
    // project root. Implements the precedence rule documented
    // above. The manifest may be a null pointer when called
    // outside a project (returns hasPin=false).
    llvm::Expected<ResolvedToolchain> resolveEffectiveToolchain(
        const Manifest* manifest,
        const std::string& projectRoot);

    // Toolchain-store layout on the current host. The layout is
    // overridable via the CAJETA_TOOLCHAIN_HOME env var (CI knob)
    // or a constructor argument (for tests).
    struct ToolchainStoreLayout {
        std::string root;               // <home>/.cajeta/toolchains
        // Convenience: <root>/<dist>/<version>/bin/cajeta
        std::string binaryPath(const std::string& distribution,
                               const std::string& version) const;
        // <root>/<dist>/<version>
        std::string installRoot(const std::string& distribution,
                                const std::string& version) const;
        // <root>/current — the workstation-wide default symlink.
        std::string defaultSymlinkPath() const;
    };
    ToolchainStoreLayout resolveToolchainStoreLayout(
        const std::string& homeOverride = "");

    // Enumerate installed toolchains by walking the store. Returns
    // entries sorted (distribution asc, version desc within a dist).
    struct InstalledToolchain {
        std::string distribution;
        std::string version;
        std::string installRoot;
        bool isDefault = false;     // matches the `current` symlink
    };
    std::vector<InstalledToolchain> listInstalledToolchains(
        const ToolchainStoreLayout& layout);

    // Predicate: does the running cajeta binary satisfy the
    // resolved pin? The check compares CAJETA_VERSION (set at
    // build time) against `pin.version`. Returns false when the
    // pin is absent (caller treats as "any binary OK").
    //
    // `runningVersion` is injectable for tests; pass an empty
    // string to read CAJETA_VERSION directly.
    bool runningBinarySatisfiesPin(const ResolvedToolchain& tc,
                                    const std::string& runningVersion = "");

    // Should the dispatcher re-exec? Returns the resolved binary
    // path when yes; std::nullopt when no.
    //
    // The full precedence:
    //
    //   1. CAJETA_NO_DISPATCH=1     → std::nullopt (env escape hatch)
    //   2. tc.hasPin == false        → std::nullopt (no pin to satisfy)
    //   3. fetch policy == Off       → std::nullopt (skip the check)
    //   4. running binary matches    → std::nullopt
    //   5. pinned binary installed   → its absolute path
    //   6. pinned binary NOT installed:
    //        fetch=Auto  → caller's responsibility to install,
    //                      then re-resolve (we just say "install")
    //        fetch=Warn  → std::nullopt + warn-message in `notes`
    //        fetch=Error → llvm::Error with install hint
    //        fetch=Off   → std::nullopt
    //
    // The return type carries either the resolved binary path
    // (caller should re-exec) or a status code for the alternative
    // outcomes.
    enum class DispatchAction {
        Continue,           // run the existing binary
        ReExec,             // execve the path in `resolvedBinaryPath`
        NeedsInstall,       // ask the caller to fetch then re-resolve
    };
    struct DispatchDecision {
        DispatchAction action = DispatchAction::Continue;
        std::string resolvedBinaryPath;   // populated when action == ReExec
        std::string installHint;          // populated when action == NeedsInstall
        std::vector<std::string> notes;    // diagnostics (warn-and-proceed)
    };
    llvm::Expected<DispatchDecision> computeDispatchDecision(
        const ResolvedToolchain& tc,
        const ToolchainStoreLayout& layout,
        const std::string& runningVersion = "");

    // Compose a deterministic toolchain identity string for the IR
    // cache discriminator. Shape: "<dist>:<version>[:<sha256>]".
    // When the pin is absent, returns the running version as
    // "official:<runningVersion>" so single-machine builds without
    // a pin still get a stable cache key.
    std::string toolchainIdentity(const ResolvedToolchain& tc,
                                   const std::string& runningVersion = "");

} // namespace cajeta::buildtool
