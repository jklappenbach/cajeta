// Cajeta build-tool property resolution.
//
// Properties are the unified ${PROPERTY} substitution mechanism for
// the manifest. Built-ins (${details.name}, ${flavor}, ${env.NAME},
// ...) and user-defined properties (from the `properties` block) live
// in the same flat namespace. Override precedence (highest wins):
// CLI -P > CAJETA_PROPERTY_* env > active profile > manifest properties.
//
// See BuildTool.md "Properties" for the spec, plan/build-tool-plan.md
// Phase 1 for context.

#pragma once

#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // Inputs to a resolution pass. The Manifest provides the
    // user-defined `properties` block + built-in-relevant fields
    // (details.name/version, etc.); CLI / env overrides plug in
    // additional values that take precedence over the manifest.
    struct PropertyOverrides {
        // CLI: parsed from `-P NAME=VALUE` flags.
        std::map<std::string, std::string> cli;
        // Env: from CAJETA_PROPERTY_NAME (uppercased + underscores).
        std::map<std::string, std::string> env;
        // Optional flavor / profile / target context that built-ins
        // need to materialize.
        std::optional<std::string> flavor;
        std::optional<std::string> profile;
        std::optional<std::string> target;
        // Workspace root override (defaults to manifest's directory).
        std::optional<std::string> workspaceRoot;
    };

    // Resolved property set. `values` is the flat name→value map;
    // `resolutionOrder` records the topological order properties were
    // resolved in (useful for `cajeta info --properties` output and
    // for the lockfile). Fixed built-ins (`details.*`, `flavor`,
    // `profile`, `target`, `workspace.root`, `cajeta.version`) are
    // materialized into `values` eagerly during `resolveProperties`.
    // Open-namespace built-ins (`env.*`, `artifact.*`) resolve lazily
    // via `lookup()`.
    struct ResolvedProperties {
        std::map<std::string, std::string> values;
        std::vector<std::string> resolutionOrder;

        // Lookup that handles both eagerly-resolved entries in
        // `values` and lazy built-ins. Returns nullopt for genuinely
        // undefined names.
        std::optional<std::string> lookup(const std::string& name) const;
    };

    // Resolve all properties in a manifest with the given overrides.
    // Returns an error on:
    //   - cyclic property reference
    //   - missing property reference (only when actually evaluated;
    //     unreferenced missing properties are silent until consumed)
    //   - user property name collides with a built-in
    //   - shape error (env.NAME built-in needs the env var)
    llvm::Expected<ResolvedProperties> resolveProperties(
        const Manifest& manifest,
        const PropertyOverrides& overrides = {});

    // Substitute every ${NAME} reference in `s` using the resolved
    // property table. Each `${NAME}` lookup that's not in `props`
    // produces an error citing the reference and a where-context.
    llvm::Expected<std::string> substitute(
        const std::string& s,
        const ResolvedProperties& props,
        const std::string& whereContext = "<unknown>");

    // Helper to populate PropertyOverrides::env from the process
    // environment. Looks for variables prefixed with CAJETA_PROPERTY_
    // and converts the suffix back to dotted lowercase form
    // (CAJETA_PROPERTY_STACK_VERSION → "stack-version"; underscores
    // in source become hyphens in the property name).
    void loadEnvOverrides(PropertyOverrides& dst);

    // Parse one CLI override token (the value passed to `-P` or
    // `--property=`). Format is NAME=VALUE; everything after the
    // first `=` is the value (the value itself may contain `=`).
    // Returns nullopt with a message on malformed input.
    llvm::Expected<std::pair<std::string, std::string>> parseCliOverride(
        const std::string& token);

} // namespace cajeta::buildtool
