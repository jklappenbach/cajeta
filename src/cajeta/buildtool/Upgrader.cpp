#include "cajeta/buildtool/Upgrader.h"

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/ManifestEditor.h"
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

        // Pull the constraint string for `name` from the manifest's
        // `settings.dependencies` block. Returns nullopt when the dep
        // isn't declared.
        std::optional<std::string> manifestConstraintFor(
            const Manifest& m, const std::string& name) {
            const auto* deps = m.settingsRaw.getObject("dependencies");
            if (!deps) return std::nullopt;
            const llvm::json::Value* v = deps->get(name);
            if (!v) return std::nullopt;
            // Two accepted shapes — string or { "version": "..." }.
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

        // Extract `settings.capabilities` (array of strings) from a
        // sidecar manifest's raw JSON bytes. Returns an empty set when
        // the sidecar is empty, doesn't parse, or doesn't declare any.
        // (Sidecar-less repos contribute no capabilities; that's the
        // intended backwards-compat behavior — they trigger no false
        // capability-change flags.)
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

        // Highest version of `name` across all repos. Returns the
        // version + the first repo that carries it (priority order).
        // Empty version when no repo has any version of `name`.
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

        // Locate `version` of `name` in the configured repos. Returns
        // the repo name that carries it (priority order); empty when
        // no repo has it.
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

        // Pull the sidecar `cajeta.json` (raw bytes) for `name@version`
        // from whichever repo carries it. Empty string when no repo
        // carries the version, or when the carrying repo has no
        // sidecar (pre-sidecar archives).
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

        // Map name → sidecar JSON pulled during the baseline
        // `resolveMvs` walk. The walk returns ResolvedDependency rows
        // but their sidecar bytes aren't surfaced — re-fetch via the
        // repo set instead (cheap: cache + listVersions are local).
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

        // Build the same repo set the resolver uses. Reuse the
        // .cajeta/cache/downloads/ stage so HTTP fetches land in the
        // same place the build sees.
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

        // Names targeted by this upgrade — explicit selection or
        // every declared direct dep.
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

        // Baseline = current resolved graph (so we can compare new
        // capabilities against what the consumer is actually shipping
        // today, not just what's literally written in the manifest).
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
            // A baseline failure is informational, not fatal — the
            // user is upgrading, possibly to fix an unresolvable
            // current state. Swallow the error and proceed with
            // empty old sidecars.
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

            // Pick newVersion: explicit when provided, else highest.
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
            // When the baseline was unresolvable (no oldVersion), any
            // valid new version is a change — the user is fixing the
            // build.
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

} // namespace cajeta::buildtool
