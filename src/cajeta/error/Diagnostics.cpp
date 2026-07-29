#include "Diagnostics.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <vector>

#include <antlr4-runtime.h>

#include "cajeta/compile/CajetaModule.h"
#include "cajeta/error/DiagnosticEngine.h"

namespace cajeta {

    // The file a located diagnostic belongs to. `activeModule` is set only
    // inside synthesis / template-instantiation re-entry (where it names the
    // TRIGGER's file, which is what such an error must be attributed to), so
    // it wins. Ordinary codegen leaves it null and sets currentCodegenModule
    // instead (Method.cpp's CodegenFrame) — without that fallback every
    // codegen-time located error reports an empty file.
    static std::string diagnosticFile() {
        if (auto m = CajetaModule::getActiveModule()) return m->getSourcePath();
        if (auto m = CajetaModule::getCurrentCodegenModule()) return m->getSourcePath();
        return {};
    }

    void reportOrThrow(int line, int column,
                       const std::string& errorId, const std::string& message) {
        DiagnosticEngine* eng = DiagnosticEngine::active();
        if (eng && eng->collectsErrors()) {
            eng->report("error", errorId, message, diagnosticFile(), line, column);
        } else {
            throw locatedException(line, column, message, errorId);
        }
    }

    void reportOrThrow(antlr4::Token* token,
                       const std::string& errorId, const std::string& message) {
        int line = token ? static_cast<int>(token->getLine()) : -1;
        int column = token ? static_cast<int>(token->getCharPositionInLine()) + 1 : -1;
        reportOrThrow(line, column, errorId, message);
    }

    Exception locatedException(antlr4::Token* token,
                               const std::string& message,
                               const std::string& errorId) {
        // ANTLR lines are 1-based; columns are 0-based — normalize to 1-based.
        int line = token ? static_cast<int>(token->getLine()) : -1;
        int column = token ? static_cast<int>(token->getCharPositionInLine()) + 1 : -1;
        return Exception(message, errorId, diagnosticFile(), line, column);
    }

    Exception locatedException(int line, int column,
                               const std::string& message,
                               const std::string& errorId) {
        return Exception(message, errorId, diagnosticFile(), line, column);
    }

    namespace {
        // Minimal RFC 8259 string escaping for the NDJSON diagnostic payload.
        std::string jsonEscape(const std::string& s) {
            std::string o;
            o.reserve(s.size() + 8);
            for (unsigned char c : s) {
                switch (c) {
                    case '"':  o += "\\\""; break;
                    case '\\': o += "\\\\"; break;
                    case '\n': o += "\\n";  break;
                    case '\r': o += "\\r";  break;
                    case '\t': o += "\\t";  break;
                    case '\b': o += "\\b";  break;
                    case '\f': o += "\\f";  break;
                    default:
                        if (c < 0x20) {          // other control chars → \u00XX
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                            o += buf;
                        } else {
                            o += static_cast<char>(c);
                        }
                }
            }
            return o;
        }

        // Emit a "key": value pair for a string field, or JSON null when empty.
        void strOrNull(std::string& out, const char* key, const std::string& v) {
            out += "\"";
            out += key;
            out += "\":";
            if (v.empty()) out += "null";
            else { out += "\""; out += jsonEscape(v); out += "\""; }
        }
    } // namespace

    void emitJsonDiagnostic(const std::string& severity,
                            const std::string& code,
                            const std::string& message,
                            const std::string& file,
                            int line,
                            int column) {
        std::string o = "{";
        strOrNull(o, "severity", severity); o += ",";
        strOrNull(o, "code", code);         o += ",";
        strOrNull(o, "message", message);   o += ",";
        strOrNull(o, "file", file);         o += ",";
        o += "\"line\":";   o += (line   > 0 ? std::to_string(line)   : "null"); o += ",";
        o += "\"column\":"; o += (column > 0 ? std::to_string(column) : "null");
        o += "}\n";
        // stderr, unbuffered-friendly: one write per line so consumers reading
        // the pipe see each diagnostic as it is produced.
        std::cerr << o << std::flush;
    }

    namespace {
        bool g_jsonProgress = false;
    }

    void setJsonProgressEnabled(bool enabled) { g_jsonProgress = enabled; }
    bool jsonProgressEnabled() { return g_jsonProgress; }

    void emitJsonProgress(const std::string& phase,
                          const std::string& state,
                          const std::string& label,
                          long long elapsedMs) {
        std::string o = "{\"kind\":\"progress\",";
        strOrNull(o, "phase", phase); o += ",";
        strOrNull(o, "state", state); o += ",";
        strOrNull(o, "label", label);
        if (elapsedMs >= 0) {
            o += ",\"elapsedMs\":";
            o += std::to_string(elapsedMs);
        }
        o += "}\n";
        // Same stream + flush discipline as emitJsonDiagnostic: one write per
        // line so the IDE's pipe reader sees a phase the moment it starts,
        // instead of when the process exits.
        std::cerr << o << std::flush;
    }

    void emitJsonCacheHit(const std::string& artifact) {
        std::string o = "{\"kind\":\"cache\",";
        strOrNull(o, "state", "hit");     o += ",";
        strOrNull(o, "artifact", artifact);
        o += "}\n";
        std::cerr << o << std::flush;
    }

    ProgressPhase::ProgressPhase(std::string phase, std::string label)
        : phase(std::move(phase)),
          label(std::move(label)),
          active(jsonProgressEnabled()),
          startedAt(std::chrono::steady_clock::now()) {
        if (active) emitJsonProgress(this->phase, "start", this->label);
    }

    ProgressPhase::~ProgressPhase() {
        if (!active) return;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt).count();
        // Destructor may run while an exception (a fatal diagnostic) unwinds —
        // the phase still gets closed, so the IDE never leaves a phase node
        // spinning forever. emitJsonProgress does not throw.
        emitJsonProgress(phase, "finish", label, static_cast<long long>(ms));
    }

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
