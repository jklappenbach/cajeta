// Cajeta build-tool sandbox + reproducible-build plumbing — Phase 11.
//
// The sandbox is a thin abstraction over the host's confinement
// primitives:
//
//   - Linux:   bwrap (bubblewrap) — fast, supported by every modern
//              distro; falls back gracefully to "no sandbox + warning"
//              when bwrap isn't installed.
//   - macOS:   sandbox-exec profile (deferred slice — wrapper emits
//              the unwrapped argv today).
//   - Windows: Job objects + restricted descriptors (deferred slice
//              — same).
//
// The unit of confinement is a single subprocess invocation. Actions
// that fork themselves (build / exec / test / package / upload)
// consult `wrapInSandbox()` to translate `(policy, argv, env)` into
// a confined `(argv', env')` pair before `fork() + execvp()`.
//
// Reproducible-build helpers live in `Reproducibility.{h,cpp}` so
// the sandbox surface stays focused on confinement; both ship in
// Phase 11.

#pragma once

#include <llvm/Support/Error.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // Confinement policy. Each action declares the minimum set of
    // capabilities it needs; the sandbox layer enforces them.
    enum class Capability {
        Filesystem,   // read+write under the project root + dep cache
        Process,      // fork+exec other processes (compiler, tar, …)
        Network,      // outbound network (curl, git fetch, registry)
        Env,          // read host environment beyond the safe baseline
    };

    struct SandboxPolicy {
        std::set<Capability> capabilities;
        // Project root — always writable. Anything outside is
        // read-only by default; add more dirs via `readWritePaths`
        // or `readOnlyPaths`.
        std::string projectRoot;
        std::vector<std::string> readWritePaths;
        std::vector<std::string> readOnlyPaths;
        // Env vars passed through to the child. PATH + HOME (faked
        // to project-local cache) are always present; this list
        // adds anything else the action explicitly needs.
        std::set<std::string> envPassthrough;
        // When true the sandbox layer is a no-op — the caller's
        // argv/env is returned unchanged. Used by the `--no-sandbox`
        // debug flag.
        bool disabled = false;
    };

    // What the sandbox produced for the caller to fork+exec.
    struct SandboxedInvocation {
        std::vector<std::string> argv;
        std::vector<std::string> envEntries;     // KEY=VALUE form
        std::string strategy;                    // "bwrap" | "passthrough" | "macos-stub" | …
        std::vector<std::string> notes;          // diagnostics (e.g. "bwrap not installed")
    };

    // Returns true when the host has a working sandbox primitive
    // available (Linux: bwrap on PATH; other platforms: never in
    // v1). Callers use this to pick `--no-sandbox` automatically in
    // CI environments that haven't been provisioned yet.
    bool hostSandboxAvailable();

    // Wrap `argv` with the host's sandbox primitive per `policy`.
    // Errors only when the policy is internally inconsistent
    // (e.g. an empty `argv`); a missing host primitive degrades to
    // passthrough + a note in the result.
    llvm::Expected<SandboxedInvocation> wrapInSandbox(
        const SandboxPolicy& policy,
        const std::vector<std::string>& argv,
        const std::vector<std::string>& envEntries);

    // The canonical capability set for each native action name.
    // Returns std::nullopt for unknown actions (plugins declare
    // their own capability list via plugin metadata, validated
    // against the consumer's allowlist).
    std::optional<std::set<Capability>>
    nativeActionCapabilities(const std::string& actionName);

    // Convert a capability to / from its on-the-wire string form.
    // Used by manifest / lockfile / log output.
    std::string capabilityToString(Capability c);
    std::optional<Capability> capabilityFromString(
        const std::string& s);

    // Validate a per-action capability request against the
    // consumer's `settings.capabilities` allowlist. Returns the
    // first disallowed capability for diagnostic citation, or
    // std::nullopt when every request is allowed.
    std::optional<Capability> firstDisallowedCapability(
        const std::set<Capability>& requested,
        const std::set<Capability>& allowed);

} // namespace cajeta::buildtool
