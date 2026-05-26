#include "Diagnostics.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace cajeta {

    int levenshteinDistance(const std::string& a, const std::string& b) {
        // Two-row rolling DP keeps space at O(min(|a|, |b|)). We swap
        // so the "inner" string is the shorter one, since the row width
        // is |b| + 1.
        if (a.size() < b.size()) {
            return levenshteinDistance(b, a);
        }
        if (b.empty()) return static_cast<int>(a.size());

        std::vector<int> prev(b.size() + 1);
        std::vector<int> curr(b.size() + 1);
        for (std::size_t j = 0; j <= b.size(); ++j) {
            prev[j] = static_cast<int>(j);
        }
        for (std::size_t i = 1; i <= a.size(); ++i) {
            curr[0] = static_cast<int>(i);
            for (std::size_t j = 1; j <= b.size(); ++j) {
                int substCost = (a[i - 1] == b[j - 1]) ? 0 : 1;
                int del  = prev[j] + 1;
                int ins  = curr[j - 1] + 1;
                int subs = prev[j - 1] + substCost;
                curr[j] = std::min(std::min(del, ins), subs);
            }
            std::swap(prev, curr);
        }
        return prev[b.size()];
    }

    std::vector<std::string> pickSimilar(
            const std::string& target,
            const std::vector<std::string>& candidates,
            int maxDistance,
            std::size_t maxSuggestions) {
        // Score every candidate. Skip exact matches — there's nothing
        // to suggest if the name already equals the target (that's
        // not a typo, it's a definition-order or scoping issue and
        // the caller should not have surfaced "did you mean" in
        // the first place).
        std::vector<std::pair<int, std::string>> scored;
        scored.reserve(candidates.size());
        for (auto& c : candidates) {
            if (c == target) continue;
            int d = levenshteinDistance(target, c);
            if (d <= maxDistance) {
                scored.emplace_back(d, c);
            }
        }
        // Sort by distance ascending, then by name ascending so ties
        // are deterministic across runs (load-bearing for the test
        // assertions on exact error strings).
        std::sort(scored.begin(), scored.end(),
            [](const std::pair<int, std::string>& l,
               const std::pair<int, std::string>& r) {
                if (l.first != r.first) return l.first < r.first;
                return l.second < r.second;
            });
        std::vector<std::string> out;
        out.reserve(std::min(scored.size(), maxSuggestions));
        for (auto& s : scored) {
            if (out.size() >= maxSuggestions) break;
            out.push_back(s.second);
        }
        return out;
    }

    std::string formatDidYouMean(const std::vector<std::string>& suggestions) {
        if (suggestions.empty()) return "";
        if (suggestions.size() == 1) {
            return " did you mean `" + suggestions[0] + "`?";
        }
        std::string out = " did you mean one of: ";
        for (std::size_t i = 0; i < suggestions.size(); ++i) {
            if (i > 0) out += ", ";
            out += "`";
            out += suggestions[i];
            out += "`";
        }
        out += "?";
        return out;
    }

} // namespace cajeta
