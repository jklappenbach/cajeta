#include "cajeta/prof/ProfileSelection.h"

#include <algorithm>
#include <cctype>

namespace cajeta::prof {

    namespace {
        std::string trim(const std::string& s) {
            size_t b = 0, e = s.size();
            while (b < e && std::isspace((unsigned char) s[b])) ++b;
            while (e > b && std::isspace((unsigned char) s[e - 1])) --e;
            return s.substr(b, e - b);
        }

        // Recursive glob. `**` consumes any run of characters; `*` consumes any
        // run that contains no `.`. Both may match empty. Depth is bounded by
        // the number of wildcards in the pattern, which a human wrote.
        bool globAt(const std::string& p, size_t pi,
                    const std::string& n, size_t ni) {
            while (pi < p.size()) {
                if (p[pi] == '*') {
                    const bool crossesDots = (pi + 1 < p.size() && p[pi + 1] == '*');
                    const size_t next = pi + (crossesDots ? 2 : 1);
                    // Try every split, shortest first.
                    for (size_t k = ni; k <= n.size(); ++k) {
                        if (globAt(p, next, n, k)) return true;
                        if (k < n.size() && !crossesDots && n[k] == '.') break;
                    }
                    return false;
                }
                if (ni >= n.size() || n[ni] != p[pi]) return false;
                ++pi;
                ++ni;
            }
            return ni == n.size();
        }
    }

    bool ProfileSelection::matches(const std::string& pattern,
                                   const std::string& name) {
        return globAt(pattern, 0, name, 0);
    }

    ProfileSelection ProfileSelection::parse(const std::string& text,
                                             std::vector<std::string>* errors) {
        ProfileSelection sel;
        size_t pos = 0;
        int lineNo = 0;
        while (pos <= text.size()) {
            size_t nl = text.find('\n', pos);
            std::string line = text.substr(
                pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
            ++lineNo;

            const size_t hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            line = trim(line);
            if (line.empty()) continue;

            bool exclude = false;
            std::string pattern = line;
            const size_t sp = line.find_first_of(" \t");
            if (sp != std::string::npos) {
                const std::string kw = line.substr(0, sp);
                const std::string rest = trim(line.substr(sp));
                if (kw == "include" || kw == "exclude") {
                    exclude = (kw == "exclude");
                    pattern = rest;
                } else if (errors) {
                    // Two words and the first is not a keyword: almost
                    // certainly a typo. Refusing it is the safe direction —
                    // silently treating "inlcude a.b" as the pattern
                    // "inlcude a.b" would match nothing and read as a bug in
                    // the profiler rather than in the file.
                    errors->push_back(
                        "profiler selection line " + std::to_string(lineNo) +
                        ": expected `include` or `exclude`, got `" + kw + "`");
                    continue;
                } else {
                    continue;
                }
            }
            if (pattern.empty()) {
                if (errors)
                    errors->push_back(
                        "profiler selection line " + std::to_string(lineNo) +
                        ": " + (exclude ? "exclude" : "include") +
                        " with no pattern");
                continue;
            }
            (exclude ? sel.exc : sel.inc).push_back(pattern);
        }
        return sel;
    }

    bool ProfileSelection::selects(const std::string& canonicalClassName) const {
        if (!inc.empty()) {
            bool in = false;
            for (const auto& p : inc) {
                if (matches(p, canonicalClassName)) { in = true; break; }
            }
            if (!in) return false;
        }
        for (const auto& p : exc) {
            if (matches(p, canonicalClassName)) return false;
        }
        return true;
    }

    std::string ProfileSelection::describe() const {
        auto canon = [](std::vector<std::string> v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
            return v;
        };
        std::string out;
        auto append = [&out](const char* kw, const std::vector<std::string>& v) {
            for (const auto& p : v) {
                if (!out.empty()) out += "; ";
                out += kw;
                out += ' ';
                out += p;
            }
        };
        append("include", canon(inc));
        append("exclude", canon(exc));
        return out.empty() ? std::string("all") : out;
    }

} // namespace cajeta::prof
