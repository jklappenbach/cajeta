#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::util {

// Portable process-environment writes: the Windows CRT has no setenv/unsetenv and
// spells both `_putenv_s`. Both write the CRT environment the JIT reads back.
void setEnvVar(const std::string& name, const std::string& value);
void unsetEnvVar(const std::string& name);

/** Applies an environment overlay and puts back exactly what it displaced, from
 *  the destructor, because the debugger's JIT runs IN-PROCESS and would otherwise
 *  leak one session's variables into the next. Not thread-safe. */
class EnvironmentScope {
public:
    EnvironmentScope() = default;
    ~EnvironmentScope() { restore(); }

    EnvironmentScope(const EnvironmentScope&) = delete;
    EnvironmentScope& operator=(const EnvironmentScope&) = delete;

    /** Overlay `vars`; `inheritParent` false unsets every other variable for the
     *  duration, true lets a configured entry beat an inherited one. Re-applying
     *  keeps the FIRST value seen for a name as the one that will be restored. */
    void apply(const std::map<std::string, std::string>& vars, bool inheritParent);

    /** Put back every variable this scope changed. Safe to call twice. */
    void restore();

private:
    // Record a name's current value (or its absence) before first touching it.
    void remember(const std::string& name);

    // Name -> prior value (nullopt = unset), in touch order; restore walks it
    // backwards so the earliest snapshot wins on a duplicate.
    std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

} // namespace cajeta::util
