#include "cajeta/buildtool/Upgrader.h"

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/ManifestEditor.h"
#include "cajeta/buildtool/Melt.h"
#include "cajeta/buildtool/Repository.h"
#include "cajeta/buildtool/Resolver.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // The constraint string for `name` from the manifest's `settings.dependencies`,
        // or nullopt when undeclared. Accepts both the string and { "version" } shapes.
        std::optional<std::string> manifestConstraintFor(
            const Manifest& m, const std::string& name) {
            const auto* deps = m.settingsRaw.getObject("dependencies");
            if (!deps) return std::nullopt;
            const llvm::json::Value* v = deps->get(name);
            if (!v) return std::nullopt;
            if (auto s = v->getAsString()) {
                return s->str();
            }
            if (const auto* obj = v->getAsObject()) {
                if (const auto* sv = obj->get("version")) {
                    if (auto s = sv->getAsString()) return s->str();
                }
            }
            return std::nullopt;
        }

        // `settings.capabilities` from a sidecar manifest's raw JSON. Empty when the sidecar
        // is missing, unparseable or declares none, so a sidecar-less repo flags nothing.
        std::set<std::string> capabilitiesFromSidecar(
            const std::string& json) {
            std::set<std::string> out;
            if (json.empty()) return out;
            auto parsed = llvm::json::parse(json);
            if (!parsed) {
                llvm::consumeError(parsed.takeError());
                return out;
            }
            const auto* root = parsed->getAsObject();
            if (!root) return out;
            const auto* settings = root->getObject("settings");
            if (!settings) return out;
            const auto* caps = settings->getArray("capabilities");
            if (!caps) return out;
            for (const auto& v : *caps) {
                if (auto s = v.getAsString()) out.insert(s->str());
            }
            return out;
        }

        CapabilityDelta diffCapabilities(
            const std::set<std::string>& oldSet,
            const std::set<std::string>& newSet) {
            CapabilityDelta d;
            for (const auto& c : newSet) {
                if (!oldSet.count(c)) d.added.push_back(c);
            }
            for (const auto& c : oldSet) {
                if (!newSet.count(c)) d.removed.push_back(c);
            }
            std::sort(d.added.begin(), d.added.end());
            std::sort(d.removed.begin(), d.removed.end());
            return d;
        }

        // Highest version of `name` across all repos, with the first repo (in priority
        // order) that carries it. Empty version when no repo has any version of `name`.
        struct HighestPick {
            std::string version;
            std::string fromRepo;
        };
        llvm::Expected<HighestPick> highestAcrossRepos(
            const std::string& name,
            const std::vector<RepositoryPtr>& repos) {
            HighestPick best;
            for (const auto& repo : repos) {
                auto versions = repo->listVersions(name);
                if (!versions) return versions.takeError();
                for (const auto& v : *versions) {
                    if (best.version.empty() ||
                        compareVersions(v, best.version) > 0) {
                        best.version = v;
                        best.fromRepo = repo->name();
                    }
                }
            }
            return best;
        }

        // The repo, in priority order, carrying `version` of `name`; empty when none does.
        llvm::Expected<std::string> findRepoCarrying(
            const std::string& name,
            const std::string& version,
            const std::vector<RepositoryPtr>& repos) {
            for (const auto& repo : repos) {
                auto versions = repo->listVersions(name);
                if (!versions) return versions.takeError();
                for (const auto& v : *versions) {
                    if (v == version) return repo->name();
                }
            }
            return std::string{};
        }

        // Raw sidecar `cajeta.json` bytes for `name@version` from whichever repo carries
        // it. Empty when no repo carries the version, or the carrier has no sidecar.
        llvm::Expected<std::string> fetchSidecar(
            const std::string& name,
            const std::string& version,
            const std::vector<RepositoryPtr>& repos) {
            for (const auto& repo : repos) {
                auto versions = repo->listVersions(name);
                if (!versions) return versions.takeError();
                bool carriesIt = false;
                for (const auto& v : *versions) {
                    if (v == version) { carriesIt = true; break; }
                }
                if (!carriesIt) continue;
                auto sidecar = repo->fetchManifestJson(name, version);
                if (!sidecar) return sidecar.takeError();
                if (sidecar->has_value()) return **sidecar;
                return std::string{};  // repo has it, no sidecar
            }
            return std::string{};  // no repo carries it
        }

        // name -> sidecar JSON for a baseline graph; re-fetched because resolveMvs rows
        // do not surface the sidecar bytes (cheap: cache + listVersions are local).
        std::unordered_map<std::string, std::string>
        baselineSidecars(
            const std::vector<ResolvedDependency>& baseline,
            const std::vector<RepositoryPtr>& repos) {
            std::unordered_map<std::string, std::string> out;
            for (const auto& r : baseline) {
                auto bytes = fetchSidecar(r.name, r.version, repos);
                if (!bytes) {
                    llvm::consumeError(bytes.takeError());
                    out[r.name] = "";
                    continue;
                }
                out[r.name] = *bytes;
            }
            return out;
        }

    } // namespace

    bool UpgradePlan::anyChange() const {
        for (const auto& e : entries) if (e.changed) return true;
        return false;
    }

    bool UpgradePlan::anyCapabilityChange() const {
        for (const auto& e : entries) {
            if (e.changed && !e.capDelta.empty()) return true;
        }
        return false;
    }

    llvm::Expected<UpgradePlan> planUpgrade(
        const Manifest& m,
        const std::string& projectRoot,
        const std::vector<std::string>& targetNames,
        const std::map<std::string, std::string>& explicitVersions,
        std::optional<std::string> homeOverride) {

        auto deps = parseDependencies(m);
        if (!deps) return deps.takeError();
        if (deps->empty()) {
            return err("upgrade: settings.dependencies is empty — "
                       "nothing to upgrade");
        }

        auto repoSpecs = parseRepositories(m);
        if (!repoSpecs) return repoSpecs.takeError();
        if (repoSpecs->empty()) {
            return err("upgrade: settings.repositories is empty — "
                       "add at least one repository to resolve from");
        }
        std::string downloadStage =
            (std::filesystem::path(projectRoot) / ".cajeta" / "cache" /
             "downloads").string();
        auto repos = buildRepositories(*repoSpecs, downloadStage);
        if (!repos) return repos.takeError();

        std::vector<std::string> selected;
        if (targetNames.empty()) {
            for (const auto& d : *deps) selected.push_back(d.name);
        } else {
            std::set<std::string> declared;
            for (const auto& d : *deps) declared.insert(d.name);
            for (const auto& n : targetNames) {
                if (!declared.count(n)) {
                    return err("upgrade: '" + n + "' is not declared "
                               "in settings.dependencies");
                }
                selected.push_back(n);
            }
        }

        ArtifactCache cache(projectRoot, homeOverride);
        auto baseline = resolveProjectDependencies(
            m, projectRoot, homeOverride);
        std::unordered_map<std::string, std::string> oldSidecars;
        std::unordered_map<std::string, std::string> oldVersions;
        if (baseline) {
            oldSidecars = baselineSidecars(*baseline, *repos);
            for (const auto& r : *baseline) {
                oldVersions[r.name] = r.version;
            }
        } else {
            // A baseline failure is informational: the user may be upgrading to fix it.
            llvm::consumeError(baseline.takeError());
        }

        UpgradePlan plan;
        for (const auto& name : selected) {
            UpgradeEntry e;
            e.name = name;
            auto cstr = manifestConstraintFor(m, name);
            e.oldConstraint = cstr.value_or("");
            auto ovIt = oldVersions.find(name);
            if (ovIt != oldVersions.end()) e.oldVersion = ovIt->second;

            auto evIt = explicitVersions.find(name);
            if (evIt != explicitVersions.end()) {
                auto carrier = findRepoCarrying(
                    name, evIt->second, *repos);
                if (!carrier) return carrier.takeError();
                if (carrier->empty()) {
                    return err("upgrade: no repository carries '" +
                               name + "@" + evIt->second + "'");
                }
                e.newVersion = evIt->second;
                e.resolvedFromRepo = *carrier;
            } else {
                auto pick = highestAcrossRepos(name, *repos);
                if (!pick) return pick.takeError();
                if (pick->version.empty()) {
                    return err("upgrade: no repository has any "
                               "version of '" + name + "'");
                }
                e.newVersion = pick->version;
                e.resolvedFromRepo = pick->fromRepo;
            }

            e.newConstraint = e.newVersion;  // exact pin
            e.changed = !e.oldVersion.empty() &&
                        e.oldVersion != e.newVersion;
            // An unresolvable baseline makes any valid new version a change.
            if (e.oldVersion.empty()) e.changed = true;

            if (e.changed) {
                auto newSide = fetchSidecar(name, e.newVersion, *repos);
                if (!newSide) return newSide.takeError();
                auto oldSet = capabilitiesFromSidecar(
                    oldSidecars.count(name) ? oldSidecars[name] : "");
                auto newSet = capabilitiesFromSidecar(*newSide);
                e.capDelta = diffCapabilities(oldSet, newSet);
            }
            plan.entries.push_back(std::move(e));
        }
        return plan;
    }

    llvm::Expected<std::string> applyUpgradePlan(
        const std::string& manifestSource,
        const UpgradePlan& plan) {
        std::string src = manifestSource;
        for (const auto& e : plan.entries) {
            if (!e.changed) continue;
            auto rewritten = addDependencyToManifest(
                src, e.name, e.newConstraint);
            if (!rewritten) return rewritten.takeError();
            src = std::move(*rewritten);
        }
        return src;
    }

    // ─── Melt upgrades ──────────────────────────────────────────

    bool MeltUpgradePlan::anyChange() const {
        for (const auto& e : entries) if (e.changed) return true;
        return false;
    }

    namespace {

        // The typed Melt block from a melt artifact's sidecar bytes; an empty Melt
        // when the sidecar is missing or carries no melt block.
        Melt parseMeltSidecar(const std::string& json) {
            Melt out;
            if (json.empty()) return out;
            auto m = loadManifestString(json, "<melt-sidecar>");
            if (!m) {
                llvm::consumeError(m.takeError());
                return out;
            }
            if (!m->hasMelt) return out;
            auto typed = parseMelt(*m);
            if (!typed) {
                llvm::consumeError(typed.takeError());
                return out;
            }
            return std::move(*typed);
        }

        MeltDependencyDelta diffMeltDeps(
            const std::map<std::string, std::string>& oldT,
            const std::map<std::string, std::string>& newT) {
            MeltDependencyDelta d;
            for (const auto& [name, constraint] : newT) {
                auto it = oldT.find(name);
                if (it == oldT.end()) {
                    d.added.emplace_back(name, constraint);
                } else if (it->second != constraint) {
                    d.changed.emplace_back(name, it->second, constraint);
                }
            }
            for (const auto& [name, _] : oldT) {
                if (!newT.count(name)) d.removed.push_back(name);
            }
            std::sort(d.added.begin(), d.added.end());
            std::sort(d.removed.begin(), d.removed.end());
            std::sort(d.changed.begin(), d.changed.end());
            return d;
        }

    } // namespace

    llvm::Expected<MeltUpgradePlan> planMeltUpgrade(
        const Manifest& m,
        const std::string& projectRoot,
        const std::vector<std::string>& targetNames,
        const std::map<std::string, std::string>& explicitVersions,
        std::optional<std::string> homeOverride) {
        auto imports = parseSettingsMelts(m);
        if (!imports) return imports.takeError();
        if (imports->empty()) {
            return err("upgrade --melt: settings.melts is empty — "
                       "no melts declared");
        }

        auto repoSpecs = parseRepositories(m);
        if (!repoSpecs) return repoSpecs.takeError();
        if (repoSpecs->empty()) {
            return err("upgrade --melt: settings.repositories is empty — "
                       "add at least one repository to resolve from");
        }
        std::string downloadStage =
            (std::filesystem::path(projectRoot) / ".cajeta" / "cache" /
             "downloads").string();
        auto repos = buildRepositories(*repoSpecs, downloadStage);
        if (!repos) return repos.takeError();

        std::map<std::string, std::string> currentVersions;
        for (const auto& imp : *imports) {
            currentVersions[imp.name] = imp.version;
        }
        std::vector<std::string> selected;
        if (targetNames.empty()) {
            for (const auto& imp : *imports) selected.push_back(imp.name);
        } else {
            for (const auto& n : targetNames) {
                if (!currentVersions.count(n)) {
                    return err("upgrade --melt: '" + n +
                               "' is not declared in settings.melts");
                }
                selected.push_back(n);
            }
        }

        (void)homeOverride;  // matched signature with planUpgrade

        MeltUpgradePlan plan;
        for (const auto& name : selected) {
            MeltUpgradeEntry e;
            e.name = name;
            e.oldVersion = currentVersions[name];

            auto evIt = explicitVersions.find(name);
            if (evIt != explicitVersions.end()) {
                auto carrier = findRepoCarrying(name, evIt->second, *repos);
                if (!carrier) return carrier.takeError();
                if (carrier->empty()) {
                    return err("upgrade --melt: no repository carries "
                               "melt '" + name + "@" + evIt->second + "'");
                }
                e.newVersion = evIt->second;
                e.resolvedFromRepo = *carrier;
            } else {
                auto pick = highestAcrossRepos(name, *repos);
                if (!pick) return pick.takeError();
                if (pick->version.empty()) {
                    return err("upgrade --melt: no repository has any "
                               "version of melt '" + name + "'");
                }
                e.newVersion = pick->version;
                e.resolvedFromRepo = pick->fromRepo;
            }

            e.changed = e.oldVersion != e.newVersion;
            if (e.changed) {
                auto oldBytes = fetchSidecar(name, e.oldVersion, *repos);
                if (!oldBytes) return oldBytes.takeError();
                auto newBytes = fetchSidecar(name, e.newVersion, *repos);
                if (!newBytes) return newBytes.takeError();
                e.depDelta = diffMeltDeps(
                    parseMeltSidecar(*oldBytes).dependencies,
                    parseMeltSidecar(*newBytes).dependencies);
            }
            plan.entries.push_back(std::move(e));
        }
        return plan;
    }

    llvm::Expected<std::string> applyMeltUpgradePlan(
        const std::string& manifestSource,
        const MeltUpgradePlan& plan) {
        std::string src = manifestSource;
        for (const auto& e : plan.entries) {
            if (!e.changed) continue;
            auto rewritten = setMeltImportInManifest(
                src, e.name, e.oldVersion, e.newVersion);
            if (!rewritten) return rewritten.takeError();
            src = std::move(*rewritten);
        }
        return src;
    }

} // namespace cajeta::buildtool
