// Build-tool sandbox: a thin abstraction over the host's confinement primitive
// (Linux bwrap; macOS and Windows are stubs today). The unit of confinement is one
// subprocess, so a forking action wraps its argv/env before fork() + execvp().

#pragma once

#include <llvm/Support/Error.h>

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // Confinement policy: each action declares the minimum capabilities it needs.
    enum class Capability {
        Filesystem,   // read+write under the project root + dep cache
        Process,      // fork+exec other processes (compiler, tar, …)
        Network,      // outbound network (curl, git fetch, registry)
        Env,          // read host environment beyond the safe baseline
    };

    struct SandboxPolicy {
        std::set<Capability> capabilities;
        // Always writable; everything outside is read-only unless listed below.
        std::string projectRoot;
        std::vector<std::string> readWritePaths;
        std::vector<std::string> readOnlyPaths;
        // Extra env vars for the child; PATH and a project-local HOME are always there.
        std::set<std::string> envPassthrough;
        // No-op mode for `--no-sandbox`: argv and env come back unchanged.
        bool disabled = false;
    };

    // What the sandbox produced for the caller to fork+exec.
    struct SandboxedInvocation {
        std::vector<std::string> argv;
        std::vector<std::string> envEntries;     // KEY=VALUE form
        std::string strategy;                    // "bwrap" | "passthrough" | "macos-stub" | …
        std::vector<std::string> notes;          // diagnostics (e.g. "bwrap not installed")
    };

    // Whether the host has a working sandbox primitive, so an unprovisioned CI box can
    // pick `--no-sandbox` on its own.
    bool hostSandboxAvailable();

    // Wrap `argv` per `policy`. Errors only on an internally inconsistent policy; a
    // missing host primitive degrades to passthrough plus a note in the result.
    llvm::Expected<SandboxedInvocation> wrapInSandbox(
        const SandboxPolicy& policy,
        const std::vector<std::string>& argv,
        const std::vector<std::string>& envEntries);

    // The canonical capability set for a native action, nullopt for an unknown one:
    // plugins declare their own list, validated against the consumer's allowlist.
    std::optional<std::set<Capability>>
    nativeActionCapabilities(const std::string& actionName);

    // Convert a capability to and from its on-the-wire string form.
    std::string capabilityToString(Capability c);
    std::optional<Capability> capabilityFromString(
        const std::string& s);

    // The first requested capability the allowlist does not permit, for diagnostic
    // citation, or nullopt when every request is allowed.
    std::optional<Capability> firstDisallowedCapability(
        const std::set<Capability>& requested,
        const std::set<Capability>& allowed);

} // namespace cajeta::buildtool
