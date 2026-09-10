// Fuzzy skill matcher (skill-discovery spec §3.5): a trigram prefilter narrows the key
// set, then Damerau–Levenshtein scores survivors under a length-scaled threshold.
#pragma once

#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>

#include "cajeta/buildtool/skill/SkillIndex.h"

namespace cajeta::buildtool::skill {

    // One ranked match: the key, where it came from, the skill ids it resolves to, and the edit distance (0 = exact).
    struct SkillMatch {
        std::string key;
        MatchSource source;
        std::vector<std::string> ids;
        int distance = 0;
    };

    struct MatchOptions {
        bool exact = false;
    };

    // Ranks index keys by closeness to `query`: exact first, then ascending distance,
    // Name before Title, then lexicographic. Empty when nothing is within threshold.
    std::vector<SkillMatch>
    matchSkills(llvm::StringRef query, const SkillIndex& index, MatchOptions opts = {});

} // namespace cajeta::buildtool::skill
