#include "cajeta/buildtool/Resolver.h"

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

        // Strip everything after the first `-` or `+`. Used to
        // separate the numeric core of a semver from prerelease /
        // build-meta tags.
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

        // A single atomic constraint piece. The full constraint
        // string is a comma-separated AND of these.
        //
        //   op == Eq, operand "1.2.3"   → exact (release-only — does
        //                                  NOT match "1.2.3-rc1")
        //   op == Wild, operand "1.2"   → "1.2.*" form
        //   op == Ge/Gt/Le/Lt           → range operator
        enum class Op { Eq, Wild, Ge, Gt, Le, Lt };
        struct Atom { Op op; std::string operand; };

        // Parse one comma-piece. Recognized prefixes (longest-match):
        //   ">="  ">"  "<="  "<"  "="
        // No prefix + contains '*' → Wild.
        // No prefix + bare semver  → Eq.
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
                    // Release-only: prereleases (`1.2.3-rc1`) do NOT
                    // satisfy `1.2.3`. Matches Phase 6a behaviour.
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

        // Comma-separated AND. Each piece is parsed into an Atom.
        // If any piece fails to parse, the whole constraint is
        // unsatisfiable (returns false rather than raising — keeps
        // the predicate total at the resolver boundary; bad
        // constraints are caught at parse time when we add
        // validation in 6b).
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

        // Pick the highest version in `candidates` that satisfies
        // `constraint`. Returns empty string when no candidate
        // satisfies.
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

        // Pick the lowest version in `candidates` that satisfies
        // every entry in `constraints`. Returns the empty string if
        // no candidate satisfies the conjunction.
        std::string lowestSatisfyingAll(
            const std::vector<std::string>& candidates,
            const std::vector<std::string>& constraints) {
            // Ascending walk — first hit wins.
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

        // Resolve one dependency against the priority-ordered repos.
        // Honors the `from` pin; picks the highest version satisfying
        // `dep.versionConstraint`. Pulls the picked artifact into
        // the content-addressed cache. Returns a fully populated
        // ResolvedDependency on success.
        //
        // Also fetches the dep's sidecar `cajeta.json` (when the
        // winning repository can produce one) into
        // `manifestJsonOut` — callers that need the dep's own
        // constraints (the MVS walker) avoid a second fetch.
        // `manifestJsonOut` is left empty when no sidecar is
        // available.
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
                // Phase 6a punts on path / git source forms; their
                // resolution lands in 6c. Skip with a marker so
                // callers can decide whether to error or proceed.
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

        // Per-package state maintained across the MVS fixed-point
        // iteration. `constraints` accumulates every constraint
        // string ever declared against this package; `fromRepo` is
        // the (single) declared `from` pin, if any (conflicts
        // error). The rest is the currently-picked artifact and
        // the parsed declared-deps of its sidecar.
        //
        // Override interplay (Phase 6b):
        //   - `directRoot`: this package appears in the project's
        //     own `dependencies` block. Per spec, direct beats
        //     override — `overrideConstraint` is ignored when
        //     `directRoot` is set.
        //   - `overrideConstraint`: when set (and !directRoot), the
        //     pick uses ONLY this constraint set, NOT the gathered
        //     transitive constraints. The gathered set is kept for
        //     the post-MVS major-downgrade audit.
        struct MvsState {
            std::vector<std::string> constraints;
            std::optional<std::string> fromRepo;
            bool directRoot = false;
            std::optional<std::string> overrideConstraint;
            bool overrideAllowsMajorDowngrade = false;
            // Phase 6c: path replacement override. When set (and not
            // shadowed by a directRoot match), the package is resolved
            // from this local directory instead of through any
            // repository. Version + artifact + sidecar come from the
            // directory's own cajeta.json + build/archive layout.
            std::optional<std::string> overridePath;

            std::string version;
            std::string resolvedFromRepo;
            std::string artifactPath;
            std::string sha256;
            std::string manifestJson;
            std::vector<DependencySpec> childDeps;
            bool dirty = true;          // needs a pick or re-pick
            bool everPicked = false;
        };

        // Returns the effective constraint set used for picking:
        // the override's lone constraint when override is in effect,
        // else the gathered transitive set.
        std::vector<std::string> effectiveConstraints(const MvsState& s) {
            if (s.overrideConstraint && !s.directRoot) {
                return { *s.overrideConstraint };
            }
            return s.constraints;
        }

        // Pick lowest-satisfying-all across the priority-ordered
        // repos. Returns the chosen version + repo + artifact
        // path + sidecar manifest (or empty manifest when the
        // winning repo can't produce one).
        struct MvsPick {
            std::string version;
            std::string resolvedFromRepo;
            std::string artifactPath;
            std::string sha256;
            std::string manifestJson;
        };

        // Synthesize a pick from a local-path override. Reads the
        // path's cajeta.json (must declare `details.name == name`),
        // confirms a pre-built `.cja` at the conventional location,
        // and surfaces the sidecar's bytes for transitive expansion.
        //
        // Symmetric with the GitRepository v1 limitation: a path
        // override points at a *built* package, not raw source. A
        // future enhancement can spawn a recursive `cajeta build`
        // when the artifact is missing.
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

        llvm::Expected<MvsPick> pickLowestForAll(
            const std::string& name,
            const std::vector<std::string>& constraints,
            const std::optional<std::string>& fromRepo,
            const std::vector<RepositoryPtr>& repos,
            ArtifactCache& cache) {
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

    llvm::Expected<std::vector<ResolvedDependency>> resolveMvs(
        const std::vector<DependencySpec>& deps,
        const std::vector<RepositoryPtr>& repos,
        ArtifactCache& cache,
        const std::vector<OverrideSpec>& overrides,
        ResolverTimings* timings) {

        // Pre-flight: split overrides into version / path / git forms.
        // Path overrides land here in Phase 6c; git overrides still
        // defer to the next slice.
        std::unordered_map<std::string, const OverrideSpec*> overrideMap;
        std::unordered_map<std::string, const OverrideSpec*> pathOverrideMap;
        for (const auto& o : overrides) {
            if (o.git) {
                return err("settings.overrides." + o.name +
                           ": git replacement requires Phase 6c "
                           "(next slice)");
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

        // Add a constraint + optional fromRepo for `name`. Returns
        // true when the constraint set actually changed (caller
        // marks dirty to trigger a re-pick).
        auto addConstraint = [&](const std::string& name,
                                 const std::string& constraint,
                                 const std::optional<std::string>& fromRepo,
                                 bool asDirectRoot)
            -> llvm::Expected<bool> {
            auto [it, inserted] = state.try_emplace(name);
            auto& s = it->second;
            if (inserted) {
                insertionOrder.push_back(name);
                // Wire override metadata at first sight — the same
                // override applies regardless of which BFS step
                // introduced the package.
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
            }

            bool changed = inserted;
            // Constraint set is treated as a set (dedupe by string).
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

        // Seed with the root deps. These are flagged directRoot so
        // any same-named override is ignored (per spec: direct
        // wins over override).
        for (const auto& d : deps) {
            if (d.versionConstraint.empty()) continue;  // 6c forms
            auto added = addConstraint(d.name, d.versionConstraint,
                                       d.fromRepo, /*asDirectRoot=*/true);
            if (!added) return added.takeError();
        }

        // Fixed-point loop. We pick + propagate until no package is
        // dirty. Each re-pick raises a package's chosen version,
        // bounded by the version set in the repos, so termination
        // is guaranteed. We iterate `insertionOrder` repeatedly so
        // newly-discovered packages get visited in declaration
        // order — keeps output deterministic.
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
                } else {
                    pick = pickLowestForAll(
                        name, effectiveConstraints(s), s.fromRepo,
                        repos, cache);
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
                    // Re-parse declared children. The old set is
                    // discarded — children of the prior version
                    // contributed constraints which may now be
                    // stale, but constraints once added stay; that
                    // can only over-constrain, never break
                    // correctness. (Over-constraining is the
                    // conservative MVS choice when versions
                    // change semantically — a future refinement is
                    // to drop constraints contributed by a now-
                    // superseded version.)
                    s.childDeps.clear();
                    if (!s.manifestJson.empty()) {
                        auto child = loadManifestString(
                            s.manifestJson, name + "@" + s.version);
                        if (!child) return child.takeError();
                        auto childDeps = parseDependencies(*child);
                        if (!childDeps) return childDeps.takeError();
                        s.childDeps = std::move(*childDeps);
                    }
                    // Propagate children's constraints. Each new
                    // one may mark someone dirty for the next loop
                    // iteration.
                    for (const auto& cd : s.childDeps) {
                        if (cd.versionConstraint.empty()) continue;
                        auto added = addConstraint(
                            cd.name, cd.versionConstraint, cd.fromRepo,
                            /*asDirectRoot=*/false);
                        if (!added) return added.takeError();
                        if (*added) anyDirty = true;
                    }
                    // Re-pick of `name` might have CHANGED s.dirty
                    // via cycles; preserve that.
                    if (state[name].dirty) anyDirty = true;
                }
            }
        }

        // Post-MVS audit: any package that was forced by an override
        // (and is NOT a direct root dep) gets compared against what
        // the gathered transitive constraints would have selected.
        // If the override dropped the major version below what
        // transitives needed, this is the documented hard error
        // unless allow-major-downgrade is set.
        for (const auto& name : insertionOrder) {
            const auto& s = state[name];
            bool overridden = (s.overrideConstraint || s.overridePath) &&
                              !s.directRoot;
            if (!overridden) continue;
            if (s.constraints.empty()) continue;  // override unused — nothing to compare

            // Recompute the "what would MVS have picked from the
            // transitive set alone" baseline. If that set is itself
            // unsatisfiable, the override RESCUED the build — not
            // a downgrade situation.
            auto baseline = pickLowestForAll(
                name, s.constraints, s.fromRepo, repos, cache);
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

        std::vector<ResolvedDependency> out;
        out.reserve(insertionOrder.size());
        for (const auto& name : insertionOrder) {
            const auto& s = state[name];
            ResolvedDependency r;
            r.name = name;
            r.version = s.version;
            r.resolvedFromRepo = s.resolvedFromRepo;
            r.artifactPath = s.artifactPath;
            r.sha256 = s.sha256;
            out.push_back(std::move(r));
        }
        return out;
    }

    llvm::Expected<std::vector<ResolvedDependency>>
    resolveProjectDependencies(
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
        if (deps->empty()) {
            closeTotal();
            return std::vector<ResolvedDependency>{};  // no deps → no work
        }

        auto repoSpecs = parseRepositories(m);
        if (!repoSpecs) { closeTotal(); return repoSpecs.takeError(); }
        // Deps are declared but no repos to fetch them from → hard error
        // with a pointer at the user-fixable cause.
        if (repoSpecs->empty()) {
            closeTotal();
            return err("settings.dependencies declares " +
                       std::to_string(deps->size()) +
                       " dependency(ies) but settings.repositories is "
                       "empty — add at least one repository to fetch from");
        }
        // Remote drivers stage downloads under .cajeta/cache/downloads/
        // before the ArtifactCache content-addresses them. Living
        // under the project's own .cajeta keeps interrupted fetches
        // scoped to the project — `rm -rf .cajeta` is the user's
        // escape hatch.
        std::string downloadStage =
            (std::filesystem::path(projectRoot) / ".cajeta" / "cache" /
             "downloads").string();
        auto repos = buildRepositories(*repoSpecs, downloadStage);
        if (!repos) { closeTotal(); return repos.takeError(); }

        auto overrides = parseOverrides(m);
        if (!overrides) { closeTotal(); return overrides.takeError(); }

        // When timings are requested, decorate each repo with the
        // recording wrapper. `wrapWithTimings(...,nullptr)` is a
        // no-op pass-through so the non-timed path pays nothing.
        std::vector<RepositoryPtr> repoList =
            wrapWithTimings(*repos, timings);

        ArtifactCache cache(projectRoot, homeOverride);
        auto result = resolveMvs(
            *deps, repoList, cache, *overrides, timings);
        if (result && timings) {
            timings->depsResolved = static_cast<int>(result->size());
        }
        closeTotal();
        return result;
    }

} // namespace cajeta::buildtool
