#include "cajeta/util/Environment.h"

#include <cstdlib>
#include <set>

#if !defined(_WIN32)
extern char** environ;
#endif

namespace cajeta::util {

void setEnvVar(const std::string& name, const std::string& value) {
#if defined(_WIN32)
    ::_putenv_s(name.c_str(), value.c_str());
#else
    ::setenv(name.c_str(), value.c_str(), /*overwrite=*/1);
#endif
}

void unsetEnvVar(const std::string& name) {
#if defined(_WIN32)
    // The Windows CRT unsets by assigning an empty value.
    ::_putenv_s(name.c_str(), "");
#else
    ::unsetenv(name.c_str());
#endif
}

namespace {

// Every variable name currently in the environment. Collected up front because
// unsetting mutates the strip we would otherwise be iterating.
std::vector<std::string> currentNames() {
    std::vector<std::string> names;
#if defined(_WIN32)
    // The CRT exposes the block as `environ` too (via <stdlib.h>).
    for (char** e = _environ; e && *e; ++e) {
#else
    for (char** e = environ; e && *e; ++e) {
#endif
        const std::string entry(*e);
        const auto eq = entry.find('=');
        if (eq != std::string::npos) names.push_back(entry.substr(0, eq));
    }
    return names;
}

} // namespace

void EnvironmentScope::remember(const std::string& name) {
    const char* prior = ::getenv(name.c_str());
    saved_.emplace_back(name,
                        prior ? std::optional<std::string>(prior) : std::nullopt);
}

void EnvironmentScope::apply(const std::map<std::string, std::string>& vars,
                             bool inheritParent) {
    if (!inheritParent) {
        // Suppress everything the configuration does not declare. Names are
        // collected before the first unset, since unsetenv rewrites `environ`
        // underneath an in-flight walk.
        for (const auto& name : currentNames()) {
            if (vars.count(name)) continue;  // about to be set below anyway
            remember(name);
            unsetEnvVar(name);
        }
    }
    for (const auto& kv : vars) {
        remember(kv.first);
        setEnvVar(kv.first, kv.second);
    }
}

void EnvironmentScope::restore() {
    // Backwards: if a name was touched more than once, the FIRST snapshot is
    // the true original, and applying it last is what makes it win.
    for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
        if (it->second) setEnvVar(it->first, *it->second);
        else unsetEnvVar(it->first);
    }
    saved_.clear();
}

} // namespace cajeta::util
