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
                                  int line, int column) {
        if (suppressed_) return;
        if (severity == "error") errorSeen_ = true;
        diags_.push_back(CollectedDiagnostic{severity, code, message, file, line, column});
    }

    std::vector<CollectedDiagnostic> DiagnosticEngine::finalize() const {
        // Dedup by (file, line, column, code) — first occurrence wins.
        std::vector<CollectedDiagnostic> out;
        std::set<std::tuple<std::string, int, int, std::string>> seen;
        for (const auto& d : diags_) {
            if (seen.insert(std::make_tuple(d.file, d.line, d.column, d.code)).second) {
                out.push_back(d);
            }
        }
        // Emit order: by span (file, then line, then column). Unlocated (line <= 0)
        // sort after located within the same file so precise errors lead.
        std::stable_sort(out.begin(), out.end(),
            [](const CollectedDiagnostic& a, const CollectedDiagnostic& b) {
                if (a.file != b.file) return a.file < b.file;
                int al = a.line <= 0 ? INT_MAX : a.line;
                int bl = b.line <= 0 ? INT_MAX : b.line;
                if (al != bl) return al < bl;
                return a.column < b.column;
            });
        // Cap.
        if (static_cast<int>(out.size()) > CAP) {
            int extra = static_cast<int>(out.size()) - CAP;
            out.resize(CAP);
            CollectedDiagnostic note;
            note.severity = "note";
            note.message = "…and " + std::to_string(extra) + " more diagnostics";
            out.push_back(note);
        }
        return out;
    }

    void DiagnosticEngine::emit(bool json) const {
        for (const auto& d : finalize()) {
            if (json) {
                emitJsonDiagnostic(d.severity, d.code, d.message, d.file, d.line, d.column);
            } else if (d.line > 0) {
                std::cerr << "cajeta: " << d.file << ":" << d.line << ":" << d.column
                          << ": " << d.code << ": " << d.message << "\n";
            } else {
                std::cerr << "cajeta: " << d.code << ": " << d.message << "\n";
            }
        }
    }

} // namespace cajeta
