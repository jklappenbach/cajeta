#include "cajeta/buildtool/CoverageReport.h"

#include <llvm/Support/Error.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        std::string trim(const std::string& s) {
            size_t b = 0; while (b < s.size() && std::isspace(s[b])) ++b;
            size_t e = s.size();
            while (e > b && std::isspace(s[e - 1])) --e;
            return s.substr(b, e - b);
        }

        // Glob match supporting `*` (any non-slash) and `**` (any).
        // Plain substrings match literally.
        bool globMatch(const std::string& pattern,
                       const std::string& text) {
            // Iterative two-pointer with star-backtrack.
            size_t pi = 0, ti = 0;
            size_t starPi = std::string::npos, starTi = 0;
            while (ti < text.size()) {
                if (pi < pattern.size()) {
                    if (pattern[pi] == '*') {
                        bool doubled = (pi + 1 < pattern.size() &&
                                        pattern[pi + 1] == '*');
                        starPi = doubled ? pi + 2 : pi + 1;
                        starTi = ti;
                        pi = starPi;
                        // `**` lets us cross '/'; `*` doesn't.
                        // Encode by remembering whether the star
                        // we just consumed was doubled.
                        // (Simpler: when not doubled and next char
                        // is '/' we must stop expansion at '/'.)
                        // For pragmatism we treat both as "match any
                        // run including /" here — the cajeta build
                        // tool uses exclude lists primarily for
                        // single-segment patterns + `**/` prefixes;
                        // strict POSIX glob semantics aren't
                        // necessary for the v1 scope.
                        (void)doubled;
                        continue;
                    }
                    if (pattern[pi] == text[ti]) {
                        ++pi; ++ti; continue;
                    }
                }
                if (starPi != std::string::npos) {
                    pi = starPi;
                    ++starTi;
                    ti = starTi;
                    continue;
                }
                return false;
            }
            while (pi < pattern.size() && pattern[pi] == '*') ++pi;
            return pi == pattern.size();
        }

        std::string htmlEscape(const std::string& s) {
            std::string out;
            out.reserve(s.size());
            for (char c : s) {
                switch (c) {
                    case '<': out += "&lt;"; break;
                    case '>': out += "&gt;"; break;
                    case '&': out += "&amp;"; break;
                    case '"': out += "&quot;"; break;
                    default:  out += c; break;
                }
            }
            return out;
        }

        std::string jsonEscape(const std::string& s) {
            std::string out;
            out.reserve(s.size());
            for (char c : s) {
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:   out += c;      break;
                }
            }
            return out;
        }

        std::string formatPercent(double p) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f", p);
            return buf;
        }

    } // namespace

    llvm::Expected<CoverageMap> parseCoverageMap(const std::string& text) {
        CoverageMap m;
        m.grain = "line";  // default if no header comment
        std::stringstream ss(text);
        std::string line;
        int lineNo = 0;
        while (std::getline(ss, line)) {
            ++lineNo;
            std::string t = trim(line);
            if (t.empty()) continue;
            if (t[0] == '#') {
                // Header comment may carry grain hint.
                auto p = t.find("grain=");
                if (p != std::string::npos) {
                    m.grain = t.substr(p + 6);
                    // Strip trailing whitespace from grain.
                    size_t end = m.grain.find(' ');
                    if (end != std::string::npos) {
                        m.grain = m.grain.substr(0, end);
                    }
                }
                continue;
            }
            std::istringstream ls(t);
            CoverageFile cf;
            ls >> cf.path >> cf.covered >> cf.total;
            if (ls.fail() || cf.path.empty()) {
                return err("coverage-map: malformed entry on line " +
                           std::to_string(lineNo) + ": '" + t + "'");
            }
            if (cf.total < 0 || cf.covered < 0 || cf.covered > cf.total) {
                return err("coverage-map: line " +
                           std::to_string(lineNo) +
                           " counters out of range (covered " +
                           std::to_string(cf.covered) + ", total " +
                           std::to_string(cf.total) + ")");
            }
            m.files.push_back(std::move(cf));
        }
        return m;
    }

    CoverageMap applyExcludes(const CoverageMap& m,
                              const std::vector<std::string>& patterns) {
        if (patterns.empty()) return m;
        CoverageMap out;
        out.grain = m.grain;
        for (const auto& f : m.files) {
            bool excluded = false;
            for (const auto& p : patterns) {
                if (globMatch(p, f.path)) { excluded = true; break; }
            }
            if (!excluded) out.files.push_back(f);
        }
        return out;
    }

    double overallPercent(const CoverageMap& m) {
        int64_t cov = 0, tot = 0;
        for (const auto& f : m.files) {
            cov += f.covered;
            tot += f.total;
        }
        return tot == 0 ? 100.0
                        : 100.0 * static_cast<double>(cov) /
                                 static_cast<double>(tot);
    }

    std::vector<CoverageFile> bottomN(const CoverageMap& m, size_t n) {
        std::vector<CoverageFile> sorted = m.files;
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const CoverageFile& a, const CoverageFile& b) {
                             return a.percent() < b.percent();
                         });
        if (sorted.size() > n) sorted.resize(n);
        return sorted;
    }

    std::string consoleSummary(const CoverageMap& m) {
        int64_t cov = 0, tot = 0;
        for (const auto& f : m.files) {
            cov += f.covered;
            tot += f.total;
        }
        std::string s = "coverage: " + formatPercent(overallPercent(m)) +
                        "% (" + std::to_string(cov) + "/" +
                        std::to_string(tot) + " over " +
                        std::to_string(m.files.size()) +
                        " files, grain=" + m.grain + ")";
        return s;
    }

    ThresholdResult checkThresholds(const CoverageMap& m,
                                    double minOverall,
                                    double minPerFile,
                                    size_t bottomNSize) {
        ThresholdResult r;
        double overall = overallPercent(m);
        bool overallViolation =
            minOverall >= 0 && overall < minOverall;
        std::vector<CoverageFile> belowFloor;
        if (minPerFile >= 0) {
            for (const auto& f : m.files) {
                if (f.total == 0) continue;
                if (f.percent() < minPerFile) belowFloor.push_back(f);
            }
        }
        if (!overallViolation && belowFloor.empty()) return r;

        r.violated = true;
        std::ostringstream os;
        os << "coverage gate failed:\n";
        if (overallViolation) {
            os << "  overall " << formatPercent(overall)
               << "% < min " << formatPercent(minOverall) << "%\n";
            auto bn = bottomN(m, bottomNSize);
            if (!bn.empty()) {
                os << "  bottom " << bn.size() << " files:\n";
                for (const auto& f : bn) {
                    os << "    " << f.path << "  "
                       << formatPercent(f.percent()) << "% ("
                       << f.covered << "/" << f.total << ")\n";
                }
            }
        }
        if (!belowFloor.empty()) {
            os << "  files below per-file floor "
               << formatPercent(minPerFile) << "%:\n";
            std::stable_sort(belowFloor.begin(), belowFloor.end(),
                             [](const CoverageFile& a, const CoverageFile& b) {
                                 return a.percent() < b.percent();
                             });
            size_t cap = std::min(belowFloor.size(), bottomNSize);
            for (size_t i = 0; i < cap; ++i) {
                const auto& f = belowFloor[i];
                os << "    " << f.path << "  "
                   << formatPercent(f.percent()) << "% ("
                   << f.covered << "/" << f.total << ")\n";
            }
        }
        r.detail = os.str();
        return r;
    }

    namespace {

        llvm::Error writeHtml(const std::string& path,
                              const CoverageMap& m) {
            std::ofstream o(path, std::ios::binary | std::ios::trunc);
            if (!o) return err("cannot open " + path + " for writing");
            o << "<!doctype html><html><head><meta charset=\"utf-8\">"
                 "<title>cajeta coverage</title>"
                 "<style>body{font-family:sans-serif}"
                 "table{border-collapse:collapse}"
                 "th,td{padding:4px 8px;border:1px solid #ddd;"
                 "text-align:left}"
                 "th{background:#f5f5f5}"
                 ".low{background:#fee}.mid{background:#ffd}"
                 ".high{background:#efe}</style></head><body>";
            o << "<h1>cajeta coverage — " << htmlEscape(m.grain)
              << " grain</h1>";
            o << "<p>overall: " << formatPercent(overallPercent(m))
              << "%</p>";
            o << "<table><thead><tr><th>file</th><th>percent</th>"
                 "<th>covered/total</th></tr></thead><tbody>";
            for (const auto& f : m.files) {
                double p = f.percent();
                const char* cls = p < 50 ? "low" :
                                  p < 80 ? "mid" : "high";
                o << "<tr class=\"" << cls << "\">"
                  << "<td>" << htmlEscape(f.path) << "</td>"
                  << "<td>" << formatPercent(p) << "%</td>"
                  << "<td>" << f.covered << "/" << f.total << "</td>"
                  << "</tr>";
            }
            o << "</tbody></table></body></html>";
            return llvm::Error::success();
        }

        llvm::Error writeSarif(const std::string& path,
                               const CoverageMap& m) {
            std::ofstream o(path, std::ios::binary | std::ios::trunc);
            if (!o) return err("cannot open " + path + " for writing");
            o << "{\n";
            o << "  \"version\": \"2.1.0\",\n";
            o << "  \"$schema\": \"https://raw.githubusercontent.com/"
                 "oasis-tcs/sarif-spec/master/Schemata/"
                 "sarif-schema-2.1.0.json\",\n";
            o << "  \"runs\": [{\n";
            o << "    \"tool\": { \"driver\": { "
                 "\"name\": \"cajeta.coverage\", "
                 "\"informationUri\": \"https://cajeta.org\" }},\n";
            o << "    \"properties\": { "
                 "\"overall-percent\": "
              << formatPercent(overallPercent(m)) << ", "
              << "\"grain\": \"" << jsonEscape(m.grain) << "\" "
              << "},\n";
            o << "    \"results\": [";
            bool first = true;
            for (const auto& f : m.files) {
                if (f.percent() >= 80.0) continue;
                if (!first) o << ",";
                first = false;
                o << "\n      { \"ruleId\": \"coverage-low\","
                  << " \"level\": \""
                  << (f.percent() < 50 ? "warning" : "note") << "\","
                  << " \"message\": { \"text\": \""
                  << formatPercent(f.percent())
                  << "% covered (" << f.covered << "/" << f.total
                  << ")\" },"
                  << " \"locations\": [{ \"physicalLocation\": { "
                  << "\"artifactLocation\": { \"uri\": \""
                  << jsonEscape(f.path) << "\" } } }]}";
            }
            o << "\n    ]\n";
            o << "  }]\n";
            o << "}\n";
            return llvm::Error::success();
        }

        llvm::Error writeLcov(const std::string& path,
                              const CoverageMap& m) {
            std::ofstream o(path, std::ios::binary | std::ios::trunc);
            if (!o) return err("cannot open " + path + " for writing");
            for (const auto& f : m.files) {
                o << "SF:" << f.path << "\n";
                o << "LH:" << f.covered << "\n";
                o << "LF:" << f.total << "\n";
                o << "end_of_record\n";
            }
            return llvm::Error::success();
        }

        llvm::Error writeConsole(const std::string& path,
                                 const CoverageMap& m) {
            std::ofstream o(path, std::ios::binary | std::ios::trunc);
            if (!o) return err("cannot open " + path + " for writing");
            o << consoleSummary(m) << "\n";
            // Per-file details (sorted by percent ascending so the
            // worst rises to the top).
            std::vector<CoverageFile> sorted = m.files;
            std::stable_sort(sorted.begin(), sorted.end(),
                             [](const CoverageFile& a,
                                const CoverageFile& b) {
                                 return a.percent() < b.percent();
                             });
            for (const auto& f : sorted) {
                o << "  " << f.path << "  "
                  << formatPercent(f.percent()) << "% ("
                  << f.covered << "/" << f.total << ")\n";
            }
            return llvm::Error::success();
        }

    } // namespace

    llvm::Error renderCoverageReports(
        const CoverageMap& m,
        const std::map<std::string, std::string>& reports) {
        for (const auto& [fmt, path] : reports) {
            if (path.empty()) continue;
            llvm::Error e = llvm::Error::success();
            if (fmt == "html")    e = writeHtml(path, m);
            else if (fmt == "sarif") e = writeSarif(path, m);
            else if (fmt == "lcov")  e = writeLcov(path, m);
            else if (fmt == "console") e = writeConsole(path, m);
            else {
                return err("coverage: unknown report format '" +
                           fmt + "' (supported: html, sarif, lcov, "
                           "console)");
            }
            if (e) return e;
        }
        return llvm::Error::success();
    }

} // namespace cajeta::buildtool
