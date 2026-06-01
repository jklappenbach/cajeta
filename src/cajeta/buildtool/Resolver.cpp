#include "cajeta/buildtool/Resolver.h"

#include <llvm/Support/Error.h>

#include <algorithm>
#include <cctype>
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

    bool versionSatisfies(const std::string& version,
                          const std::string& constraint) {
        if (constraint == "*") return true;
        if (constraint.empty()) return false;

        // Wildcard form: "1.2.*", "1.*"
        auto starPos = constraint.find('*');
        if (starPos != std::string::npos) {
            std::string prefix = constraint.substr(0, starPos);
            // Strip trailing `.` from prefix for comparison.
            if (!prefix.empty() && prefix.back() == '.') {
                prefix.pop_back();
            }
            // Match "prefix" + "." + tail-of-any-segments.
            // Compare the version's leading dot-segments against
            // prefix's segments.
            auto pParts = splitDots(prefix);
            auto vParts = splitDots(semverCore(version));
            if (vParts.size() < pParts.size()) return false;
            for (size_t i = 0; i < pParts.size(); ++i) {
                if (vParts[i] != pParts[i]) return false;
            }
            return true;
        }
        // Exact match (Phase 6a only). Compare the full strings so
        // a prerelease (`1.2.3-rc1`) doesn't satisfy the
        // release-version constraint (`1.2.3`). Semver convention:
        // prereleases sort before the release and require explicit
        // opt-in to match.
        return version == constraint;
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

            // Walk repos in priority order. If `fromRepo` is set,
            // restrict to that one repo (per BuildTool.md spec).
            ResolvedDependency resolved;
            resolved.name = dep.name;
            bool found = false;
            for (const auto& repo : repos) {
                if (dep.fromRepo && repo->name() != *dep.fromRepo) continue;

                auto versions = repo->listVersions(dep.name);
                if (!versions) return versions.takeError();
                std::string version = highestSatisfying(
                    *versions, dep.versionConstraint);
                if (version.empty()) continue;  // try next repo

                auto path = repo->fetch(dep.name, version);
                if (!path) return path.takeError();

                // Pull into the content-addressed cache.
                auto cached = cache.insert(*path);
                if (!cached) return cached.takeError();

                resolved.version = version;
                resolved.resolvedFromRepo = repo->name();
                resolved.artifactPath = *cached;
                resolved.sha256 = ArtifactCache::sha256OfFile(*cached);
                found = true;
                break;
            }
            if (!found) {
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
            out.push_back(std::move(resolved));
        }
        return out;
    }

} // namespace cajeta::buildtool
