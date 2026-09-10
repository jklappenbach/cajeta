// Skill Search core. Transport-agnostic: over a resolved set of skill archives it
// runs the fuzzy matcher, expands hierarchically, and returns ranked, version-
// tagged `cja-skill://` URIs. No CLI types, no I/O, no network — an adapter wraps it.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>

#include "cajeta/buildtool/skill/SkillIndex.h"
#include "cajeta/buildtool/skill/SkillMatcher.h"

namespace cajeta::buildtool::skill {

    // One archive's skills; a diamond contributes two entries, one per version.
    struct ResolvedSkillArchive {
        std::string library; // coordinate, e.g. "cajeta.io"
        std::string version; // resolved version, e.g. "1.4.2"
        SkillIndex index;
    };

    struct SkillSearchContext {
        std::vector<ResolvedSkillArchive> archives;
        // module id -> (library -> resolved version); the id's form is opaque here.
        std::map<std::string, std::map<std::string, std::string>> moduleVersions;
    };

    enum class MatchTier { Exact = 0, Descendant = 1, AncestorOverview = 2 };

    struct SkillSearchResult {
        std::string uri;         // cja-skill://<library>@<version>/<id>
        std::string matchedName; // canonical name the query resolved to (§3.5)
        MatchSource source;      // matched via a name or a title
        MatchTier tier;
        int distance;            // fuzzy edit distance (0 = exact)
    };

    // Skills aiding `name`. `version` pins the owning library's version and
    // overrides `from`, which infers it from the asking module; with neither, EVERY
    // resolved version matches. Deduped by URI, ranked by distance then tier.
    std::vector<SkillSearchResult> searchSkills(
        llvm::StringRef name,
        std::optional<std::string> version,
        std::optional<std::string> from,
        const SkillSearchContext& ctx,
        MatchOptions opts = {});

    struct SkillListEntry {
        std::string uri;
        std::vector<std::string> names;
        std::string title;
    };

    // Enumerate the resolved set, or with a `scope` only that subtree, matched
    // prefix-inclusive and EXACT rather than fuzzy. `version` and `from` select
    // versions as Search does; ordering is deterministic, by URI.
    std::vector<SkillListEntry> listSkills(
        std::optional<std::string> scope,
        std::optional<std::string> version,
        std::optional<std::string> from,
        const SkillSearchContext& ctx);

} // namespace cajeta::buildtool::skill
