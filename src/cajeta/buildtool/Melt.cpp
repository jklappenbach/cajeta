#include "cajeta/buildtool/Melt.h"

#include <llvm/Support/Error.h>

#include <set>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // Fields the spec marks as exportable inside the `melt` block.
        // Anything else is rejected — matches the "inert inherits,
        // active doesn't" rule from BuildTool.md "What a melt exports".
        const std::set<std::string>& allowedMeltFields() {
            static const std::set<std::string> kFields = {
                "dependencies", "properties", "actions",
                "repositories", "melts",
            };
            return kFields;
        }

    } // namespace

    llvm::Expected<MeltImport> parseMeltImport(const std::string& s) {
        auto at = s.rfind('@');
        if (at == std::string::npos || at == 0 || at + 1 == s.size()) {
            return err("melt import '" + s +
                       "' must have the form name@version");
        }
        MeltImport out;
        out.name    = s.substr(0, at);
        out.version = s.substr(at + 1);
        // Both halves non-empty — sanity check.
        if (out.name.empty() || out.version.empty()) {
            return err("melt import '" + s +
                       "': name and version must both be non-empty");
        }
        return out;
    }

    llvm::Expected<std::vector<MeltImport>> parseSettingsMelts(
        const Manifest& m) {
        std::vector<MeltImport> out;
        const auto* arr = m.settingsRaw.getArray("melts");
        if (!arr) return out;

        for (size_t i = 0; i < arr->size(); ++i) {
            auto s = (*arr)[i].getAsString();
            if (!s) {
                return err("settings.melts[" + std::to_string(i) +
                           "] must be a string 'name@version'");
            }
            auto imp = parseMeltImport(s->str());
            if (!imp) {
                return err("settings.melts[" + std::to_string(i) +
                           "]: " + llvm::toString(imp.takeError()));
            }
            out.push_back(std::move(*imp));
        }
        return out;
    }

    namespace {

        // Parse `melt.repositories` array. Mirrors the
        // `parseRepositories` logic from Dependency.cpp but operates
        // on a sub-object rather than `settings.repositories`.
        // Reused with the cross-checking validation that the public
        // parseRepositories already does.
        llvm::Expected<std::vector<RepositorySpec>> parseMeltRepos(
            const llvm::json::Array& arr) {
            std::vector<RepositorySpec> out;
            out.reserve(arr.size());
            for (size_t i = 0; i < arr.size(); ++i) {
                const auto* obj = arr[i].getAsObject();
                if (!obj) {
                    return err("melt.repositories[" + std::to_string(i) +
                               "] must be an object");
                }
                RepositorySpec r;
                auto name = obj->getString("name");
                if (!name) {
                    return err("melt.repositories[" + std::to_string(i) +
                               "] missing required 'name'");
                }
                r.name = name->str();
                if (auto t = obj->getString("type")) r.type = t->str();
                else if (obj->getString("path")) r.type = "filesystem";
                else if (obj->getString("url"))  r.type = "http";
                else {
                    return err("melt.repositories." + r.name +
                               ": cannot infer 'type'");
                }
                if (auto v = obj->getString("path")) r.path = v->str();
                if (auto v = obj->getString("url"))  r.url  = v->str();
                if (auto v = obj->getInteger("priority")) {
                    r.priority = static_cast<int>(*v);
                }
                if (auto v = obj->getString("ref"))    r.gitRef = v->str();
                if (auto v = obj->getString("tag"))    r.gitRef = v->str();
                if (auto v = obj->getString("branch")) r.gitRef = v->str();
                if (auto v = obj->getString("rev"))    r.gitRef = v->str();
                if (auto v = obj->getString("subdir")) r.gitSubdir = v->str();
                out.push_back(std::move(r));
            }
            return out;
        }

    } // namespace

    namespace {

        // Expand one melt import: fetch its artifact through the
        // priority-ordered repos, read its manifest sidecar, verify
        // it really is a melt-shaped manifest, parse its typed
        // `melt` block, then recurse post-order into its
        // `melt.melts`. The `visiting` set is the stack of melts
        // currently being expanded — used for cycle detection.
        //
        // Aggregates contributions in declaration order into `out`.
        // Post-order means a melt that imports B sees B's
        // contributions FIRST, then layers its own on top — so the
        // outer melt's constraints win on conflict (matching the
        // spec's "later overrides earlier" semantics).
        llvm::Error expandMelt(
            const MeltImport& imp,
            const std::vector<RepositoryPtr>& repos,
            ArtifactCache& cache,
            std::set<std::string>& visiting,
            std::set<std::string>& alreadyResolved,
            std::vector<std::string>& cyclePath,
            MeltResolution& out) {

            std::string key = imp.name + "@" + imp.version;
            if (visiting.count(imp.name)) {
                cyclePath.push_back(imp.name);
                std::string chain;
                for (size_t i = 0; i < cyclePath.size(); ++i) {
                    if (i) chain += " -> ";
                    chain += cyclePath[i];
                }
                return err("melt cycle detected: " + chain);
            }
            // Same exact pin already resolved → no-op (a melt that
            // appears twice in the resolved graph just shares its
            // contributions; the constraint-table merge is idempotent).
            if (alreadyResolved.count(key)) {
                return llvm::Error::success();
            }

            // Walk the repository list to find the pinned version.
            MeltResolution::Resolved resolved;
            resolved.name = imp.name;
            resolved.version = imp.version;
            std::string manifestJson;
            bool found = false;
            for (const auto& repo : repos) {
                auto versions = repo->listVersions(imp.name);
                if (!versions) return versions.takeError();
                bool has = false;
                for (const auto& v : *versions) {
                    if (v == imp.version) { has = true; break; }
                }
                if (!has) continue;

                auto path = repo->fetch(imp.name, imp.version);
                if (!path) return path.takeError();
                auto cached = cache.insert(*path);
                if (!cached) return cached.takeError();
                auto sidecar = repo->fetchManifestJson(imp.name, imp.version);
                if (!sidecar) return sidecar.takeError();
                if (!sidecar->has_value()) {
                    return err("melt '" + key + "': repository '" +
                               repo->name() + "' has the artifact "
                               "but no manifest sidecar — melts must "
                               "publish their cajeta.json as a sidecar");
                }
                resolved.resolvedFromRepo = repo->name();
                resolved.artifactPath = *cached;
                resolved.sha256 = ArtifactCache::sha256OfFile(*cached);
                manifestJson = **sidecar;
                found = true;
                break;
            }
            if (!found) {
                std::string repoList;
                for (const auto& r : repos) {
                    if (!repoList.empty()) repoList += ", ";
                    repoList += r->name();
                }
                return err("melt '" + key + "' not found in any "
                           "repository (tried: " +
                           (repoList.empty() ? "<none>" : repoList) + ")");
            }

            auto meltManifest = loadManifestString(manifestJson, key);
            if (!meltManifest) return meltManifest.takeError();
            if (!meltManifest->hasMelt) {
                return err("'" + key + "' resolved successfully but its "
                           "manifest declares no 'melt' block — only "
                           "melt-shaped packages can be imported via "
                           "settings.melts");
            }
            auto typed = parseMelt(*meltManifest);
            if (!typed) return typed.takeError();

            // Stash transitive list before recursing — needed for the
            // lockfile entry whether or not recursion succeeds.
            resolved.transitiveMelts = typed->melts;

            // Recurse post-order: process this melt's `melts` first,
            // then apply this melt's own contributions.
            visiting.insert(imp.name);
            cyclePath.push_back(imp.name);
            for (const auto& child : typed->melts) {
                if (auto e = expandMelt(
                        child, repos, cache, visiting,
                        alreadyResolved, cyclePath, out)) {
                    return e;
                }
            }
            visiting.erase(imp.name);
            cyclePath.pop_back();

            // Apply this melt's contributions (later writes win).
            for (const auto& [name, constraint] : typed->dependencies) {
                out.depConstraints[name] = constraint;
                out.depProvidedBy[name] = key;
            }
            for (const auto& [name, value] : typed->properties) {
                out.properties[name] = value;
                out.propertyProvidedBy[name] = key;
            }
            for (const auto& kv : typed->actionsRaw) {
                out.actionsRaw[kv.first] = kv.second;
            }
            for (const auto& r : typed->repositories) {
                out.repositories.push_back(r);
            }

            alreadyResolved.insert(key);
            out.resolvedMelts.push_back(std::move(resolved));
            return llvm::Error::success();
        }

    } // namespace

    llvm::Expected<MeltResolution> resolveMelts(
        const Manifest& m,
        const std::vector<RepositoryPtr>& repos,
        ArtifactCache& cache) {
        MeltResolution out;
        auto imports = parseSettingsMelts(m);
        if (!imports) return imports.takeError();
        if (imports->empty()) return out;

        std::set<std::string> visiting;
        std::set<std::string> alreadyResolved;
        std::vector<std::string> cyclePath;
        for (const auto& imp : *imports) {
            if (auto e = expandMelt(
                    imp, repos, cache, visiting,
                    alreadyResolved, cyclePath, out)) {
                return std::move(e);
            }
        }
        return out;
    }

    llvm::Error applyMeltLookups(
        std::vector<DependencySpec>& deps,
        const MeltResolution& melts,
        std::map<std::string, std::string>& providedByOut,
        std::vector<std::string>& warningsOut) {
        for (auto& dep : deps) {
            auto it = melts.depConstraints.find(dep.name);
            if (dep.versionConstraint == "*") {
                if (it == melts.depConstraints.end()) {
                    return err("dependency '" + dep.name +
                               "' declared as '*' but no imported melt "
                               "curates it — supply an explicit version "
                               "or import a melt that pins it");
                }
                dep.versionConstraint = it->second;
                auto byIt = melts.depProvidedBy.find(dep.name);
                if (byIt != melts.depProvidedBy.end()) {
                    providedByOut[dep.name] = byIt->second;
                }
                continue;
            }
            // Explicit version: keep the consumer's pin, but surface
            // a divergence warning when a melt would have curated a
            // different version. The operator sees in their build
            // output exactly which curated guidance they overrode.
            if (it != melts.depConstraints.end() &&
                it->second != dep.versionConstraint) {
                auto byIt = melts.depProvidedBy.find(dep.name);
                std::string source = (byIt != melts.depProvidedBy.end())
                                         ? byIt->second
                                         : std::string("<melt>");
                warningsOut.push_back(
                    "dep '" + dep.name + "': consumer pins '" +
                    dep.versionConstraint + "' but melt '" + source +
                    "' curates '" + it->second +
                    "' — using consumer's");
            }
        }
        return llvm::Error::success();
    }

    llvm::Expected<Melt> parseMelt(const Manifest& m) {
        Melt out;
        if (!m.hasMelt) return out;

        // Unknown fields are rejected so consumers get a clear error
        // if they (e.g.) tried to slip `plugins` or `capabilities`
        // into a melt.
        for (const auto& kv : m.meltRaw) {
            if (!allowedMeltFields().count(kv.first.str())) {
                return err("'melt." + kv.first.str() +
                           "' is not an exportable melt field "
                           "(allowed: dependencies, properties, "
                           "actions, repositories, melts)");
            }
        }

        if (const auto* deps = m.meltRaw.getObject("dependencies")) {
            for (const auto& kv : *deps) {
                auto cs = kv.second.getAsString();
                if (!cs) {
                    return err("melt.dependencies." + kv.first.str() +
                               ": value must be a version constraint "
                               "string");
                }
                out.dependencies[kv.first.str()] = cs->str();
            }
        }

        if (const auto* props = m.meltRaw.getObject("properties")) {
            for (const auto& kv : *props) {
                auto ps = kv.second.getAsString();
                if (!ps) {
                    return err("melt.properties." + kv.first.str() +
                               ": value must be a string (properties "
                               "are inert text substituted at use site)");
                }
                out.properties[kv.first.str()] = ps->str();
            }
        }

        if (const auto* acts = m.meltRaw.getObject("actions")) {
            out.actionsRaw = *acts;
        }

        if (const auto* repos = m.meltRaw.getArray("repositories")) {
            auto parsed = parseMeltRepos(*repos);
            if (!parsed) return parsed.takeError();
            out.repositories = std::move(*parsed);
        }

        if (const auto* melts = m.meltRaw.getArray("melts")) {
            for (size_t i = 0; i < melts->size(); ++i) {
                auto s = (*melts)[i].getAsString();
                if (!s) {
                    return err("melt.melts[" + std::to_string(i) +
                               "] must be a string 'name@version'");
                }
                auto imp = parseMeltImport(s->str());
                if (!imp) {
                    return err("melt.melts[" + std::to_string(i) +
                               "]: " + llvm::toString(imp.takeError()));
                }
                out.melts.push_back(std::move(*imp));
            }
        }

        return out;
    }

} // namespace cajeta::buildtool
