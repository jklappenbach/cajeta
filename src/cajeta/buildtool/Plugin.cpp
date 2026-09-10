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
        if (m.pluginsRaw.empty()) return out;
        for (const auto& kv : m.pluginsRaw) {
            PluginSpec p;
            p.name = kv.first.str();
            // String shorthand: `"acme.thing": "1.0.*"` means `{ "version": "1.0.*" }`,
            // the form `cajeta init` archetypes ship, so it must stay accepted.
            if (auto shorthand = kv.second.getAsString()) {
                p.versionConstraint = shorthand->str();
                out.push_back(std::move(p));
                continue;
            }
            const auto* obj = kv.second.getAsObject();
            if (!obj) {
                return err("plugins." + p.name +
                           ": value must be a version string, or an object "
                           "with 'version' (and optional 'config')");
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

        // One resolved plugin pick: the chosen version plus the repo, artifact path
        // and sidecar JSON carrying it. Empty version when nothing satisfied.
        struct PluginPick {
            std::string version;
            std::string resolvedFromRepo;
            std::string artifactPath;
            std::string sha256;
            std::string manifestJson;
        };

    } // namespace

} // namespace cajeta::buildtool

#include "cajeta/buildtool/Resolver.h"

namespace cajeta::buildtool {

    namespace {

        struct HighestSatisfying {
            std::string version;
        };

        // Highest candidate version satisfying `constraint`; empty when none does.
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

            auto pluginManifest = loadManifestString(
                manifestJson, spec.name + "@" + r.version);
            if (!pluginManifest) return pluginManifest.takeError();
            auto pluginCaps = capabilitiesFromManifest(*pluginManifest);

            for (const auto& c : pluginCaps) {
                if (!allowed.count(c)) {
                    return err("plugins." + spec.name +
                               ": declares capability '" + c +
                               "' which is not in the consumer's "
                               "plugins-allowed-capabilities allowlist");
                }
            }
            r.capabilities = std::move(pluginCaps);

            // `details.plugin` is absent for sidecars predating the plugin protocol;
            // such plugins parse but cannot be invoked.
            const auto& pluginObj = pluginManifest->details.pluginRaw;
            if (auto s = pluginObj.getString("main")) {
                r.mainEntry = s->str();
            }
            r.manifestJson = manifestJson;
            // A plugin flattens its transitive closure into its own manifest (v1),
            // so its classpath comes from the raw sidecar's settings.dependencies.
            {
                auto parsedSidecar = llvm::json::parse(manifestJson);
                if (parsedSidecar) {
                    if (const auto* rootObj = parsedSidecar->getAsObject()) {
                        const auto* settingsObj = rootObj->getObject("settings");
                        const llvm::json::Object* depsObj =
                            settingsObj ? settingsObj->getObject("dependencies")
                                        : nullptr;
                        if (depsObj) {
                            for (const auto& dep : *depsObj) {
                                std::string depName = dep.first.str();
                                auto depConstraint = dep.second.getAsString();
                                if (!depConstraint) continue;
                                bool depFound = false;
                                for (const auto& repo : repos) {
                                    auto dv = repo->listVersions(depName);
                                    if (!dv) return dv.takeError();
                                    std::string v2 = highestSat(
                                        *dv, depConstraint->str());
                                    if (v2.empty()) continue;
                                    auto dp = repo->fetch(depName, v2);
                                    if (!dp) return dp.takeError();
                                    auto dc = cache.insert(*dp);
                                    if (!dc) return dc.takeError();
                                    r.depArtifacts.push_back(*dc);
                                    depFound = true;
                                    break;
                                }
                                if (!depFound) {
                                    return err("plugins." + spec.name +
                                               ": dependency '" + depName +
                                               "' " + depConstraint->str() +
                                               " not found in any repository");
                                }
                            }
                        }
                    }
                } else {
                    llvm::consumeError(parsedSidecar.takeError());
                }
            }
            if (auto s = pluginObj.getString("binary")) {
                // Relative binaries resolve against the artifact's own directory,
                // where a binary shipped in the published archive lands.
                std::string bin = s->str();
                if (!bin.empty() && bin[0] != '/') {
                    auto slash = r.artifactPath.find_last_of('/');
                    std::string parent = (slash == std::string::npos)
                                             ? "."
                                             : r.artifactPath.substr(0, slash);
                    bin = parent + "/" + bin;
                }
                r.binaryPath = std::move(bin);
            }
            if (const auto* arr = pluginObj.getArray("actions")) {
                for (const auto& v : *arr) {
                    if (auto an = v.getAsString()) {
                        r.actionNames.push_back(an->str());
                    }
                }
            }
            if (const auto* eobj = pluginObj.getObject("entries")) {
                for (const auto& kv : *eobj) {
                    if (auto sv = kv.second.getAsString()) {
                        r.entries[kv.first.str()] = sv->str();
                    }
                }
            }

            alreadyResolved[spec.name] = r.version;
            out.push_back(std::move(r));
        }
        return out;
    }

} // namespace cajeta::buildtool
