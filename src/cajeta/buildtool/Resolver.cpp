#include "cajeta/buildtool/Resolver.h"

#include <llvm/Support/Error.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <set>
#include <sstream>
#include <string>
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

        // Resolve one dependency against the priority-ordered repos.
        // Honors the `from` pin; picks the highest version satisfying
        // `dep.versionConstraint`. Pulls the picked artifact into
        // the content-addressed cache. Returns a fully populated
        // ResolvedDependency on success.
        //
        // Also fetches the dep's sidecar `cajeta.json` (when the
        // winning repository can produce one) into
        // `manifestJsonOut` — the transitive walker uses that to
        // recurse without re-fetching. `manifestJsonOut` is left
        // empty when no sidecar is available.
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

                // Pull the sidecar manifest while we still know which
                // repo won — saves the walker an extra round trip.
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

    llvm::Expected<std::vector<ResolvedDependency>> resolveTransitive(
        const std::vector<DependencySpec>& deps,
        const std::vector<RepositoryPtr>& repos,
        ArtifactCache& cache) {

        // BFS walk. Frontier holds dep specs we still need to
        // resolve; `seen` tracks package names we've already
        // resolved (first-pick wins — when the MVS solver lands,
        // it'll re-pick on conflict). Output order is topological:
        // root deps in declaration order first, then each picked
        // dep's children in declaration order.
        std::vector<ResolvedDependency> out;
        std::set<std::string> seen;
        std::deque<DependencySpec> frontier;
        for (const auto& d : deps) frontier.push_back(d);

        while (!frontier.empty()) {
            DependencySpec dep = frontier.front();
            frontier.pop_front();

            if (dep.versionConstraint.empty()) {
                // 6c source-form (path / git). Skip — same contract
                // as resolveDirect.
                continue;
            }
            if (seen.count(dep.name)) {
                // First-pick wins under the highest-satisfying
                // policy. The MVS solver will replace this with
                // a constraint-intersection re-pick.
                continue;
            }
            seen.insert(dep.name);

            std::string manifestJson;
            auto r = resolveOne(dep, repos, cache, manifestJson);
            if (!r) return r.takeError();
            out.push_back(std::move(*r));

            if (manifestJson.empty()) {
                // No sidecar — treat as a leaf. Pre-sidecar
                // artifacts silently fall through here; once they
                // republish with a sidecar their transitive deps
                // expand without any consumer-side change.
                continue;
            }
            auto child = loadManifestString(
                manifestJson, dep.name + "@" + out.back().version);
            if (!child) return child.takeError();
            auto childDeps = parseDependencies(*child);
            if (!childDeps) return childDeps.takeError();
            for (const auto& cd : *childDeps) {
                if (!seen.count(cd.name)) {
                    frontier.push_back(cd);
                }
            }
        }
        return out;
    }

} // namespace cajeta::buildtool
