// Cajeta build-tool toolchain provisioning and dispatch: a manifest pins the
// compiler version + distribution, and the tool re-execs into the pinned binary
// at ~/.cajeta/toolchains/<distribution>/<version>/bin/cajeta (rustup model).

#pragma once

#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>

#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // The fetch policy a manifest pin asks the dispatcher to apply when the
    // resolved toolchain is not already installed.
    enum class FetchPolicy {
        Auto,    // download + verify + install + dispatch
        Warn,    // warn-and-proceed with the running toolchain
        Error,   // refuse + suggest the install command
        Off,     // skip the check entirely (advanced; CI scenarios)
    };

    // Parsed `settings.toolchain` block; every field but `version` is optional
    // and `distribution` defaults to "official".
    struct ToolchainPin {
        std::string version;            // required ("1.0.3", "2.0.0-rc1", …)
        std::string distribution;       // default "official"
        std::optional<std::string> channel;       // "stable" / "beta" / "nightly" / "lts"
        std::optional<std::string> sha256;        // pin to a specific archive checksum
        FetchPolicy fetch = FetchPolicy::Auto;
        std::optional<std::string> fromRepo;      // repository name to fetch from
    };

    // The effective pin, by precedence: the `.cajeta-toolchain` project-local
    // override, then the `settings.toolchain` block, then no pin at all — the
    // caller then dispatches to the running binary.
    struct ResolvedToolchain {
        bool hasPin = false;
        ToolchainPin pin;
        std::string sourcePath;   // override file, else manifest, else empty
    };

    // String forms of FetchPolicy, round-trip safe:
    // `fetchPolicyFromString(fetchPolicyToString(p)) == p`.
    std::string fetchPolicyToString(FetchPolicy p);
    std::optional<FetchPolicy> fetchPolicyFromString(
        const std::string& s);

    // True iff `distribution` is one of the names the registry protocol owns;
    // a user-named distribution may not claim one.
    bool isReservedDistribution(const std::string& distribution);
    const std::vector<std::string>& reservedDistributions();

    // Parses the `settings.toolchain` block, std::nullopt when absent; errors on a
    // missing field, unknown subfield, bad fetch policy or reserved distribution.
    llvm::Expected<std::optional<ToolchainPin>> parseToolchainPin(
        const Manifest& m);

    // Reads `<projectRoot>/.cajeta-toolchain`: one `<distribution>:<version>` line,
    // surrounding whitespace allowed. std::nullopt when absent, error when bad.
    llvm::Expected<std::optional<ToolchainPin>>
    readToolchainOverrideFile(const std::string& projectRoot);

    // Applies the precedence above to a manifest plus project root. `manifest`
    // may be null when called outside a project, which yields hasPin = false.
    llvm::Expected<ResolvedToolchain> resolveEffectiveToolchain(
        const Manifest* manifest,
        const std::string& projectRoot);

    // Toolchain-store layout for this host, overridable by CAJETA_TOOLCHAIN_HOME
    // or by the argument to resolveToolchainStoreLayout (tests).
    struct ToolchainStoreLayout {
        std::string root;               // <home>/.cajeta/toolchains
        std::string binaryPath(const std::string& distribution,
                               const std::string& version) const;
        std::string installRoot(const std::string& distribution,
                                const std::string& version) const;
        std::string defaultSymlinkPath() const;   // <root>/current
    };
    ToolchainStoreLayout resolveToolchainStoreLayout(
        const std::string& homeOverride = "");

    // Enumerates installed toolchains by walking the store, sorted by
    // distribution ascending and version descending within a distribution.
    struct InstalledToolchain {
        std::string distribution;
        std::string version;
        std::string installRoot;
        bool isDefault = false;     // matches the `current` symlink
    };
    std::vector<InstalledToolchain> listInstalledToolchains(
        const ToolchainStoreLayout& layout);

    // True when the running binary satisfies the resolved pin, comparing
    // `pin.version` against CAJETA_VERSION; false when there is no pin.
    // `runningVersion` is injectable for tests — empty reads CAJETA_VERSION.
    bool runningBinarySatisfiesPin(const ResolvedToolchain& tc,
                                    const std::string& runningVersion = "");

    // Whether the dispatcher should re-exec, and into what: Continue under
    // CAJETA_NO_DISPATCH=1, no pin, fetch=Off or a matching running binary;
    // ReExec when the pinned binary is installed; else NeedsInstall or an error.
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

    // A deterministic identity for the IR cache discriminator, shaped
    // "<dist>:<version>[:<sha256>]", or "official:<runningVersion>" with no pin.
    std::string toolchainIdentity(const ResolvedToolchain& tc,
                                   const std::string& runningVersion = "");

} // namespace cajeta::buildtool
