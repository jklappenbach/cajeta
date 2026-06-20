//
// Skill Search core (skill-discovery spec §3.2–§3.5). Transport-agnostic: takes
// the resolved set of skill archives + optional version / consumer-scoping, runs
// the fuzzy matcher (D.4a) per archive, expands hierarchically (exact ∪
// descendants ∪ nearest-ancestor overview), and returns ranked, version-tagged
// `cja-skill://` URIs — no CLI types, no I/O, no network. An MCP/CLI adapter
// wraps this.
//
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>

#include "cajeta/buildtool/skill/SkillIndex.h"
#include "cajeta/buildtool/skill/SkillMatcher.h"

namespace cajeta::buildtool::skill {

    // One resolved library archive's skill data, as Search sees it. A diamond
    // contributes two entries with the same `library` and different `version`.
    struct ResolvedSkillArchive {
        std::string library; // coordinate, e.g. "cajeta.io"
        std::string version; // resolved version, e.g. "1.4.2"
        SkillIndex index;
    };

    // The inputs Search assembles over (built from the lockfile + resolved
    // archives by the caller — offline).
    struct SkillSearchContext {
        std::vector<ResolvedSkillArchive> archives;
        // Consumer scoping: module id → (library → resolved version), so `from`
        // can infer the version the asking module sees (spec §3.4). The module
        // identifier form is opaque to Search.
        std::map<std::string, std::map<std::string, std::string>> moduleVersions;
    };

    // Why a result surfaced, for ranking (spec §3.2: exact → descendant →
    // ancestor).
    enum class MatchTier { Exact = 0, Descendant = 1, AncestorOverview = 2 };

    struct SkillSearchResult {
        std::string uri;         // cja-skill://<library>@<version>/<id>
        std::string matchedName; // canonical name the query resolved to (§3.5)
        MatchSource source;      // matched via a name or a title
        MatchTier tier;
        int distance;            // fuzzy edit distance (0 = exact)
    };

    // Search for skills aiding `name`.
    //   `version` — restrict to that resolved version of the owning library.
    //   `from`    — infer the version from the asking module's resolution;
    //               `version` overrides `from`; with neither, every resolved
    //               version matches (diamond → all, version-tagged). No silent
    //               pick.
    //   `opts.exact` — disable fuzzy matching.
    // Results are deduped by URI and ranked: distance, then tier, then name
    // before title, then URI. Empty when nothing matches within threshold.
    std::vector<SkillSearchResult> searchSkills(
        llvm::StringRef name,
        std::optional<std::string> version,
        std::optional<std::string> from,
        const SkillSearchContext& ctx,
        MatchOptions opts = {});

    // One enumerated skill (spec §3.6): its URI, the canonical names it binds,
    // and its title.
    struct SkillListEntry {
        std::string uri;
        std::vector<std::string> names;
        std::string title;
    };

    // Enumerate skills in the resolved set (spec §3.6). With no `scope`, lists
    // every skill; with a library/package `scope`, only that subtree
    // (prefix-inclusive, §3.2 — exact, NOT fuzzy). `version`/`from` select
    // versions exactly as Search does (multi-version → version-tagged entries).
    // Deterministic ordering (by URI).
    std::vector<SkillListEntry> listSkills(
        std::optional<std::string> scope,
        std::optional<std::string> version,
        std::optional<std::string> from,
        const SkillSearchContext& ctx);

} // namespace cajeta::buildtool::skill
