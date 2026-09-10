#include "cajeta/buildtool/Resolver.h"

#include "cajeta/buildtool/Melt.h"
#include "cajeta/buildtool/OllaStore.h"
#include "cajeta/buildtool/repo/FilesystemRepository.h"
#include "cajeta/buildtool/repo/GitRepository.h"
#include "cajeta/buildtool/repo/TimingRepository.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // True for the `cajeta.*` modules embedded in the compiler, which the resolver
        // satisfies implicitly; first-party `cajeta.*` packages still resolve normally.
        bool isBuiltinStdlibDep(const std::string& name) {
            static const char* const kStdlibRoots[] = {
                "cajeta.codec", "cajeta.collection", "cajeta.error",
                "cajeta.hash", "cajeta.io", "cajeta.lang", "cajeta.concurrent",
                "cajeta.reflect", "cajeta.time", "cajeta.wire",
            };
            for (const char* root : kStdlibRoots) {
                std::string r(root);
                if (name == r || name.rfind(r + ".", 0) == 0) return true;
            }
            return false;
        }

        std::vector<std::string> splitDots(const std::string& s) {
            std::vector<std::string> out;
            std::string cur;
            for (char c : s) {
                if (c == '.') { out.push_back(cur); cur.clear(); }
                else cur += c;
            }
            out.push_back(cur);
            return out;
        }

        // The numeric core of a semver: everything before the first `-` or `+`.
        std::string semverCore(const std::string& v) {
            size_t cut = v.size();
            for (size_t i = 0; i < v.size(); ++i) {
                if (v[i] == '-' || v[i] == '+') { cut = i; break; }
            }
            return v.substr(0, cut);
        }

        bool isAllDigits(const std::string& s) {
            if (s.empty()) return false;
            for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            return true;
        }

    } // namespace

    int compareVersions(const std::string& a, const std::string& b) {
        auto aParts = splitDots(semverCore(a));
        auto bParts = splitDots(semverCore(b));
        size_t n = std::max(aParts.size(), bParts.size());
        for (size_t i = 0; i < n; ++i) {
            std::string av = i < aParts.size() ? aParts[i] : "0";
            std::string bv = i < bParts.size() ? bParts[i] : "0";
            if (isAllDigits(av) && isAllDigits(bv)) {
                unsigned long long ai = std::stoull(av);
                unsigned long long bi = std::stoull(bv);
                if (ai != bi) return ai < bi ? -1 : 1;
            } else {
                int c = av.compare(bv);
                if (c != 0) return c < 0 ? -1 : 1;
            }
        }
        return 0;
    }

    int compareMajor(const std::string& a, const std::string& b) {
        auto aParts = splitDots(semverCore(a));
        auto bParts = splitDots(semverCore(b));
        std::string aMaj = aParts.empty() ? "0" : aParts[0];
        std::string bMaj = bParts.empty() ? "0" : bParts[0];
        if (isAllDigits(aMaj) && isAllDigits(bMaj)) {
            unsigned long long ai = std::stoull(aMaj);
            unsigned long long bi = std::stoull(bMaj);
            if (ai == bi) return 0;
            return ai < bi ? -1 : 1;
        }
        int c = aMaj.compare(bMaj);
        return c == 0 ? 0 : (c < 0 ? -1 : 1);
    }

    namespace {

        std::string trim(const std::string& s) {
            size_t b = 0, e = s.size();
            while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
            return s.substr(b, e - b);
        }

        // One atomic piece of a comma-separated AND. Eq is release-only ("1.2.3"
        // excludes "1.2.3-rc1"), and Wild holds the segment prefix of a "1.2.*" form.
        enum class Op { Eq, Wild, Ge, Gt, Le, Lt };
        struct Atom { Op op; std::string operand; };

        // Parses one comma-piece: a longest-match ">=", ">", "<=", "<" or "=" prefix,
        // else Wild if the operand contains '*', else Eq.
        llvm::Expected<Atom> parseAtom(const std::string& raw) {
            std::string s = trim(raw);
            if (s.empty()) {
                return err("empty constraint atom");
            }
            auto starts = [&](const char* p) {
                size_t n = std::strlen(p);
                return s.size() >= n && s.compare(0, n, p) == 0;
            };
            Op op = Op::Eq;
            size_t consume = 0;
            if (starts(">=")) { op = Op::Ge; consume = 2; }
            else if (starts("<=")) { op = Op::Le; consume = 2; }
            else if (starts(">"))  { op = Op::Gt; consume = 1; }
            else if (starts("<"))  { op = Op::Lt; consume = 1; }
            else if (starts("="))  { op = Op::Eq; consume = 1; }
            std::string operand = trim(s.substr(consume));
            if (operand.empty()) {
                return err("constraint atom '" + raw + "' has no version");
            }
            if (op == Op::Eq && operand.find('*') != std::string::npos) {
                op = Op::Wild;
                operand = operand.substr(0, operand.find('*'));
                if (!operand.empty() && operand.back() == '.') {
                    operand.pop_back();
                }
            }
            return Atom{op, operand};
        }

        bool atomMatches(const Atom& a, const std::string& version) {
            switch (a.op) {
                case Op::Eq:
                    return version == a.operand;
                case Op::Wild: {
                    if (a.operand.empty()) return true;  // bare "*"
                    auto pParts = splitDots(a.operand);
                    auto vParts = splitDots(semverCore(version));
                    if (vParts.size() < pParts.size()) return false;
                    for (size_t i = 0; i < pParts.size(); ++i) {
                        if (vParts[i] != pParts[i]) return false;
                    }
                    return true;
                }
                case Op::Ge: return compareVersions(version, a.operand) >= 0;
                case Op::Gt: return compareVersions(version, a.operand) >  0;
                case Op::Le: return compareVersions(version, a.operand) <= 0;
                case Op::Lt: return compareVersions(version, a.operand) <  0;
            }
            return false;
        }

        std::vector<std::string> splitCommas(const std::string& s) {
            std::vector<std::string> out;
            std::string cur;
            for (char c : s) {
                if (c == ',') { out.push_back(cur); cur.clear(); }
                else cur += c;
            }
            out.push_back(cur);
            return out;
        }

    } // namespace

    bool versionSatisfies(const std::string& version,
                          const std::string& constraint) {
        if (constraint == "*") return true;
        if (constraint.empty()) return false;

        // An unparsable piece makes the whole constraint unsatisfiable rather than
        // raising, which keeps this predicate total at the resolver boundary.
        for (const auto& piece : splitCommas(constraint)) {
            auto atom = parseAtom(piece);
            if (!atom) {
                consumeError(atom.takeError());
                return false;
            }
            if (!atomMatches(*atom, version)) return false;
        }
        return true;
    }

    namespace {

        // The highest version in `candidates` satisfying `constraint`, else empty.
        std::string highestSatisfying(
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

    namespace {

        // The lowest version in `candidates` satisfying every constraint, else empty.
        std::string lowestSatisfyingAll(
            const std::vector<std::string>& candidates,
            const std::vector<std::string>& constraints) {
            std::vector<std::string> sorted = candidates;
            std::sort(sorted.begin(), sorted.end(),
                [](const std::string& a, const std::string& b) {
                    return compareVersions(a, b) < 0;
                });
            for (const auto& v : sorted) {
                bool ok = true;
                for (const auto& c : constraints) {
                    if (!versionSatisfies(v, c)) { ok = false; break; }
                }
                if (ok) return v;
            }
            return "";
        }

        // Resolves one dependency against the priority-ordered repos, honoring `from`,
        // caching the artifact and leaving its sidecar bytes in `manifestJsonOut`.
        llvm::Expected<ResolvedDependency> resolveOne(
            const DependencySpec& dep,
            const std::vector<RepositoryPtr>& repos,
            ArtifactCache& cache,
            std::string& manifestJsonOut) {
            ResolvedDependency resolved;
            resolved.name = dep.name;
            manifestJsonOut.clear();

            for (const auto& repo : repos) {
                if (dep.fromRepo && repo->name() != *dep.fromRepo) continue;

                auto versions = repo->listVersions(dep.name);
                if (!versions) return versions.takeError();
                std::string version = highestSatisfying(
                    *versions, dep.versionConstraint);
                if (version.empty()) continue;  // try next repo

                auto path = repo->fetch(dep.name, version);
                if (!path) return path.takeError();

                auto cached = cache.insert(*path);
                if (!cached) return cached.takeError();

                auto sidecar = repo->fetchManifestJson(dep.name, version);
                if (!sidecar) return sidecar.takeError();
                if (sidecar->has_value()) {
                    manifestJsonOut = **sidecar;
                }

                resolved.version = version;
                resolved.resolvedFromRepo = repo->name();
                resolved.artifactPath = *cached;
                resolved.sha256 = ArtifactCache::sha256OfFile(*cached);
                return resolved;
            }

            std::string repoList;
            for (const auto& r : repos) {
                if (dep.fromRepo && r->name() != *dep.fromRepo) continue;
                if (!repoList.empty()) repoList += ", ";
                repoList += r->name();
            }
            return err("dependency '" + dep.name + " " +
                       dep.versionConstraint +
                       "' not satisfied by any repository (tried: " +
                       (repoList.empty() ? "<none>" : repoList) + ")");
        }

    } // namespace

    llvm::Expected<std::vector<ResolvedDependency>> resolveDirect(
        const std::vector<DependencySpec>& deps,
        const std::vector<RepositoryPtr>& repos,
        ArtifactCache& cache) {
        std::vector<ResolvedDependency> out;
        out.reserve(deps.size());

        for (const auto& dep : deps) {
            if (dep.versionConstraint.empty()) {
                // A path or git source form carries no constraint and resolves elsewhere.
                continue;
            }
            std::string _manifestJson;
            auto r = resolveOne(dep, repos, cache, _manifestJson);
            if (!r) return r.takeError();
            out.push_back(std::move(*r));
        }
        return out;
    }

    namespace {

        // Per-package state across the MVS fixed-point iteration: every constraint ever
        // declared, the single `from` pin, the current pick. directRoot beats override.
        struct MvsState {
            std::vector<std::string> constraints;
            std::optional<std::string> fromRepo;
            bool directRoot = false;
            std::optional<std::string> overrideConstraint;
            bool overrideAllowsMajorDowngrade = false;
            // Resolve from this local directory instead of any repository.
            std::optional<std::string> overridePath;
            // Resolve from this git URL and ref, cloned on demand into the stage dir.
            std::optional<std::string> overrideGitUrl;
            std::optional<std::string> overrideGitRef;

            std::string version;
            std::string resolvedFromRepo;
            std::string artifactPath;
            std::string sha256;
            std::string manifestJson;
            std::vector<DependencySpec> childDeps;
            bool dirty = true;          // needs a pick or re-pick
            bool everPicked = false;
        };

        // The override's lone constraint when one is in effect, else the gathered set.
        std::vector<std::string> effectiveConstraints(const MvsState& s) {
            if (s.overrideConstraint && !s.directRoot) {
                return { *s.overrideConstraint };
            }
            return s.constraints;
        }

        // One package's chosen version, its repo, the cached artifact and the sidecar
        // manifest, which is empty when the winning repo cannot produce one.
        struct MvsPick {
            std::string version;
            std::string resolvedFromRepo;
            std::string artifactPath;
            std::string sha256;
            std::string manifestJson;
        };

        // Builds a pick from a local-path override, whose cajeta.json must declare
        // `details.name == name` and whose `.cja` must already be built.
        llvm::Expected<MvsPick> pickFromPathOverride(
            const std::string& name,
            const std::string& path,
            ArtifactCache& cache) {
            namespace fs = std::filesystem;
            fs::path root = path;
            std::error_code ec;
            if (!fs::is_directory(root, ec)) {
                return err("override for '" + name +
                           "': path '" + path +
                           "' is not a directory");
            }
            fs::path sidecar = root / "cajeta.json";
            if (!fs::is_regular_file(sidecar, ec)) {
                return err("override for '" + name +
                           "': no cajeta.json at '" + sidecar.string() +
                           "'");
            }
            std::ifstream in(sidecar, std::ios::binary);
            if (!in) {
                return err("override for '" + name +
                           "': cannot open '" + sidecar.string() + "'");
            }
            std::ostringstream buf;
            buf << in.rdbuf();
            std::string sidecarBytes = buf.str();

            auto parsed = llvm::json::parse(sidecarBytes);
            if (!parsed) {
                llvm::consumeError(parsed.takeError());
                return err("override for '" + name +
                           "': cajeta.json at '" + sidecar.string() +
                           "' is not valid JSON");
            }
            const auto* obj = parsed->getAsObject();
            if (!obj) {
                return err("override for '" + name +
                           "': cajeta.json at '" + sidecar.string() +
                           "' must be a JSON object");
            }
            const auto* details = obj->getObject("details");
            if (!details) {
                return err("override for '" + name +
                           "': cajeta.json at '" + sidecar.string() +
                           "' missing 'details' block");
            }
            auto declName = details->getString("name");
            auto declVer  = details->getString("version");
            if (!declName || !declVer) {
                return err("override for '" + name +
                           "': cajeta.json at '" + sidecar.string() +
                           "' must declare details.name + details.version");
            }
            if (declName->str() != name) {
                return err("override for '" + name +
                           "': cajeta.json at '" + sidecar.string() +
                           "' declares name='" + declName->str() +
                           "' which does not match");
            }

            std::string version = declVer->str();
            fs::path artifact = root / "build" / "archive" /
                                (name + "-" + version + ".cja");
            if (!fs::is_regular_file(artifact, ec)) {
                return err("override for '" + name +
                           "': expected pre-built artifact at '" +
                           artifact.string() + "' but it does not exist. "
                           "v1 limitation: run `cajeta build` in '" +
                           root.string() + "' first.");
            }

            auto cached = cache.insert(artifact.string());
            if (!cached) return cached.takeError();

            MvsPick out;
            out.version = version;
            out.resolvedFromRepo = "<path-override>";
            out.artifactPath = *cached;
            out.sha256 = ArtifactCache::sha256OfFile(*cached);
            out.manifestJson = sidecarBytes;
            return out;
        }

        // Builds a pick from a git override through an ephemeral GitRepository, which
        // clones lazily into the stage dir; the same built-package contract applies.
        llvm::Expected<MvsPick> pickFromGitOverride(
            const std::string& name,
            const std::string& url,
            const std::string& ref,
            const std::string& stageDir,
            ArtifactCache& cache) {
            if (stageDir.empty()) {
                return err("override for '" + name +
                           "': git replacement requires a stage "
                           "directory; resolveMvs's caller must pass "
                           "one (resolveProjectDependencies does)");
            }
            GitRepository repo("<git-override:" + name + ">",
                               url, ref, /*subdir=*/"", stageDir);
            auto versions = repo.listVersions(name);
            if (!versions) return versions.takeError();
            if (versions->empty()) {
                return err("override for '" + name +
                           "': git checkout at '" + url + "@" + ref +
                           "' declares a different package");
            }
            const std::string& v = (*versions)[0];

            auto path = repo.fetch(name, v);
            if (!path) return path.takeError();
            auto cached = cache.insert(*path);
            if (!cached) return cached.takeError();
            auto sidecar = repo.fetchManifestJson(name, v);
            if (!sidecar) return sidecar.takeError();

            MvsPick out;
            out.version = v;
            out.resolvedFromRepo = "<git-override>";
            out.artifactPath = *cached;
            out.sha256 = ArtifactCache::sha256OfFile(*cached);
            if (sidecar->has_value()) out.manifestJson = **sidecar;
            return out;
        }

        // Picks the lowest version satisfying every constraint, walking the repos in
        // priority order: the first repo with any satisfying version wins.
        llvm::Expected<MvsPick> pickLowestForAll(
            const std::string& name,
            const std::vector<std::string>& constraints,
            const std::optional<std::string>& fromRepo,
            const std::vector<RepositoryPtr>& repos,
            ArtifactCache& cache,
            const std::string& ollaWriteThroughRoot) {
            for (const auto& repo : repos) {
                if (fromRepo && repo->name() != *fromRepo) continue;
                auto versions = repo->listVersions(name);
                if (!versions) return versions.takeError();
                std::string v = lowestSatisfyingAll(*versions, constraints);
                if (v.empty()) continue;
                auto path = repo->fetch(name, v);
                if (!path) return path.takeError();
                auto cached = cache.insert(*path);
                if (!cached) return cached.takeError();
                auto sidecar = repo->fetchManifestJson(name, v);
                if (!sidecar) return sidecar.takeError();

                MvsPick out;
                out.version = v;
                out.resolvedFromRepo = repo->name();
                out.artifactPath = *cached;
                out.sha256 = ArtifactCache::sha256OfFile(*cached);
                if (sidecar->has_value()) out.manifestJson = **sidecar;

                // Mirror a remote fetch into ~/.olla so the next resolve is a local
                // hit; trust on first use, so the fetched bytes define the hash.
                if (!ollaWriteThroughRoot.empty() && repo->name() != "olla") {
                    OllaStore ollaStore(ollaWriteThroughRoot);
                    auto wt = ollaStore.writeVerified(
                        name, v, *cached, /*expectedSha256=*/"",
                        out.manifestJson);
                    if (!wt) return wt.takeError();
                }
                return out;
            }
            std::string joined;
            for (const auto& c : constraints) {
                if (!joined.empty()) joined += ", ";
                joined += c;
            }
            std::string repoList;
            for (const auto& r : repos) {
                if (fromRepo && r->name() != *fromRepo) continue;
                if (!repoList.empty()) repoList += ", ";
                repoList += r->name();
            }
            return err("no version of '" + name +
                       "' satisfies constraints [" + joined +
                       "] (tried: " +
                       (repoList.empty() ? "<none>" : repoList) + ")");
        }

    } // namespace

    llvm::Expected<ResolvedGraph> resolveMvsGraph(
        const std::vector<DependencySpec>& deps,
        const std::vector<RepositoryPtr>& repos,
        ArtifactCache& cache,
        const std::vector<OverrideSpec>& overrides,
        ResolverTimings* timings,
        const std::string& gitOverrideStageDir,
        const std::string& ollaWriteThroughRoot) {

        // Pre-flight: split the overrides into version, path and git forms.
        std::unordered_map<std::string, const OverrideSpec*> overrideMap;
        std::unordered_map<std::string, const OverrideSpec*> pathOverrideMap;
        std::unordered_map<std::string, const OverrideSpec*> gitOverrideMap;
        for (const auto& o : overrides) {
            if (o.git) {
                if (!o.rev) {
                    return err("settings.overrides." + o.name +
                               ": git replacement requires 'rev' "
                               "(branch/tag/commit)");
                }
                gitOverrideMap[o.name] = &o;
                continue;
            }
            if (o.path) {
                pathOverrideMap[o.name] = &o;
                continue;
            }
            if (o.versionConstraint.empty()) {
                return err("settings.overrides." + o.name +
                           ": override must specify a version constraint");
            }
            overrideMap[o.name] = &o;
        }

        std::unordered_map<std::string, MvsState> state;
        std::vector<std::string> insertionOrder;

        // Adds a constraint and optional `from` pin for `name`, returning true when the
        // set actually changed, which marks the package dirty for a re-pick.
        auto addConstraint = [&](const std::string& name,
                                 const std::string& constraint,
                                 const std::optional<std::string>& fromRepo,
                                 bool asDirectRoot)
            -> llvm::Expected<bool> {
            auto [it, inserted] = state.try_emplace(name);
            auto& s = it->second;
            if (inserted) {
                insertionOrder.push_back(name);
                // Wire override metadata at first sight: which step found it is irrelevant.
                auto it2 = overrideMap.find(name);
                if (it2 != overrideMap.end()) {
                    s.overrideConstraint = it2->second->versionConstraint;
                    s.overrideAllowsMajorDowngrade =
                        it2->second->allowMajorDowngrade;
                }
                auto itp = pathOverrideMap.find(name);
                if (itp != pathOverrideMap.end()) {
                    s.overridePath = *itp->second->path;
                    s.overrideAllowsMajorDowngrade =
                        itp->second->allowMajorDowngrade;
                }
                auto itg = gitOverrideMap.find(name);
                if (itg != gitOverrideMap.end()) {
                    s.overrideGitUrl = *itg->second->git;
                    s.overrideGitRef = *itg->second->rev;
                    s.overrideAllowsMajorDowngrade =
                        itg->second->allowMajorDowngrade;
                }
            }

            bool changed = inserted;
            if (std::find(s.constraints.begin(), s.constraints.end(),
                          constraint) == s.constraints.end()) {
                s.constraints.push_back(constraint);
                changed = true;
            }
            if (asDirectRoot && !s.directRoot) {
                s.directRoot = true;
                changed = true;  // direct-vs-override flip changes pick
            }
            if (fromRepo) {
                if (s.fromRepo && *s.fromRepo != *fromRepo) {
                    return err("conflicting 'from' repository for '" + name +
                               "': '" + *s.fromRepo + "' vs '" + *fromRepo + "'");
                }
                if (!s.fromRepo) {
                    s.fromRepo = fromRepo;
                    changed = true;
                }
            }
            if (changed) s.dirty = true;
            return changed;
        };

        // Root deps are flagged directRoot, so a same-named override is ignored.
        for (const auto& d : deps) {
            if (d.versionConstraint.empty()) continue;  // 6c forms
            auto added = addConstraint(d.name, d.versionConstraint,
                                       d.fromRepo, /*asDirectRoot=*/true);
            if (!added) return added.takeError();
        }

        // Pick and propagate until nothing is dirty. A re-pick can only raise a version
        // within the finite repo set, so this terminates, and walking `insertionOrder`
        // keeps the output deterministic.
        bool anyDirty = true;
        while (anyDirty) {
            anyDirty = false;
            if (timings) ++timings->mvsIterations;
            for (size_t i = 0; i < insertionOrder.size(); ++i) {
                const std::string name = insertionOrder[i];
                auto& s = state[name];
                if (!s.dirty) continue;

                llvm::Expected<MvsPick> pick = llvm::Expected<MvsPick>(
                    MvsPick{});
                if (s.overridePath && !s.directRoot) {
                    pick = pickFromPathOverride(name, *s.overridePath, cache);
                } else if (s.overrideGitUrl && !s.directRoot) {
                    pick = pickFromGitOverride(
                        name, *s.overrideGitUrl, *s.overrideGitRef,
                        gitOverrideStageDir, cache);
                } else {
                    pick = pickLowestForAll(
                        name, effectiveConstraints(s), s.fromRepo,
                        repos, cache, ollaWriteThroughRoot);
                }
                if (!pick) return pick.takeError();

                bool versionChanged = !s.everPicked ||
                                      s.version != pick->version;
                s.version = pick->version;
                s.resolvedFromRepo = pick->resolvedFromRepo;
                s.artifactPath = pick->artifactPath;
                s.sha256 = pick->sha256;
                s.manifestJson = pick->manifestJson;
                s.everPicked = true;
                s.dirty = false;

                if (versionChanged) {
                    // The prior version's children are discarded, but the constraints
                    // they added stay: that over-constrains, it never breaks the solve.
                    s.childDeps.clear();
                    if (!s.manifestJson.empty()) {
                        auto child = loadManifestString(
                            s.manifestJson, name + "@" + s.version);
                        if (!child) return child.takeError();
                        auto childDeps = parseDependencies(*child);
                        if (!childDeps) return childDeps.takeError();
                        s.childDeps = std::move(*childDeps);
                    }
                    for (const auto& cd : s.childDeps) {
                        if (cd.versionConstraint.empty()) continue;
                        auto added = addConstraint(
                            cd.name, cd.versionConstraint, cd.fromRepo,
                            /*asDirectRoot=*/false);
                        if (!added) return added.takeError();
                        if (*added) anyDirty = true;
                    }
                    // A cycle may have re-dirtied `name` during this pass; preserve that.
                    if (state[name].dirty) anyDirty = true;
                }
            }
        }

        // Audit: a package forced by an override, and not a direct root dep, is compared
        // against what its transitive constraints would have picked. Dropping the major
        // version below that is an error unless allow-major-downgrade is set.
        for (const auto& name : insertionOrder) {
            const auto& s = state[name];
            bool overridden = (s.overrideConstraint || s.overridePath ||
                               s.overrideGitUrl) &&
                              !s.directRoot;
            if (!overridden) continue;
            if (s.constraints.empty()) continue;  // override unused — nothing to compare

            // An unsatisfiable transitive set means the override rescued the build.
            auto baseline = pickLowestForAll(
                name, s.constraints, s.fromRepo, repos, cache,
                /*ollaWriteThroughRoot=*/"");  // hypothetical — no write-through
            if (!baseline) {
                consumeError(baseline.takeError());
                continue;
            }
            if (compareMajor(s.version, baseline->version) < 0 &&
                !s.overrideAllowsMajorDowngrade) {
                std::string transitives;
                for (const auto& c : s.constraints) {
                    if (!transitives.empty()) transitives += ", ";
                    transitives += c;
                }
                return err("override for '" + name + "' pins to '" +
                           s.version + "' but transitives need [" +
                           transitives + "] (baseline pick was '" +
                           baseline->version +
                           "'); set 'allow-major-downgrade': true on the "
                           "override to accept this drop");
            }
        }

        ResolvedGraph out;
        out.packages.reserve(insertionOrder.size());
        // The direct deps as seeded: 6c forms were never added to the solve.
        for (const auto& d : deps) {
            if (d.versionConstraint.empty()) continue;
            out.roots.push_back(d);
        }
        for (const auto& name : insertionOrder) {
            const auto& s = state[name];
            ResolvedDependency r;
            r.name = name;
            r.version = s.version;
            r.resolvedFromRepo = s.resolvedFromRepo;
            r.artifactPath = s.artifactPath;
            r.sha256 = s.sha256;
            out.packages.push_back(std::move(r));
            // No sidecar means the children are unknown, which is opaque, not empty.
            if (s.manifestJson.empty()) {
                out.opaque.insert(name);
                continue;
            }
            auto& kids = out.children[name];
            for (const auto& cd : s.childDeps) {
                if (cd.versionConstraint.empty()) continue;  // solver skipped it
                kids.push_back(cd);
            }
        }
        return out;
    }

    llvm::Expected<std::vector<ResolvedDependency>> resolveMvs(
        const std::vector<DependencySpec>& deps,
        const std::vector<RepositoryPtr>& repos,
        ArtifactCache& cache,
        const std::vector<OverrideSpec>& overrides,
        ResolverTimings* timings,
        const std::string& gitOverrideStageDir,
        const std::string& ollaWriteThroughRoot) {
        auto g = resolveMvsGraph(deps, repos, cache, overrides, timings,
                                 gitOverrideStageDir, ollaWriteThroughRoot);
        if (!g) return g.takeError();
        return std::move(g->packages);
    }

    llvm::Expected<ResolvedGraph>
    resolveProjectGraph(
        const Manifest& m,
        const std::string& projectRoot,
        std::optional<std::string> homeOverride,
        ResolverTimings* timings) {

        auto totalStart = std::chrono::steady_clock::now();
        auto closeTotal = [&]() {
            if (timings) {
                timings->total =
                    std::chrono::duration_cast<ResolverTimings::Duration>(
                        std::chrono::steady_clock::now() - totalStart);
            }
        };

        auto deps = parseDependencies(m);
        if (!deps) { closeTotal(); return deps.takeError(); }

        // The embedded stdlib satisfies these, so a stdlib-only project resolves with
        // no repositories configured and no network.
        deps->erase(std::remove_if(deps->begin(), deps->end(),
                        [](const DependencySpec& d) {
                            return isBuiltinStdlibDep(d.name);
                        }),
                    deps->end());

        auto repoSpecs = parseRepositories(m);
        if (!repoSpecs) { closeTotal(); return repoSpecs.takeError(); }
        // No early "deps but no repositories" error: the implicit ~/.olla repository
        // prepended below is always a source, and the pick step reports what is missing.
        // Downloads stage under the project's own .cajeta, scoping interrupted fetches.
        std::string downloadStage =
            (std::filesystem::path(projectRoot) / ".cajeta" / "cache" /
             "downloads").string();
        auto repos = buildRepositories(*repoSpecs, downloadStage);
        if (!repos) { closeTotal(); return repos.takeError(); }

        // Melts resolve before deps so their table can substitute `"*"` constraints.
        ArtifactCache cache(projectRoot, homeOverride);
        auto melts = resolveMelts(m, *repos, cache);
        if (!melts) { closeTotal(); return melts.takeError(); }

        if (!melts->repositories.empty()) {
            // stable_sort keeps declaration order among equal priorities.
            auto specs = *repoSpecs;
            for (const auto& r : melts->repositories) specs.push_back(r);
            std::stable_sort(specs.begin(), specs.end(),
                [](const RepositorySpec& a, const RepositorySpec& b) {
                    return a.priority > b.priority;
                });
            auto rebuilt = buildRepositories(specs, downloadStage);
            if (!rebuilt) { closeTotal(); return rebuilt.takeError(); }
            *repos = std::move(*rebuilt);
        }

        // Any `"*"` left after this substitution is a hard error; a consumer that
        // diverges from a melt's curated version wins, with a warning.
        std::map<std::string, std::string> meltProvidedBy;
        std::vector<std::string> meltDivergenceWarnings;
        if (auto e = applyMeltLookups(*deps, *melts, meltProvidedBy,
                                      meltDivergenceWarnings)) {
            closeTotal();
            return std::move(e);
        }
        for (const auto& w : meltDivergenceWarnings) {
            llvm::errs() << "warning: " << w << "\n";
        }

        if (deps->empty()) {
            closeTotal();
            return ResolvedGraph{};  // no deps → no work
        }

        auto overrides = parseOverrides(m);
        if (!overrides) { closeTotal(); return overrides.takeError(); }

        // Local-first: the pick short-circuits on the first repo with a satisfying
        // version, so a hit in ~/.olla never touches a declared remote.
        std::string ollaRoot = OllaStore::resolveRoot(homeOverride);
        repos->insert(repos->begin(),
            std::make_shared<FilesystemRepository>("olla", ollaRoot));

        // wrapWithTimings passes through on a null pointer, so untimed runs pay nothing.
        std::vector<RepositoryPtr> repoList =
            wrapWithTimings(*repos, timings);

        auto result = resolveMvsGraph(
            *deps, repoList, cache, *overrides, timings, downloadStage,
            /*ollaWriteThroughRoot=*/ollaRoot);
        if (result && timings) {
            timings->depsResolved = static_cast<int>(result->packages.size());
        }
        closeTotal();
        return result;
    }

    llvm::Expected<std::vector<ResolvedDependency>>
    resolveProjectDependencies(
        const Manifest& m,
        const std::string& projectRoot,
        std::optional<std::string> homeOverride,
        ResolverTimings* timings) {
        auto g = resolveProjectGraph(m, projectRoot, std::move(homeOverride),
                                     timings);
        if (!g) return g.takeError();
        return std::move(g->packages);
    }

} // namespace cajeta::buildtool
