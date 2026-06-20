//
// Skill Search core. See SkillSearch.h and
// docs/specs/skill-discovery-spec.md §3.2–§3.5.
//
#include "cajeta/buildtool/skill/SkillSearch.h"

#include <algorithm>
#include <map>
#include <set>

namespace cajeta::buildtool::skill {

    namespace {

        std::string makeUri(llvm::StringRef library, llvm::StringRef version,
                            llvm::StringRef id) {
            return ("cja-skill://" + library + "@" + version + "/" + id).str();
        }

        // Ids of the nearest ancestor (by stripping trailing '/'/'.' segments)
        // that has any bound skill — the "overview" surfaced for a deep query
        // (spec §3.2). Empty when no ancestor has a skill.
        std::vector<std::string> ancestorOverviewIds(const SkillIndex& idx,
                                                     llvm::StringRef name) {
            std::string cur = name.str();
            while (true) {
                size_t cut = cur.find_last_of("/.");
                if (cut == std::string::npos) return {};
                cur = cur.substr(0, cut);
                auto ids = idx.query(cur, false);
                if (!ids.empty()) return ids;
            }
        }

        // Should archive `a` be included given the version / from selection?
        bool includeArchive(const ResolvedSkillArchive& a,
                            const std::optional<std::string>& version,
                            const std::optional<std::string>& from,
                            const SkillSearchContext& ctx) {
            if (version) {
                return a.version == *version; // explicit version overrides from
            }
            if (from) {
                auto mod = ctx.moduleVersions.find(*from);
                if (mod == ctx.moduleVersions.end()) return false;
                auto lib = mod->second.find(a.library);
                if (lib == mod->second.end()) return false; // module can't see it
                return a.version == lib->second;
            }
            return true; // diamond: every resolved version, tagged
        }

        // "Better" = should win a URI-dedup tie: lower distance, then lower tier,
        // then a name match over a title match.
        bool better(const SkillSearchResult& x, const SkillSearchResult& y) {
            if (x.distance != y.distance) return x.distance < y.distance;
            if (x.tier != y.tier) return x.tier < y.tier;
            return x.source == MatchSource::Name && y.source == MatchSource::Title;
        }

    } // namespace

    std::vector<SkillSearchResult> searchSkills(
        llvm::StringRef name, std::optional<std::string> version,
        std::optional<std::string> from, const SkillSearchContext& ctx,
        MatchOptions opts) {

        std::map<std::string, SkillSearchResult> byUri; // dedup, keep best

        auto consider = [&](SkillSearchResult r) {
            auto it = byUri.find(r.uri);
            if (it == byUri.end() || better(r, it->second)) {
                byUri[r.uri] = std::move(r);
            }
        };

        for (const ResolvedSkillArchive& a : ctx.archives) {
            if (!includeArchive(a, version, from, ctx)) continue;

            for (const SkillMatch& m : matchSkills(name, a.index, opts)) {
                if (m.source == MatchSource::Name) {
                    const std::string& mName = m.key;
                    std::set<std::string> exact;
                    for (const auto& id : a.index.query(mName, false)) {
                        exact.insert(id);
                        consider({makeUri(a.library, a.version, id), mName,
                                  MatchSource::Name, MatchTier::Exact, m.distance});
                    }
                    for (const auto& id : a.index.query(mName, true)) {
                        if (exact.count(id)) continue;
                        consider({makeUri(a.library, a.version, id), mName,
                                  MatchSource::Name, MatchTier::Descendant,
                                  m.distance});
                    }
                    for (const auto& id : ancestorOverviewIds(a.index, mName)) {
                        consider({makeUri(a.library, a.version, id), mName,
                                  MatchSource::Name, MatchTier::AncestorOverview,
                                  m.distance});
                    }
                } else {
                    for (const auto& id : m.ids) {
                        auto names = a.index.namesForId(id);
                        std::string mName = names.empty() ? m.key : names.front();
                        consider({makeUri(a.library, a.version, id), mName,
                                  MatchSource::Title, MatchTier::Exact, m.distance});
                    }
                }
            }
        }

        std::vector<SkillSearchResult> out;
        out.reserve(byUri.size());
        for (auto& [uri, r] : byUri) out.push_back(std::move(r));

        std::sort(out.begin(), out.end(),
                  [](const SkillSearchResult& a, const SkillSearchResult& b) {
                      if (a.distance != b.distance) return a.distance < b.distance;
                      if (a.tier != b.tier) return a.tier < b.tier;
                      if (a.source != b.source)
                          return a.source == MatchSource::Name;
                      return a.uri < b.uri;
                  });
        return out;
    }

    std::vector<SkillListEntry> listSkills(
        std::optional<std::string> scope, std::optional<std::string> version,
        std::optional<std::string> from, const SkillSearchContext& ctx) {

        std::vector<SkillListEntry> out;
        for (const ResolvedSkillArchive& a : ctx.archives) {
            if (!includeArchive(a, version, from, ctx)) continue;

            // Scope is an exact, prefix-inclusive subtree (query, not fuzzy);
            // no scope enumerates the whole archive.
            std::vector<std::string> ids =
                scope ? a.index.query(*scope, /*hierarchical=*/true)
                      : a.index.allIds();

            for (const std::string& id : ids) {
                const SkillEntry* e = a.index.entry(id);
                out.push_back({makeUri(a.library, a.version, id),
                               a.index.namesForId(id),
                               e ? e->title : std::string()});
            }
        }

        std::sort(out.begin(), out.end(),
                  [](const SkillListEntry& a, const SkillListEntry& b) {
                      return a.uri < b.uri;
                  });
        return out;
    }

} // namespace cajeta::buildtool::skill
