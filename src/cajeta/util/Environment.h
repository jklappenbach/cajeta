#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::util {

// Portable process-environment writes. POSIX `setenv`/`unsetenv` do not exist
// in the Windows CRT, which spells both `_putenv_s` — the same split
// `__cajeta_env_set` handles on the C runtime side
// (runtime/native/cajeta_rt_lang.c). Both write the CRT environment that the
// in-process JIT runtime later reads back through getenv().
void setEnvVar(const std::string& name, const std::string& value);
void unsetEnvVar(const std::string& name);

/**
 * Applies an environment overlay and puts back exactly what it displaced.
 *
 * This exists because the debugger's JIT runs IN-PROCESS: applying a launch
 * environment mutates the `cajeta dap` server itself, not a child. Without an
 * undo, the first session's variables leak into every session after it, and a
 * developer who sets FOO once sees it in a configuration that never mentioned
 * FOO (run-config-ergonomics spec 4.1.4 / 4.2.4).
 *
 * Restore runs from the destructor rather than an explicit call, so a session
 * that ends by throwing, by an early return, or by the server being torn down
 * mid-run restores just as reliably as a clean disconnect.
 *
 * Not thread-safe, and neither is the process environment it writes: apply
 * before the program thread starts and restore after it joins.
 */
class EnvironmentScope {
public:
    EnvironmentScope() = default;
    ~EnvironmentScope() { restore(); }

    EnvironmentScope(const EnvironmentScope&) = delete;
    EnvironmentScope& operator=(const EnvironmentScope&) = delete;

    /**
     * Overlay `vars` on the current environment.
     *
     * `inheritParent` false means the debuggee sees ONLY `vars`: every other
     * variable is unset for the duration (spec 4.1.5). True overlays, so a
     * configured entry beats an inherited one of the same name.
     *
     * Idempotent across calls only in the sense that the FIRST value seen for a
     * name is the one restored — re-applying does not lose the original.
     */
    void apply(const std::map<std::string, std::string>& vars, bool inheritParent);

    /** Put back every variable this scope changed. Safe to call twice. */
    void restore();

private:
    // Record a name's current value (or its absence) before first touching it.
    void remember(const std::string& name);

    // name -> value before we touched it; nullopt means it was not set.
    // A vector preserves the order variables were touched in; restore walks it
    // backwards so the earliest snapshot wins on any duplicate.
    std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

} // namespace cajeta::util
