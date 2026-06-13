#include "cajeta/buildtool/Sandbox.h"

#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // Canonical capability list shipped in capabilities-v1.
        // Mirrors what plugins declare via `settings.capabilities`
        // and what the sandbox layer enforces.
        const std::map<std::string, Capability>& capByName() {
            static const std::map<std::string, Capability> m = {
                {"filesystem", Capability::Filesystem},
                {"process",    Capability::Process},
                {"network",    Capability::Network},
                {"env",        Capability::Env},
            };
            return m;
        }

    } // namespace

    std::string capabilityToString(Capability c) {
        for (const auto& kv : capByName()) {
            if (kv.second == c) return kv.first;
        }
        return "<unknown>";
    }

    std::optional<Capability> capabilityFromString(const std::string& s) {
        auto it = capByName().find(s);
        if (it == capByName().end()) return std::nullopt;
        return it->second;
    }

    std::optional<Capability> firstDisallowedCapability(
        const std::set<Capability>& requested,
        const std::set<Capability>& allowed) {
        for (auto c : requested) {
            if (!allowed.count(c)) return c;
        }
        return std::nullopt;
    }

    std::optional<std::set<Capability>>
    nativeActionCapabilities(const std::string& actionName) {
        using C = Capability;
        // Hand-tabulated catalog — one entry per action registered in
        // ActionRegistry (Action.cpp). Keep in sync with that registry and
        // docs/BuildTool.md "Native action catalog". (Planned actions such as
        // lint/doc/fmt are intentionally absent until they register.)
        static const std::map<std::string, std::set<C>> table = {
            {"exec",       {C::Filesystem, C::Process, C::Env}},
            {"copy",       {C::Filesystem}},
            {"delete",     {C::Filesystem}},
            {"mkdir",      {C::Filesystem}},
            {"sign",       {C::Filesystem}},
            {"verify-sig", {C::Filesystem}},
            {"version",    {C::Filesystem}},
            {"download",   {C::Filesystem, C::Network, C::Process}},
            {"build",      {C::Filesystem, C::Process}},
            {"clean",      {C::Filesystem}},
            {"test",       {C::Filesystem, C::Process}},
            {"package",    {C::Filesystem, C::Process}},
            {"upload",     {C::Filesystem, C::Network, C::Process}},
            {"publish",    {C::Filesystem, C::Network, C::Process}},
        };
        auto it = table.find(actionName);
        if (it == table.end()) return std::nullopt;
        return it->second;
    }

    bool hostSandboxAvailable() {
#ifdef __linux__
        auto p = llvm::sys::findProgramByName("bwrap");
        return static_cast<bool>(p);
#else
        return false;
#endif
    }

    namespace {

        // Build the bwrap argv for a single confined invocation.
        // The shape we emit:
        //
        //   bwrap --die-with-parent
        //         --new-session
        //         --proc /proc
        //         --dev /dev
        //         --tmpfs /tmp
        //         --bind <projectRoot> <projectRoot>
        //         [--ro-bind <ro> <ro> …]
        //         [--bind <rw> <rw> …]
        //         [--unshare-net]                   (when !network)
        //         [--clearenv]
        //         [--setenv K V …]
        //         -- <inner argv>
        std::vector<std::string> buildBwrapArgv(
            const SandboxPolicy& policy,
            const std::vector<std::string>& innerArgv,
            const std::vector<std::string>& envEntries) {
            std::vector<std::string> out;
            out.push_back("bwrap");
            out.push_back("--die-with-parent");
            out.push_back("--new-session");
            out.push_back("--proc");
            out.push_back("/proc");
            out.push_back("--dev");
            out.push_back("/dev");
            out.push_back("--tmpfs");
            out.push_back("/tmp");
            // System read-only mounts so the compiler can find its
            // own libraries. The Linux sandbox spec calls for the
            // build to see only the project + dep cache; bwrap
            // doesn't ship a usable libc/loader so we mount /usr +
            // /lib + /etc/alternatives read-only.
            for (const char* sys : {"/usr", "/lib", "/lib64", "/bin",
                                     "/sbin", "/etc/alternatives"}) {
                if (std::filesystem::exists(sys)) {
                    out.push_back("--ro-bind-try");
                    out.push_back(sys);
                    out.push_back(sys);
                }
            }
            if (!policy.projectRoot.empty()) {
                out.push_back("--bind");
                out.push_back(policy.projectRoot);
                out.push_back(policy.projectRoot);
            }
            for (const auto& p : policy.readWritePaths) {
                out.push_back("--bind");
                out.push_back(p);
                out.push_back(p);
            }
            for (const auto& p : policy.readOnlyPaths) {
                out.push_back("--ro-bind-try");
                out.push_back(p);
                out.push_back(p);
            }
            if (!policy.capabilities.count(Capability::Network)) {
                out.push_back("--unshare-net");
            }
            // Always start from a clean environment; explicit
            // passthrough below.
            out.push_back("--clearenv");
            // PATH and HOME are baseline. HOME is faked to /tmp so
            // tools that consult $HOME don't reach the host's.
            out.push_back("--setenv");
            out.push_back("PATH");
            out.push_back("/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
            out.push_back("--setenv");
            out.push_back("HOME");
            out.push_back("/tmp");
            for (const auto& kv : envEntries) {
                auto eq = kv.find('=');
                if (eq == std::string::npos) continue;
                out.push_back("--setenv");
                out.push_back(kv.substr(0, eq));
                out.push_back(kv.substr(eq + 1));
            }
            out.push_back("--");
            for (const auto& a : innerArgv) out.push_back(a);
            return out;
        }

    } // namespace

    llvm::Expected<SandboxedInvocation> wrapInSandbox(
        const SandboxPolicy& policy,
        const std::vector<std::string>& argv,
        const std::vector<std::string>& envEntries) {
        if (argv.empty()) {
            return err("sandbox: empty argv");
        }
        SandboxedInvocation r;
        if (policy.disabled) {
            r.argv = argv;
            r.envEntries = envEntries;
            r.strategy = "passthrough";
            r.notes.push_back("sandbox disabled (--no-sandbox or policy.disabled)");
            return r;
        }
        if (!hostSandboxAvailable()) {
            r.argv = argv;
            r.envEntries = envEntries;
#ifdef __linux__
            r.strategy = "passthrough";
            r.notes.push_back(
                "bwrap not found on PATH — sandbox degraded to "
                "passthrough; install bubblewrap to confine builds");
#elif defined(__APPLE__)
            r.strategy = "macos-stub";
            r.notes.push_back(
                "macOS sandbox-exec wrapper deferred — passthrough "
                "in v1");
#elif defined(_WIN32)
            r.strategy = "windows-stub";
            r.notes.push_back(
                "Windows Job-object wrapper deferred — passthrough "
                "in v1");
#else
            r.strategy = "passthrough";
            r.notes.push_back("no sandbox primitive on this platform");
#endif
            return r;
        }
#ifdef __linux__
        r.argv = buildBwrapArgv(policy, argv, envEntries);
        r.strategy = "bwrap";
        return r;
#else
        r.argv = argv;
        r.envEntries = envEntries;
        r.strategy = "passthrough";
        return r;
#endif
    }

} // namespace cajeta::buildtool
