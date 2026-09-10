// Build-tool property resolution: ${PROPERTY} substitution over one flat
// namespace of built-ins and user `properties`, with override precedence
// CLI -P > CAJETA_PROPERTY_* env > active profile > manifest properties.

#pragma once

#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // Everything a resolution pass takes beyond the manifest: `cli` from
    // `-P NAME=VALUE`, `env` from CAJETA_PROPERTY_*, and the flavor / profile /
    // target context built-ins need. `workspaceRoot` defaults to the manifest's.
    struct PropertyOverrides {
        std::map<std::string, std::string> cli;
        std::map<std::string, std::string> env;
        std::optional<std::string> flavor;
        std::optional<std::string> profile;
        std::optional<std::string> target;
        std::optional<std::string> workspaceRoot;
    };

    // The flat name→value table plus the topological resolution order; the fixed
    // built-ins (`details.*`, `flavor`, `profile`, `target`, …) land eagerly.
    struct ResolvedProperties {
        std::map<std::string, std::string> values;
        std::vector<std::string> resolutionOrder;

        // Covers `values` plus lazy `env.*` / `artifact.*`; nullopt = undefined.
        std::optional<std::string> lookup(const std::string& name) const;
    };

    // Errors on a cyclic reference, a name colliding with a built-in, or a
    // missing reference — the last only once something evaluates it.
    llvm::Expected<ResolvedProperties> resolveProperties(
        const Manifest& manifest,
        const PropertyOverrides& overrides = {});

    // Substitutes every ${NAME} in `s`; an unresolvable one is an error citing
    // the reference and `whereContext`.
    llvm::Expected<std::string> substitute(
        const std::string& s,
        const ResolvedProperties& props,
        const std::string& whereContext = "<unknown>");

    // Rewrites ${NAME} in the `settings` and `plugins` blocks in place. Must run
    // after resolveProperties and before anything parses those blocks. `tasks`
    // is left alone: its references are late-bound and only TaskContext answers.
    llvm::Error substituteManifestProperties(
        Manifest& manifest, const ResolvedProperties& props);

    // Fills `dst.env` from CAJETA_PROPERTY_* vars, lowercasing the suffix and
    // turning its underscores into hyphens (…_STACK_VERSION → "stack-version").
    void loadEnvOverrides(PropertyOverrides& dst);

    // Splits one `-P` / `--property=` token at its first `=`; the value keeps any
    // later `=`. Malformed input is an error, not a silent skip.
    llvm::Expected<std::pair<std::string, std::string>> parseCliOverride(
        const std::string& token);

} // namespace cajeta::buildtool
