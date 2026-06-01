#include "cajeta/buildtool/Plugin.h"

#include <llvm/Support/Error.h>

#include <algorithm>
#include <set>
#include <unordered_map>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        std::set<std::string> capabilitiesFromManifest(const Manifest& m) {
            std::set<std::string> out;
            const auto* caps = m.settingsRaw.getArray("capabilities");
            if (!caps) return out;
            for (const auto& v : *caps) {
                if (auto s = v.getAsString()) out.insert(s->str());
            }
            return out;
        }

    } // namespace

    bool isFirstPartyPluginName(const std::string& pluginName) {
        // The `cajeta.*` namespace (plus the literal "cajeta").
        if (pluginName == "cajeta") return true;
        return pluginName.size() > 7 &&
               pluginName.compare(0, 7, "cajeta.") == 0;
    }

    std::vector<std::string> defaultUserPluginAllowlist() {
        return {"filesystem"};
    }

    std::vector<std::string> defaultFirstPartyPluginAllowlist() {
        return {"filesystem", "process", "network"};
    }

    llvm::Expected<std::vector<PluginSpec>> parsePlugins(const Manifest& m) {
        std::vector<PluginSpec> out;
        // `plugins` is the top-level block, not a sub-block of settings.
        if (m.pluginsRaw.empty()) return out;
        for (const auto& kv : m.pluginsRaw) {
            PluginSpec p;
            p.name = kv.first.str();
            const auto* obj = kv.second.getAsObject();
            if (!obj) {
                return err("plugins." + p.name +
                           ": value must be an object with "
                           "'version' (and optional 'config')");
            }
            auto v = obj->getString("version");
            if (!v) {
                return err("plugins." + p.name +
                           ": missing required 'version'");
            }
            p.versionConstraint = v->str();
            if (const auto* cfg = obj->getObject("config")) {
                p.configRaw = *cfg;
            }
            out.push_back(std::move(p));
        }
        return out;
    }

    llvm::Expected<std::vector<std::string>>
    parsePluginsAllowedCapabilities(const Manifest& m) {
        std::vector<std::string> out;
        const auto* arr = m.settingsRaw.getArray(
            "plugins-allowed-capabilities");
        if (!arr) return out;
        for (size_t i = 0; i < arr->size(); ++i) {
            auto s = (*arr)[i].getAsString();
            if (!s) {
                return err("settings.plugins-allowed-capabilities[" +
                           std::to_string(i) + "] must be a string");
            }
            out.push_back(s->str());
        }
        return out;
    }

    namespace {

        // Pick the highest version of `name` satisfying `constraint`.
        // Returns the (version, repo, artifactPath, manifestJson) tuple
        // on success; empty version when no repo carries a satisfying
        // version. Mirrors the dep resolver's resolveOne but
        // single-shot for the plugin layer.
        struct PluginPick {
            std::string version;
            std::string resolvedFromRepo;
            std::string artifactPath;
            std::string sha256;
            std::string manifestJson;
        };

        // Forward decl: the resolver exposes these. Defined in
        // Resolver.cpp.
        // (Reuse rather than re-implement so plugin resolution stays
        // honest about the same version-constraint semantics deps use.)
    } // namespace

    // Implemented inline (instead of pulling Resolver.cpp internals)
    // to keep the dependency edge minimal. Uses highestSatisfying
    // semantics from the public Resolver.h surface.

} // namespace cajeta::buildtool

#include "cajeta/buildtool/Resolver.h"

namespace cajeta::buildtool {

    namespace {

        struct HighestSatisfying {
            std::string version;
        };

        std::string highestSat(
            const std::vector<std::string>& candidates,
            const std::string& constraint) {
            std::string best;
            for (const auto& v : candidates) {
                if (!versionSatisfies(v, constraint)) continue;
                if (best.empty() || compareVersions(v, best) > 0) {
                    best = v;
                }
            }
            return best;
        }

    } // namespace

    llvm::Expected<std::vector<ResolvedPlugin>> resolvePlugins(
        const std::vector<PluginSpec>& specs,
        const std::vector<RepositoryPtr>& repos,
        const std::vector<std::string>& allowedCapabilities,
        ArtifactCache& cache) {
        std::vector<ResolvedPlugin> out;
        std::unordered_map<std::string, std::string> alreadyResolved;

        for (const auto& spec : specs) {
            if (alreadyResolved.count(spec.name)) {
                return err("plugins." + spec.name +
                           ": declared twice in the manifest");
            }

            // Choose the per-plugin allowlist. First-party plugins
            // get the wider default; user plugins use the consumer's
            // explicit set (falling back to defaultUserPluginAllowlist
            // when none was declared).
            std::set<std::string> allowed;
            if (allowedCapabilities.empty()) {
                auto base = isFirstPartyPluginName(spec.name)
                                ? defaultFirstPartyPluginAllowlist()
                                : defaultUserPluginAllowlist();
                allowed.insert(base.begin(), base.end());
            } else {
                allowed.insert(allowedCapabilities.begin(),
                               allowedCapabilities.end());
            }

            // Walk priority-ordered repos for the highest satisfying
            // version. First repo with a satisfying version wins.
            ResolvedPlugin r;
            r.name = spec.name;
            std::string manifestJson;
            bool found = false;
            for (const auto& repo : repos) {
                auto versions = repo->listVersions(spec.name);
                if (!versions) return versions.takeError();
                std::string v = highestSat(*versions,
                                           spec.versionConstraint);
                if (v.empty()) continue;
                auto path = repo->fetch(spec.name, v);
                if (!path) return path.takeError();
                auto cached = cache.insert(*path);
                if (!cached) return cached.takeError();
                auto sidecar = repo->fetchManifestJson(spec.name, v);
                if (!sidecar) return sidecar.takeError();
                if (!sidecar->has_value()) {
                    return err("plugins." + spec.name +
                               ": repository '" + repo->name() +
                               "' has the artifact but no manifest "
                               "sidecar — plugins must publish their "
                               "cajeta.json as a sidecar so the "
                               "capability check can run");
                }
                r.version = v;
                r.resolvedFromRepo = repo->name();
                r.artifactPath = *cached;
                r.sha256 = ArtifactCache::sha256OfFile(*cached);
                manifestJson = **sidecar;
                found = true;
                break;
            }
            if (!found) {
                std::string repoList;
                for (const auto& rp : repos) {
                    if (!repoList.empty()) repoList += ", ";
                    repoList += rp->name();
                }
                return err("plugins." + spec.name + " " +
                           spec.versionConstraint +
                           ": no repository has a satisfying version "
                           "(tried: " +
                           (repoList.empty() ? "<none>" : repoList) + ")");
            }

            // Parse the plugin's sidecar to read its declared caps.
            auto pluginManifest = loadManifestString(
                manifestJson, spec.name + "@" + r.version);
            if (!pluginManifest) return pluginManifest.takeError();
            auto pluginCaps = capabilitiesFromManifest(*pluginManifest);

            // Enforce: every declared cap must be in the allowlist.
            for (const auto& c : pluginCaps) {
                if (!allowed.count(c)) {
                    return err("plugins." + spec.name +
                               ": declares capability '" + c +
                               "' which is not in the consumer's "
                               "plugins-allowed-capabilities allowlist");
                }
            }
            r.capabilities = std::move(pluginCaps);
            alreadyResolved[spec.name] = r.version;
            out.push_back(std::move(r));
        }
        return out;
    }

} // namespace cajeta::buildtool
