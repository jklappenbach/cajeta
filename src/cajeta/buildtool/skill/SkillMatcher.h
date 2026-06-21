//
// Fuzzy skill matcher (skill-discovery spec §3.5) — the technology core of
// Search. Typo-tolerant resolution of a query to index keys (canonical names
// AND titles): a trigram prefilter (SkillIndex::candidates, D.2) narrows the key
// set, then Damerau–Levenshtein (optimal string alignment, so adjacent
// transpositions cost 1) scores survivors — per segment for names, token-wise
// for titles — under a length-scaled threshold. Pure function of (query, index).
//
#pragma once

#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>

#include "cajeta/buildtool/skill/SkillIndex.h"

namespace cajeta::buildtool::skill {

    // One ranked match: the key it matched, where it came from, the skill ids it
    // resolves to, and the edit distance (0 = exact).
    struct SkillMatch {
        std::string key;
        MatchSource source;
        std::vector<std::string> ids;
        int distance = 0;
    };

    struct MatchOptions {
        // Bypass fuzzy matching — accept only exact (distance 0) keys.
        bool exact = false;
    };

    // Rank index keys by closeness to `query`. Results are ordered exact-first,
    // then by ascending distance, then Name before Title, then key lexicographic
    // (a deterministic total order). Returns empty when nothing is within the
    // length-scaled threshold.
    std::vector<SkillMatch>
    matchSkills(llvm::StringRef query, const SkillIndex& index, MatchOptions opts = {});

} // namespace cajeta::buildtool::skill
