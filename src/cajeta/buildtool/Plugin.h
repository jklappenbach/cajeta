// Cajeta build-tool plugin model: a plugin is a `.cja` package exporting named
// actions to tasks. Data only — subprocess spawning and action dispatch live
// in PluginRuntime.

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

    // One entry from the consumer's `plugins` block: a version constraint plus
    // an opaque per-plugin configuration object.
    struct PluginSpec {
        std::string name;            // namespace (e.g. "cajeta.coverage")
        std::string versionConstraint;
        // Raw; forwarded to the plugin's actions as default parameter values.
        llvm::json::Object configRaw;
    };

    // One resolved plugin: the artifact, where it came from, and the capability
    // set it declared — already validated against the consumer's allowlist.
    struct ResolvedPlugin {
        std::string name;
        std::string version;
        std::string resolvedFromRepo;
        std::string artifactPath;
        std::string sha256;
        std::set<std::string> capabilities;
        // `details.plugin.main`: a static no-arg method reading the protocol
        // request from stdin. With no `binary` the runtime compiles the .cja to
        // a cached binary on first use — the default distribution model.
        std::string mainEntry;
        // The sidecar manifest's raw bytes, written through when auto-homed.
        std::string manifestJson;
        // The plugin's own resolved dependencies — its compile classpath.
        std::vector<std::string> depArtifacts;
        // `details.plugin.binary` as an absolute path; empty for a pure-source
        // plugin, which parses but cannot be dispatched at runtime.
        std::string binaryPath;
        // `details.plugin.actions`: the namespaced names this plugin advertises,
        // driving dispatch in PluginAction / ActionRegistry.
        std::vector<std::string> actionNames;
        // `details.plugin.entries`, action name → entry symbol: forwarded to the
        // plugin binary, and otherwise uninterpreted round-trip data in v1.
        std::map<std::string, std::string> entries;
    };

    // Parses `plugins` from the consumer manifest; each value must be an object
    // with a `version` string and an optional `config` object.
    llvm::Expected<std::vector<PluginSpec>> parsePlugins(const Manifest& m);

    // Parses `settings.plugins-allowed-capabilities`, empty when the field is
    // absent; the caller then applies the defaults below.
    llvm::Expected<std::vector<std::string>>
    parsePluginsAllowedCapabilities(const Manifest& m);

    // Fetches each declared plugin through the priority-ordered repos, reads its
    // sidecar manifest and checks its `settings.capabilities` against
    // `allowedCapabilities`. Errors on no version, a bad capability, or an id clash.
    llvm::Expected<std::vector<ResolvedPlugin>> resolvePlugins(
        const std::vector<PluginSpec>& specs,
        const std::vector<RepositoryPtr>& repos,
        const std::vector<std::string>& allowedCapabilities,
        ArtifactCache& cache);

    // The default allowlist outside the `cajeta.*` namespace: `["filesystem"]`.
    std::vector<std::string> defaultUserPluginAllowlist();

    // The wider default for `cajeta.*` plugins: filesystem, process, network.
    std::vector<std::string> defaultFirstPartyPluginAllowlist();

    // True iff `pluginName` is first-party, i.e. in the `cajeta.*` namespace.
    bool isFirstPartyPluginName(const std::string& pluginName);

} // namespace cajeta::buildtool
