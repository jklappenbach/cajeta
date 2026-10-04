#include "DiagnosticEngine.h"

#include <algorithm>
#include <climits>
#include <iostream>
#include <set>
#include <tuple>

#include "Diagnostics.h"

namespace cajeta {

    namespace {
        thread_local DiagnosticEngine* g_active = nullptr;
    }

    DiagnosticEngine* DiagnosticEngine::active() { return g_active; }
    void DiagnosticEngine::setActive(DiagnosticEngine* e) { g_active = e; }

    void DiagnosticEngine::report(const std::string& severity,
                                  const std::string& code,
                                  const std::string& message,
                                  const std::string& file,
                                  int line, int column,
                                  const std::string& origin) {
        if (suppressed_) return;
        if (severity == "error") errorSeen_ = true;
        diags_.push_back(CollectedDiagnostic{severity, code, message, file, line, column,
                                             origin.empty() ? "project" : origin});
    }

    namespace {
        int originRank(const std::string& origin) {
            if (origin == "stdlib") return 2;
            if (origin == "dependency") return 1;
            return 0;
        }
    }

    std::vector<CollectedDiagnostic> DiagnosticEngine::finalize() const {
        std::vector<CollectedDiagnostic> all;
        std::set<std::tuple<std::string, int, int, std::string>> seen;
        for (const auto& d : diags_) {
            if (seen.insert(std::make_tuple(d.file, d.line, d.column, d.code)).second) {
                all.push_back(d);
            }
        }
        std::stable_sort(all.begin(), all.end(),
            [](const CollectedDiagnostic& a, const CollectedDiagnostic& b) {
                int ar = originRank(a.origin);
                int br = originRank(b.origin);
                if (ar != br) return ar < br;
                if (a.file != b.file) return a.file < b.file;
                int al = a.line <= 0 ? INT_MAX : a.line;
                int bl = b.line <= 0 ? INT_MAX : b.line;
                if (al != bl) return al < bl;
                return a.column < b.column;
            });
        std::vector<CollectedDiagnostic> out;
        size_t i = 0;
        while (i < all.size()) {
            size_t j = i;
            while (j < all.size() && originRank(all[j].origin) == originRank(all[i].origin)) ++j;
            size_t n = j - i;
            size_t keep = n > static_cast<size_t>(CAP) ? static_cast<size_t>(CAP) : n;
            out.insert(out.end(), all.begin() + i, all.begin() + i + keep);
            if (n > keep) {
                CollectedDiagnostic note;
                note.severity = "note";
                note.origin = all[i].origin;
                note.message = "…and " + std::to_string(n - keep) + " more "
                    + (note.origin == "project" ? std::string() : note.origin + " ")
                    + "diagnostics";
                out.push_back(note);
            }
            i = j;
        }
        return out;
    }

    void DiagnosticEngine::emit(bool json) const {
        const bool all = showAllDiagnosticOrigins();
        for (const auto& d : finalize()) {
            if (!all && d.origin != "project" && d.severity != "error") continue;
            if (json) {
                emitJsonDiagnostic(d.severity, d.code, d.message, d.file, d.line, d.column,
                                   d.origin);
            } else if (d.line > 0) {
                std::cerr << "cajeta: " << d.file << ":" << d.line << ":" << d.column
                          << ": " << d.code << ": " << d.message << "\n";
            } else {
                std::cerr << "cajeta: " << d.code << ": " << d.message << "\n";
            }
        }
    }

} // namespace cajeta
