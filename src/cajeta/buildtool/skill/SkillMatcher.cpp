//
// Fuzzy skill matcher. See SkillMatcher.h and
// specs/archive/skill-discovery-spec.md §3.5.
//
#include "cajeta/buildtool/skill/SkillMatcher.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <vector>

namespace cajeta::buildtool::skill {

    namespace {

        constexpr int kInf = std::numeric_limits<int>::max();

        // Optimal string alignment distance (Damerau–Levenshtein restricted to
        // adjacent transpositions, each cost 1).
        int osa(llvm::StringRef a, llvm::StringRef b) {
            const size_t n = a.size(), m = b.size();
            if (n == 0) return static_cast<int>(m);
            if (m == 0) return static_cast<int>(n);
            std::vector<int> prev2(m + 1, 0), prev(m + 1), cur(m + 1);
            for (size_t j = 0; j <= m; ++j) prev[j] = static_cast<int>(j);
            for (size_t i = 1; i <= n; ++i) {
                cur[0] = static_cast<int>(i);
                for (size_t j = 1; j <= m; ++j) {
                    int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
                    int del = prev[j] + 1;
                    int ins = cur[j - 1] + 1;
                    int sub = prev[j - 1] + cost;
                    int best = std::min({del, ins, sub});
                    if (i > 1 && j > 1 && a[i - 1] == b[j - 2] &&
                        a[i - 2] == b[j - 1]) {
                        best = std::min(best, prev2[j - 2] + 1);
                    }
                    cur[j] = best;
                }
                prev2 = prev;
                prev = cur;
            }
            return prev[m];
        }

        std::string lower(llvm::StringRef s) {
            std::string o;
            o.reserve(s.size());
            for (char c : s)
                o.push_back(static_cast<char>(std::tolower((unsigned char)c)));
            return o;
        }

        // Split a canonical name into segments on '/' and '.'.
        std::vector<llvm::StringRef> segments(llvm::StringRef name) {
            std::vector<llvm::StringRef> out;
            size_t start = 0;
            for (size_t i = 0; i <= name.size(); ++i) {
                if (i == name.size() || name[i] == '/' || name[i] == '.') {
                    out.push_back(name.substr(start, i - start));
                    start = i + 1;
                }
            }
            return out;
        }

        std::vector<std::string> tokens(llvm::StringRef s) {
            std::vector<std::string> out;
            size_t i = 0;
            while (i < s.size()) {
                while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
                size_t start = i;
                while (i < s.size() && !std::isspace((unsigned char)s[i])) ++i;
                if (i > start) out.push_back(lower(s.substr(start, i - start)));
            }
            return out;
        }

        // Per-segment edit allowance, scaled by the key segment's length.
        int segAllowance(size_t len) {
            if (len <= 2) return 0;
            if (len <= 5) return 1;
            if (len <= 9) return 2;
            return static_cast<int>(len / 4);
        }

        // Length-scaled allowance for a whole title.
        int titleAllowance(size_t len) {
            return std::max<int>(1, static_cast<int>(len / 5));
        }

        // Segment-aware name distance: kInf when the segment counts differ or any
        // segment exceeds its own allowance (so a typo stays local and structure
        // is respected). Otherwise the summed per-segment OSA distance.
        int nameDistance(llvm::StringRef query, llvm::StringRef key) {
            auto qs = segments(query);
            auto ks = segments(key);
            if (qs.size() != ks.size()) return kInf;
            int total = 0;
            for (size_t i = 0; i < ks.size(); ++i) {
                int d = osa(qs[i], ks[i]);
                if (d > segAllowance(ks[i].size())) return kInf;
                total += d;
            }
            return total;
        }

        // Title distance: token-wise OSA (case-insensitive) when token counts
        // match, else whole-string OSA on the lowercased text. kInf when beyond
        // the length-scaled allowance.
        int titleDistance(llvm::StringRef query, llvm::StringRef key) {
            auto qt = tokens(query);
            auto kt = tokens(key);
            int dist;
            if (qt.size() == kt.size() && !qt.empty()) {
                dist = 0;
                for (size_t i = 0; i < kt.size(); ++i) dist += osa(qt[i], kt[i]);
            } else {
                dist = osa(lower(query), lower(key));
            }
            return dist > titleAllowance(key.size()) ? kInf : dist;
        }

    } // namespace

    std::vector<SkillMatch>
    matchSkills(llvm::StringRef query, const SkillIndex& index, MatchOptions opts) {
        std::vector<SkillMatch> out;
        for (const SkillCandidate& c : index.candidates(query)) {
            int d = (c.source == MatchSource::Name)
                        ? nameDistance(query, c.key)
                        : titleDistance(query, c.key);
            if (d == kInf) continue;
            if (opts.exact && d != 0) continue;
            out.push_back({c.key, c.source, c.ids, d});
        }

        std::sort(out.begin(), out.end(), [](const SkillMatch& a, const SkillMatch& b) {
            if (a.distance != b.distance) return a.distance < b.distance;
            if (a.source != b.source) return a.source == MatchSource::Name;
            return a.key < b.key;
        });
        return out;
    }

} // namespace cajeta::buildtool::skill
