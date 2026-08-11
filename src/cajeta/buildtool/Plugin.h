// Cajeta build-tool plugin model — Phase 7b.
//
// A plugin is a `.cja` package whose purpose is to export named
// actions consumable from tasks. The package's manifest declares
// the action namespace (typically the package name, e.g.
// `cajeta.coverage`) plus the capabilities the plugin requires
// at runtime.
//
// This header models:
//   - The `plugins` block of a consumer's manifest (which plugins
//     they import + per-plugin config).
//   - `settings.plugins-allowed-capabilities` (the consumer's
//     allowlist of capabilities any plugin is permitted to declare).
//   - The typed resolution result: one entry per plugin with the
//     resolved version, repo, checksum, and the plugin's own
//     declared capability set (validated against the allowlist).
//
// Subprocess spawning + action dispatch lives in PluginRuntime
// (separate slice) — this header is just data.

#pragma once

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Repository.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // One entry from the consumer's `plugins` block.
    //   "plugins": {
    //       "cajeta.coverage": {
    //           "version": "1.0.*",
    //           "config": { ... }
    //       }
    //   }
    struct PluginSpec {
        std::string name;            // namespace (e.g. "cajeta.coverage")
        std::string versionConstraint;
        // Plugin-specific configuration object. Kept raw — each
        // plugin documents the shape it accepts. The build tool
        // forwards this to the plugin's actions as default param
        // values.
        llvm::json::Object configRaw;
    };

    // One resolved plugin: the artifact, who supplied it, and the
    // capability set it declared. Mirrors the schema slot reserved
    // in the lockfile's `plugins` array (Phase 2).
    struct ResolvedPlugin {
        std::string name;
        std::string version;
        std::string resolvedFromRepo;
        std::string artifactPath;
        std::string sha256;
        // The plugin's own declared `settings.capabilities`. Already
        // intersected against the consumer's allowlist by the time
        // a plugin appears here — anything outside the allowlist
        // would have caused resolvePlugins() to error.
        std::set<std::string> capabilities;
        // The sidecar's `details.plugin.main` — a static no-arg method
        // (`pkg.Class.method`) that reads the protocol request from stdin.
        // When set and `binary` is absent, the runtime compiles the .cja
        // into a cached binary on first use (auto-homed in the local olla
        // store) — running-the-cja is the DEFAULT distribution model;
        // an explicit `binary` remains the override.
        std::string mainEntry;
        // The sidecar manifest's raw bytes — written through when the
        // artifact is auto-homed into the local store.
        std::string manifestJson;
        // Artifacts of the plugin's own (flat, v1) `settings.dependencies`,
        // resolved beside the plugin — the compile classpath.
        std::vector<std::string> depArtifacts;
        // The plugin's `details.plugin.binary` field, resolved to an
        // absolute path. Empty until the plugin lands a binary —
        // pure-source plugin packages parse successfully but can't
        // be dispatched at runtime.
        std::string binaryPath;
        // The plugin's `details.plugin.actions` list — the namespaced
        // action names this plugin advertises. Drives action-name
        // dispatch in PluginAction / ActionRegistry. Empty when the
        // sidecar's plugin block is absent.
        std::vector<std::string> actionNames;
        // `details.plugin.entries` map: action-name → entry symbol.
        // The plugin runtime forwards the entry path to the plugin
        // binary so it knows which entry to call. v1 makes this
        // round-trip data (the build tool doesn't interpret it);
        // future in-process dispatch resolves symbols against it.
        std::map<std::string, std::string> entries;
    };

    // Parse `plugins` from the consumer manifest. Each value must be
    // an object with a `version` string + optional `config` object.
    llvm::Expected<std::vector<PluginSpec>> parsePlugins(const Manifest& m);

    // Parse `settings.plugins-allowed-capabilities`. Returns an
    // empty vector when the field is absent — callers default to
    // `["filesystem"]` per spec for non-first-party plugins.
    llvm::Expected<std::vector<std::string>>
    parsePluginsAllowedCapabilities(const Manifest& m);

    // Fetch each declared plugin via the priority-ordered repos,
    // read its sidecar manifest, validate it's plugin-shaped
    // (Phase 7b minimum: any package — the spec calls for a
    // `plugin.id` field; we accept its absence in v1 and use the
    // package's `details.name`), then check the plugin's declared
    // `settings.capabilities` against `allowedCapabilities`.
    //
    // First-party plugins (the `cajeta.*` namespace) get a less
    // restrictive default allowlist — they're shipped with the
    // toolchain and trusted. The full set: filesystem, process,
    // network. User plugins always face the explicit allowlist.
    //
    // Errors:
    //   - Plugin can't be resolved to a satisfying version.
    //   - Plugin declares a capability not in the allowlist.
    //   - Multiple plugins claim the same id (collision).
    llvm::Expected<std::vector<ResolvedPlugin>> resolvePlugins(
        const std::vector<PluginSpec>& specs,
        const std::vector<RepositoryPtr>& repos,
        const std::vector<std::string>& allowedCapabilities,
        ArtifactCache& cache);

    // Returns the default allowlist for plugins NOT in the
    // `cajeta.*` namespace. Per spec: `["filesystem"]`. Exposed
    // for tests + callers that want to apply the default when the
    // consumer didn't declare an explicit allowlist.
    std::vector<std::string> defaultUserPluginAllowlist();

    // Returns the default allowlist for `cajeta.*` first-party
    // plugins. Per spec: less restrictive — includes filesystem,
    // process, and network.
    std::vector<std::string> defaultFirstPartyPluginAllowlist();

    // Predicate: does `pluginName` live in the `cajeta.*` namespace?
    // First-party plugins ship with the toolchain and earn the
    // wider default allowlist.
    bool isFirstPartyPluginName(const std::string& pluginName);

} // namespace cajeta::buildtool
